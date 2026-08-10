// Checkpoint 5: HIL test-interface HTTP endpoints (TestInterfaceServer -
// project upload, forced screen switch, snapshot, topic-value readback),
// on top of checkpoint 4's live MQTT-driven redraw and checkpoint 3's WiFi
// provisioning (WiFiSetupServer, a lean WiFi+MQTT-only AP configurator -
// see its own header for why the full dual-purpose UnifiedConfigurator
// wasn't ported as-is) + MQTT hello/status (MqttClient, ported ~verbatim
// from MqttEPaperDisplay2 - LWT/status is baked into its own connect(),
// nothing extra needed here). No saved credentials, or a failed connect,
// enters AP setup mode automatically; holding the push button for 3s while
// connected re-enters it. Once connected, subscribes to every topic the
// loaded project's objects are bound to, renders screen 0, and starts the
// test interface server; from then on, onMqttMessage() keeps whatever's on
// screen live - see its own comment and docs/device-contract.md (designer
// repo) §3/§4/§6 for the rendering-parity/MQTT/testInterface contract this
// ports from the e-paper firmware.
#include <M5Dial.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "ClippedCanvas16.h"
#include "project/ProjectLoader.h"
#include "project/ColorScreenRenderer.h"
#include "test_project.h"
#include "test_icon_bmp.h"
#include "DeviceInfo.h"
#include "config/ConfigManager.h"
#include "M5DisplayAdapter.h"
#include "WiFiSetupServer.h"
#include "MqttClient.h"
#include "TestInterfaceServer.h"
#include "DeployManager.h"
#ifdef JTAG_DEBUG_TEST
#include "project/ProjectInstaller.h"
#include "test_deflate_zip.h"
#endif
#ifdef POWER_STATE_TEST
#include <esp_sleep.h>
#endif

ClippedCanvas16 canvas(240, 240);
ProjectLoader projectLoader;
ColorScreenRenderer* screenRenderer = nullptr;

ConfigManager configManager;
M5DisplayAdapter displayAdapter;
WiFiSetupServer wifiSetupServer(displayAdapter, configManager);
MqttClient mqttClient;
TestInterfaceServer testInterfaceServer(&canvas);
DeployManager deployManager;

bool setupModeActive = false;

// Entering setup mode used to be a plain 3s button hold - easy to trigger
// by accident (e.g. an absent-minded long press while wearing the device)
// and, once in, the only way back out was a physical power-cycle (nothing
// ever cleared setupModeActive - see the button-press-exit handling in
// loop() below for the other half of that fix). Replaced 2026-08-10 with
// a deliberate compound gesture: hold the button, then rotate the dial
// left by SETUP_GESTURE_CLICKS clicks, then right by the same amount
// (relative to wherever the left phase ended, not back to the exact
// start) - nobody bumps a button and spins a dial in a specific two-phase
// pattern by accident. M5Dial.Encoder.read() returns raw quadrature
// increments, not "clicks" - SETUP_GESTURE_CLICK_INCREMENTS is an
// estimate (this dial's detents felt like ~3-4 increments each during
// testing), tune it if the gesture triggers too early/late on real
// hardware. Which physical rotation direction counts as "left" (negative
// vs positive delta) hasn't been hardware-verified either - if the
// gesture never registers, try swapping which phase requires a negative
// vs positive delta below.
const long SETUP_GESTURE_CLICK_INCREMENTS = 4;
const long SETUP_GESTURE_CLICKS = 3;
const long SETUP_GESTURE_THRESHOLD = SETUP_GESTURE_CLICK_INCREMENTS * SETUP_GESTURE_CLICKS;
long setupGestureStartEncoderPos = 0;
long setupGestureMinDelta = 0;
bool setupGestureWentLeft = false;

// Auto-sleep (backlight + WiFi off) after IDLE_SLEEP_MS with no button
// press or encoder movement - see enterIdleSleep()/wakeFromIdleSleep()'s
// own comments. 2 minutes (2026-08-10, tuned down from an initial 20 -
// confirmed working end-to-end on real hardware first, then shortened
// per feedback that 20 was too long in practice).
const unsigned long IDLE_SLEEP_MS = 2UL * 60UL * 1000UL;
unsigned long lastActivityMs = 0;
bool deviceSleeping = false;
long lastEncoderPosForIdle = 0;

// Which screen is currently on the display - only ever 0 for now (no
// button/encoder navigation wired up yet), but onMqttMessage() already
// needs to know it to decide whether an incoming value affects what's
// visible right now, so this is tracked from the start rather than
// hardcoded, matching Application::currentScreenIndex_ in the e-paper
// firmware.
int currentScreenIndex = 0;
// Mirrors Application::topicsSubscribed_ - subscribeToAllTopics() is a
// no-op once this is true, so a project reload can force a clean
// resubscribe by clearing it first (not done anywhere yet - only one
// project load path exists today).
bool topicsSubscribed = false;

// Always overwrites (not "if missing") - these are bring-up fixtures that
// change as object types get ported, not real persisted data; an
// "if missing" guard bit us once already (a fixed test_project.h change
// silently had no effect because the stale LittleFS copy from a previous
// flash was never replaced).
void writeTestProject() {
  File f = LittleFS.open("/test_project.json", "w");
  if (!f) {
    Serial.println("[M5Dial] Failed to open /test_project.json for writing");
    return;
  }
  f.print(TEST_PROJECT_JSON);
  f.close();
  Serial.println("[M5Dial] Wrote /test_project.json");
}

void writeTestAssets() {
  LittleFS.mkdir("/assets");
  File f = LittleFS.open("/assets/test-icon.bmp", "w");
  if (!f) {
    Serial.println("[M5Dial] Failed to open /assets/test-icon.bmp for writing");
    return;
  }
  f.write(TEST_ICON_BMP, sizeof(TEST_ICON_BMP));
  f.close();
  Serial.printf("[M5Dial] Wrote /assets/test-icon.bmp (%d bytes)\n", (int)sizeof(TEST_ICON_BMP));
}

void blitCanvasToDisplay() {
  M5Dial.Display.startWrite();
  M5Dial.Display.setSwapBytes(true);
  M5Dial.Display.pushImage(0, 0, 240, 240, canvas.getBuffer());
  M5Dial.Display.endWrite();
}

// An object bound to a JSON subtopic stores "<topic>#<path>" in its own
// properties.topic (see ProjectLoader::getTopicValue) - only the real
// topic before "#" is ever valid to subscribe to at the broker, and MQTT
// itself never delivers the "#path" suffix on an incoming message, so
// every comparison against a live topic name needs this stripped first.
// Mirrors Application::stripJsonPath() in the e-paper firmware exactly.
String stripJsonPath(const String& topicOrPath) {
  int hashIndex = topicOrPath.indexOf('#');
  return hashIndex == -1 ? topicOrPath : topicOrPath.substring(0, hashIndex);
}

// Recurses into obj.children for parity with the e-paper firmware's
// collectTopics() (tab-control -> panel nesting) even though nothing in
// the M5 Dial DDF's supportedObjectTypes includes tab-control/panel yet -
// children is always empty here today, but this stays correct for free
// once that changes instead of silently missing nested topics again (see
// hil/combinations.js's own header comment for the exact bug this caused
// on the e-paper target the first time tab-control shipped).
void collectTopics(const std::vector<ScreenObject>& objects, std::vector<String>& outTopics) {
  for (const auto& obj : objects) {
    if (!obj.properties.topic.isEmpty()) {
      String realTopic = stripJsonPath(obj.properties.topic);
      bool exists = false;
      for (const auto& existing : outTopics) {
        if (existing == realTopic) {
          exists = true;
          break;
        }
      }
      if (!exists) outTopics.push_back(realTopic);
    }
    if (!obj.children.empty()) collectTopics(obj.children, outTopics);
  }
}

// Subscribes to every topic used anywhere in the project (not just the
// current screen) in one pass, same as the e-paper firmware - cheaper than
// resubscribing on every screen switch, and MqttClient queues subscribe
// calls made before a real connection exists for replay on the next
// connect/reconnect, so call order relative to mqttClient.connect() doesn't
// matter.
void subscribeToAllTopics() {
  if (topicsSubscribed) return;
  if (!projectLoader.isLoaded()) return;

  const ProjectConfig& project = projectLoader.getProject();
  std::vector<String> topics;
  for (const auto& screen : project.screens) {
    collectTopics(screen.objects, topics);
  }
  if (!topics.empty()) mqttClient.subscribeToKeys(topics);
  topicsSubscribed = true;
}

// Whether any object in this screen (or a nested child) is bound to `topic`
// - decides if an incoming value is worth a redraw at all, since only the
// currently-displayed screen is ever visible.
bool screenUsesTopic(const std::vector<ScreenObject>& objects, const String& topic) {
  for (const auto& obj : objects) {
    if (!obj.properties.topic.isEmpty() && stripJsonPath(obj.properties.topic) == topic) return true;
    if (!obj.children.empty() && screenUsesTopic(obj.children, topic)) return true;
  }
  return false;
}

// The core of Checkpoint 4: an incoming MQTT value updates the project's
// topic-value cache and, if the current screen actually displays it,
// triggers a full renderScreen() + blit. No partial-update path exists
// here (unlike the e-paper firmware's renderObjectsPartial) - a color LCD
// has no e-paper ghosting/refresh-time concern to optimize around, so a
// full redraw on every relevant change is simple and cheap enough.
// Set by onMqttMessage(), consumed by loop() right after mqttClient.loop()
// returns - see onMqttMessage()'s own comment for why the actual deploy
// handling can't happen from inside the MQTT callback itself.
bool pendingDeploy = false;
String pendingDeployPayload;

void onMqttMessage(const String& topic, const String& payload) {
  // Device-level control topic, not a project-data topic - route it to
  // DeployManager instead of falling through to setTopicValue() below,
  // which would otherwise try to treat the deploy JSON payload as a
  // literal displayed value for a (nonexistent) object bound to this
  // topic string.
  //
  // Deliberately deferred, not handled inline: this callback runs nested
  // inside PubSubClient's own loop()/packet-parsing call stack, and
  // DeployManager::handleDeployMessage() calls mqttClient.publish()
  // roughly a hundred times in a row (one per download-progress percent).
  // PubSubClient's publish() and its incoming-packet parsing share one
  // internal buffer - found 2026-08-10 on real hardware: every publish()
  // call here looked successful (returned/logged normally) but a
  // third-party MQTT subscriber watching the broker directly never saw a
  // single deploy-status message arrive, consistent with those outgoing
  // packets getting corrupted by writing into a buffer the still-in-
  // progress incoming-message processing also owns. Setting a flag here
  // and doing the real work from loop() - after mqttClient.loop() has
  // fully returned and PubSubClient's own call stack has fully unwound -
  // sidesteps the reentrancy entirely.
  String deployTopic = String(TOPIC_PREFIX) + "/" + mqttClient.getClientId() + "/deploy";
  if (topic == deployTopic) {
    pendingDeployPayload = payload;
    pendingDeploy = true;
    return;
  }

  if (!projectLoader.isLoaded()) return;
  projectLoader.setTopicValue(topic, payload);

  const ProjectConfig& project = projectLoader.getProject();
  if (currentScreenIndex < 0 || currentScreenIndex >= (int)project.screens.size()) return;
  const Screen& screen = project.screens[currentScreenIndex];
  if (!screenUsesTopic(screen.objects, topic)) return;

  if (!screenRenderer) return;
  if (!screenRenderer->renderScreen(currentScreenIndex)) {
    Serial.println("[M5Dial] renderScreen() failed during MQTT-triggered redraw");
    return;
  }
  blitCanvasToDisplay();
  Serial.printf("[M5Dial] Redrew screen %d after MQTT update on \"%s\"\n", currentScreenIndex, topic.c_str());
}

void loadAndRenderProject() {
  // A project uploaded via TestInterfaceServer's POST /api/project lands at
  // /PROJECT/project.json (ProjectLoader::loadProject's own default path,
  // matching the e-paper firmware's convention) and takes priority over the
  // built-in bring-up fixture once one exists - it persists across reboots
  // (LittleFS, not wiped by writeTestProject()/writeTestAssets()), same as
  // on the e-paper side.
  String projectPath = LittleFS.exists("/PROJECT/project.json") ? "/PROJECT/project.json" : "/test_project.json";
  if (!projectLoader.loadProject(projectPath)) {
    Serial.printf("[M5Dial] Failed to load project from %s\n", projectPath.c_str());
    return;
  }
  Serial.printf("[M5Dial] Loaded project \"%s\" with %d screen(s) from %s\n",
                projectLoader.getProject().name.c_str(), projectLoader.getProject().screens.size(), projectPath.c_str());

  currentScreenIndex = 0;
  mqttClient.clearSubscriptions();
  topicsSubscribed = false;
  subscribeToAllTopics();

  if (!screenRenderer) screenRenderer = new ColorScreenRenderer(projectLoader, &canvas);
  if (!screenRenderer->renderScreen(currentScreenIndex)) {
    Serial.println("[M5Dial] renderScreen(0) failed");
    return;
  }
  blitCanvasToDisplay();
  Serial.println("[M5Dial] Rendered screen 0 to display");
}

// POST /api/screen's handler (see TestInterfaceServer::setScreenSwitchHandler)
// - forces a full render of an arbitrary screen index without reloading the
// project or rebooting, so a HIL orchestrator can exercise every screen from
// one upload cheaply. Always a full render, never anything that would depend
// on whatever was on screen before, so repeated snapshots are reproducible.
bool handleTestScreenSwitch(int index) {
  if (!projectLoader.isLoaded()) return false;
  const ProjectConfig& project = projectLoader.getProject();
  if (index < 0 || index >= (int)project.screens.size()) return false;
  if (!screenRenderer) return false;

  currentScreenIndex = index;
  if (!screenRenderer->renderScreen(index)) return false;
  blitCanvasToDisplay();
  return true;
}

void publishHello() {
  JsonDocument doc;
  doc["deviceId"] = DEVICE_ID;
  doc["firmwareVersion"] = FIRMWARE_VERSION;
  // ddfVersion+url - see DeviceInfo.h's DDF_VERSION comment. Without
  // these, device-scan-section.tsx deliberately treats this as "older/
  // simpler firmware that doesn't announce a DDF" and skips it entirely -
  // found 2026-08-10 as why this device never showed up under "Announced
  // Devices" despite hello/status already working fine for the Deploy
  // dialog (which only needs deviceId, a lower bar than DDF discovery).
  doc["ddfVersion"] = DDF_VERSION;
  doc["url"] = "http://" + WiFi.localIP().toString() + "/ddf.zip";
  String payload;
  serializeJson(doc, payload);
  mqttClient.publish(String(TOPIC_PREFIX) + "/" + mqttClient.getClientId() + "/hello", payload, true);
  Serial.println("[M5Dial] Published hello");
}

// screenbee/<clientId>/deploy-status - see DeployManager.h's own header
// comment for the full flow this reports on. Not retained (matches
// docs/device-contract.md §4's contract text: unlike deploy/hello/status,
// deploy-status is never called "retained" there) - it's a point-in-time
// progress event stream, not a persistent flag a late subscriber should
// see stale.
void publishDeployStatus(const String& deployId, const String& state, const String& error, int percent) {
  JsonDocument doc;
  doc["deployId"] = deployId;
  doc["state"] = state;
  if (!error.isEmpty()) doc["error"] = error;
  if (percent >= 0) doc["percent"] = percent;
  String payload;
  serializeJson(doc, payload);
  bool ok = mqttClient.publish(String(TOPIC_PREFIX) + "/" + mqttClient.getClientId() + "/deploy-status", payload, false);
  Serial.printf("[M5Dial] publish(deploy-status) -> %s: %s\n", ok ? "true" : "FALSE", payload.c_str());
}

// Clears the retained screenbee/<clientId>/deploy trigger - see
// DeployManager.h's own comment for why this is the actual fix for the
// reboot loop found 2026-08-10, not just tidiness.
void clearDeployTrigger() {
  mqttClient.publish(String(TOPIC_PREFIX) + "/" + mqttClient.getClientId() + "/deploy", "", true);
}

void setupMQTT() {
  WiFiCredentials creds;
  if (configManager.loadWiFiCredentials(creds)) {
    mqttClient.configure(creds.mqttHost, static_cast<uint16_t>(creds.mqttPort), creds.mqttUsername, creds.mqttPassword);
  } else {
    mqttClient.configure("", 0, "", "");
  }
  deployManager.setPublishStatusHandler(publishDeployStatus);
  deployManager.setClearTriggerHandler(clearDeployTrigger);
  // Subscribed once here, not from the connected-callback - MqttClient::
  // connect() already re-subscribes everything in its own subscribedTopics_
  // on every reconnect (see its own comment), same mechanism
  // subscribeToAllTopics() already relies on for project-bound topics.
  // NOTE: this subscription gets silently wiped moments later by
  // loadAndRenderProject()'s clearSubscriptions() call (see setupWiFi()'s
  // own comment on its re-subscribe right after that call, right below in
  // this same function's caller) - kept here too, not just there, so
  // connect()'s subscribedTopics_-replay-on-reconnect still has the right
  // topic queued from the very first connection attempt onward, before
  // loadAndRenderProject() has even run once.
  mqttClient.subscribe(String(TOPIC_PREFIX) + "/" + mqttClient.getClientId() + "/deploy");
  mqttClient.setConnectedCallback([]() { publishHello(); });
  mqttClient.setCallback(onMqttMessage);
  mqttClient.connect();
}

// Try saved credentials; on success configure MQTT + render the test
// project; on failure (or no saved credentials at all) enter AP setup
// mode - matches Application::setupWiFi()'s logic in the e-paper firmware,
// just without the "Hold Button 0" display line duplicated here (shown by
// WiFiSetupServer's own status screens instead).
void setupWiFi() {
  WiFiCredentials creds;
  bool haveCreds = configManager.loadWiFiCredentials(creds) && !creds.ssid.isEmpty();

  if (haveCreds) {
    displayAdapter.showLines({"Connecting to WiFi...", creds.ssid});
    WiFi.mode(WIFI_STA);
    WiFi.begin(creds.ssid.c_str(), creds.password.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
      delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
      creds.lastIP = WiFi.localIP().toString();
      creds.lastRSSI = WiFi.RSSI();
      configManager.saveWiFiCredentials(creds);
      Serial.printf("[M5Dial] WiFi connected: %s (%d dBm)\n", creds.lastIP.c_str(), creds.lastRSSI);
      setupModeActive = false;
      Serial.printf("[M5Dial] MQTT config: host=\"%s\" port=%d user=\"%s\"\n",
                     creds.mqttHost.c_str(), creds.mqttPort, creds.mqttUsername.c_str());
      setupMQTT();
      loadAndRenderProject();
      // Re-subscribe to the deploy topic - loadAndRenderProject() just
      // called mqttClient.clearSubscriptions() (to drop the *previous*
      // project's own topic subscriptions before subscribeToAllTopics()
      // adds the new project's) - clearSubscriptions() has no concept of
      // "device-level" vs "project-content" topics, so it silently wiped
      // the deploy subscription setupMQTT() had just established a moment
      // earlier too. Found 2026-08-10 as the real reason a *live*-
      // published deploy trigger so often never reached this device at
      // all: it was only ever actually listening on `deploy` for the
      // brief window between setupMQTT()'s subscribe and this line - long
      // enough to usually catch a *retained* trigger already waiting
      // (arrives essentially with the SUBACK), but not a genuinely live
      // one published later while the device sat there, subscribed to
      // nothing, looking completely normal otherwise (a manual reset
      // "fixed" it because that's exactly what re-opens the same brief
      // catch window with the trigger still retained on the broker).
      mqttClient.subscribe(String(TOPIC_PREFIX) + "/" + mqttClient.getClientId() + "/deploy");

      testInterfaceServer.setScreenSwitchHandler(handleTestScreenSwitch);
      testInterfaceServer.setGetTopicValueHandler([](const String& topic) {
        return projectLoader.getTopicValue(topic);
      });
      if (testInterfaceServer.start()) {
        Serial.printf("[M5Dial] Test interface server listening on http://%s/\n", creds.lastIP.c_str());
      } else {
        Serial.println("[M5Dial] Test interface server failed to start");
      }
      return;
    }
    Serial.println("[M5Dial] WiFi connect failed, entering setup mode");
  }

  Serial.println("[M5Dial] No saved WiFi credentials, calling wifiSetupServer.start()");
  setupModeActive = true;
  wifiSetupServer.start();
  Serial.println("[M5Dial] wifiSetupServer.start() returned");
}

#ifdef POWER_STATE_TEST
// env:m5dial_powertest only (-DPOWER_STATE_TEST) - cycles through display
// (and, since 2026-08-10, radio) power states forever, each held long
// enough to get a stable multimeter reading. Built because the actual
// goal turned out to be "turn the screen off (and ideally the radio too)
// at night in the camper", not battery-life-driven deep sleep - deep
// sleep is a poor fit anyway (the rotary encoder is wired to GPIO 40/41,
// outside the ESP32-S3's RTC GPIO range (0-21), so it can't serve as a
// deep-sleep wake source; only the push button, on GPIO 21, could).
//
// States 1-3 deliberately never touch WiFi, isolating what the display
// alone costs (measured 2026-08-10: 40 / 25 / 19 mA @ 12V):
//   1. Normal        - full brightness, panel awake.
//   2. Backlight off  - panel driver still fully powered/active.
//   3. Panel sleep    - Display.sleep() (backlight off + GC9A01 SLPIN).
//
// States 4-6 connect WiFi for real (reusing saved credentials, same as
// normal operation) to answer a follow-up question states 1-3 can't:
// leaving WiFi/MQTT running (as normal operation always does) while just
// dimming the screen doesn't address radio-off/EMF concerns at all - only
// an explicit WiFi.mode(WIFI_OFF) does, and that's worth knowing the real
// cost and reconnect latency of before building it into real behavior:
//   4. WiFi connected, normal display - real "both on" baseline, and logs
//      how long the initial connect actually took.
//   5. WiFi connected, backlight off - what "just dim it" really costs in
//      real operation (radio stays fully active the whole time).
//   6. WiFi off (explicit disconnect + mode(WIFI_OFF)), backlight off,
//      CPU fully awake (no sleep - encoder/button stay instantly
//      responsive) - the actual candidate for "dark and radio-quiet".
//      Reconnects WiFi again at the end of this state and logs exactly
//      how long that took.
//
// State 7, light sleep, runs LAST, after WiFi is already off from state 6
// - esp_light_sleep_start() (CPU clock-gated, not powered down like deep
// sleep - can still wake from any GPIO, encoder included, unlike deep
// sleep) was found 2026-08-10 to drop the USB-CDC connection on this
// hardware (the host-side serial monitor goes silent through it and
// doesn't resume without manually reopening the port) - running it last
// keeps every earlier state's log output watchable live across a whole
// cycle without that interruption landing mid-cycle. No deep sleep state
// at all - the encoder can't wake it, and power draw wasn't the actual
// problem being solved.
void runPowerStateTest() {
  const unsigned long HOLD_MS = 20000;

  // ConfigManager::loadWiFiCredentials() reads from LittleFS - normal
  // setup() mounts it before anything WiFi-related runs, but this test is
  // called from setup() *before* that point (deliberately, to skip the
  // rest of normal init entirely). Without this, every credentials load
  // below silently failed and states 5-7 were skipped every cycle -
  // found 2026-08-10 on the very first real hardware run of this test.
  if (!LittleFS.begin(true)) {
    Serial.println("[PowerTest] LittleFS mount failed - radio states will be skipped");
  }

  auto connectWiFiForTest = [](const char* label) -> bool {
    WiFiCredentials creds;
    if (!configManager.loadWiFiCredentials(creds) || creds.ssid.isEmpty()) {
      Serial.println("[PowerTest] No saved WiFi credentials - skipping radio states");
      return false;
    }
    unsigned long start = millis();
    WiFi.mode(WIFI_STA);
    WiFi.begin(creds.ssid.c_str(), creds.password.c_str());
    while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
      delay(100);
    }
    unsigned long elapsedMs = millis() - start;
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[PowerTest] %s: WiFi connected in %lums\n", label, elapsedMs);
      return true;
    }
    Serial.printf("[PowerTest] %s: WiFi did NOT connect within %lums\n", label, elapsedMs);
    return false;
  };

  while (true) {
    Serial.println("[PowerTest] === 1. NORMAL (backlight full, panel awake, WiFi off) ===");
    M5Dial.Display.wakeup();
    M5Dial.Display.setBrightness(150);
    delay(HOLD_MS);

    Serial.println("[PowerTest] === 2. BACKLIGHT OFF (panel still awake, WiFi off) ===");
    M5Dial.Display.setBrightness(0);
    delay(HOLD_MS);

    Serial.println("[PowerTest] === 3. PANEL SLEEP (backlight off + GC9A01 SLPIN, WiFi off) ===");
    M5Dial.Display.sleep();
    delay(HOLD_MS);
    M5Dial.Display.wakeup();

    Serial.println("[PowerTest] === 4. WiFi CONNECTED, NORMAL display ===");
    M5Dial.Display.setBrightness(150);
    bool wifiUp = connectWiFiForTest("State 4 connect");
    delay(HOLD_MS);

    if (wifiUp) {
      Serial.println("[PowerTest] === 5. WiFi CONNECTED, BACKLIGHT OFF ===");
      M5Dial.Display.setBrightness(0);
      delay(HOLD_MS);

      Serial.println("[PowerTest] === 6. WiFi OFF, BACKLIGHT OFF, CPU awake ===");
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(HOLD_MS);

      unsigned long reconnectStart = millis();
      connectWiFiForTest("State 6 wake-reconnect");
      Serial.printf("[PowerTest] Total dark-to-data-flowing time: %lums\n", millis() - reconnectStart);
    }

    // Light sleep last - found 2026-08-10 that it drops the USB-CDC
    // connection on this hardware (the host-side serial monitor goes
    // silent and doesn't resume without manually reopening it), so
    // running it last keeps every earlier state's log output watchable
    // live without an interruption to work around mid-cycle.
    Serial.println("[PowerTest] === 7. LIGHT SLEEP (CPU clock-gated, WiFi off) ===");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    M5Dial.Display.setBrightness(0);
    esp_sleep_enable_timer_wakeup(HOLD_MS * 1000ULL);
    esp_light_sleep_start();
    Serial.println("[PowerTest] Woke from light sleep, cycle repeating");
  }
}
#endif

#ifdef JTAG_DEBUG_TEST
// JTAG-only (env:m5dial_debug + -DJTAG_DEBUG_TEST): reproduces the
// DEFLATE-extraction crash without WiFi/HTTP in the loop at all, so a
// live JTAG session only has to stay stable through boot + this one call,
// not through an active WiFi radio too (which proved USB-flaky under
// combined load in an earlier debugging session - see platformio.ini's
// env:m5dial_debug comment). Runs before setupWiFi() specifically so
// there's no radio activity yet when the crash happens.
void runJtagDebugTest() {
  File f = LittleFS.open("/jtag_test.zip", "w");
  if (!f) {
    Serial.println("[JtagDebugTest] Failed to open /jtag_test.zip for writing");
    return;
  }
  f.write(TEST_DEFLATE_ZIP, sizeof(TEST_DEFLATE_ZIP));
  f.close();
  Serial.printf("[JtagDebugTest] Wrote /jtag_test.zip (%d bytes)\n", (int)sizeof(TEST_DEFLATE_ZIP));

  Serial.println("[JtagDebugTest] Calling peekProjectDeviceId - set breakpoints/watchpoints now");
  String deviceId = ProjectInstaller::peekProjectDeviceId("/jtag_test.zip");
  Serial.printf("[JtagDebugTest] peekProjectDeviceId() -> \"%s\"\n", deviceId.c_str());
}
#endif

// Idle-sleep: backlight only, after IDLE_SLEEP_MS of no button/encoder
// activity, restored instantly on the next press or turn. WiFi/MQTT
// deliberately stay up the whole time (2026-08-10, changed from an
// earlier version that also turned the radio off) - the device needs to
// stay reachable for MQTT-driven redraws and, concretely, MQTT self-
// deploy: the `deploy` trigger is only delivered while actually
// connected, so a radio-off sleep meant a deploy just sat unprocessed
// until something happened to wake the device, which looked from the
// designer's side like the deploy had silently done nothing. A screen
// that's dark but still fully live (WiFi/MQTT/HTTP all normal, redraws
// still happen into the framebuffer even while unlit) trades a small
// amount of extra power for actually being remotely usable - see
// hil/README.md's M5 Dial section (designer repo) / this file's own
// earlier history for the power figures if that tradeoff ever needs
// revisiting. No ESP32 sleep mode involved either way - the CPU stays
// fully awake throughout, so M5Dial.update()/button/encoder polling in
// loop() never stops.
void enterIdleSleep() {
  Serial.println("[M5Dial] Idle timeout - backlight off (WiFi/MQTT stay up)");
  deviceSleeping = true;
  M5Dial.Display.setBrightness(0);
}

void wakeFromIdleSleep() {
  Serial.println("[M5Dial] Activity detected - backlight on");
  deviceSleeping = false;
  M5Dial.Display.setBrightness(150);
}

void setup() {
  auto cfg = M5.config();
  M5Dial.begin(cfg, /*enableEncoder=*/true, /*enableRFID=*/false);
  M5Dial.Display.setBrightness(150);

  Serial.begin(115200);
  Serial.println("[M5Dial] Checkpoint 5 - HIL test-interface endpoints");

#ifdef POWER_STATE_TEST
  runPowerStateTest();  // never returns
  return;
#endif

  if (!LittleFS.begin(true)) {
    Serial.println("[M5Dial] LittleFS mount failed");
    return;
  }
  writeTestProject();
  writeTestAssets();

#ifdef JTAG_DEBUG_TEST
  runJtagDebugTest();
#endif

  Serial.println("[M5Dial] Calling configManager.begin()");
  configManager.begin();
  Serial.println("[M5Dial] Calling setupWiFi()");
  setupWiFi();
  Serial.println("[M5Dial] setupWiFi() returned, setup() complete");

  lastActivityMs = millis();
  lastEncoderPosForIdle = M5Dial.Encoder.read();
}

void loop() {
  M5Dial.update();

  if (setupModeActive) {
    wifiSetupServer.handleClient();
    if (wifiSetupServer.hasWiFiCredentials() && !WiFi.isConnected()) {
      // Credentials just configured via the setup form - restart to
      // apply them cleanly through the normal setupWiFi() path, same as
      // the e-paper firmware.
      Serial.println("[M5Dial] hasWiFiCredentials() true, restarting to apply");
      delay(1000);
      ESP.restart();
    }
    // A plain button press (not another 3s hold) cancels setup mode and
    // returns to normal operation - previously the only way out was a
    // physical power-cycle/reset, since nothing here ever cleared
    // setupModeActive. Goes through the same clean-reboot path every
    // other setup-mode exit already uses (see the credentials-configured
    // branch above and TestInterfaceServer::stop()'s own comment on why
    // this codebase prefers a fresh boot over tearing down/rebuilding
    // live AP/WebServer/WiFi state mid-request) rather than trying to
    // resume live.
    if (M5Dial.BtnA.wasPressed()) {
      Serial.println("[M5Dial] Button pressed in setup mode, cancelling and restarting");
      displayAdapter.showLines({"Cancelled.", "Rebooting..."});
      delay(1000);
      ESP.restart();
    }
    // No one connected to the AP within 2 minutes - see
    // WiFiSetupServer::tick()'s own comment. Only fires in AP mode (a
    // no-op elsewhere), so this is safe to call unconditionally here.
    if (wifiSetupServer.tick()) {
      Serial.println("[M5Dial] No AP client connected within the timeout, restarting");
      displayAdapter.showLines({"No connection.", "Rebooting..."});
      delay(1000);
      ESP.restart();
    }
    delay(10);
    return;
  }

  // Auto-sleep tracking - see enterIdleSleep()/wakeFromIdleSleep()'s own
  // comments. "Activity" is deliberately limited to a button press or an
  // encoder movement, not a background MQTT-driven redraw - presence of
  // new data isn't presence of a person. Unlike this feature's first
  // version, sleeping no longer skips mqttClient.loop()/
  // testInterfaceServer.handleClient() below at all - WiFi/MQTT/HTTP stay
  // fully live through a "sleep", only the backlight actually changes.
  long currentEncoderPosForIdle = M5Dial.Encoder.read();
  bool userActivity = M5Dial.BtnA.wasPressed() || (currentEncoderPosForIdle != lastEncoderPosForIdle);
  lastEncoderPosForIdle = currentEncoderPosForIdle;

  if (userActivity) {
    lastActivityMs = millis();
    if (deviceSleeping) wakeFromIdleSleep();
  } else if (!deviceSleeping && millis() - lastActivityMs >= IDLE_SLEEP_MS) {
    enterIdleSleep();
  }

  mqttClient.loop();

  // Handled here, not inside onMqttMessage() itself - see that function's
  // own comment for why. mqttClient.loop() above has fully returned by
  // this point, so DeployManager's own publish() calls are safe.
  if (pendingDeploy) {
    pendingDeploy = false;
    deployManager.handleDeployMessage(pendingDeployPayload);
  }

  testInterfaceServer.handleClient();

  // Hold the push button, then rotate the dial left (~3 clicks) then
  // right (~3 clicks, measured from wherever the left phase ended) to
  // re-enter setup mode - see this gesture's own header comment (top of
  // file) for why it replaced a plain 3s hold.
  if (M5Dial.BtnA.wasPressed()) {
    setupGestureStartEncoderPos = M5Dial.Encoder.read();
    setupGestureMinDelta = 0;
    setupGestureWentLeft = false;
  }

  if (M5Dial.BtnA.isPressed()) {
    long delta = M5Dial.Encoder.read() - setupGestureStartEncoderPos;
    if (delta < setupGestureMinDelta) setupGestureMinDelta = delta;

    if (!setupGestureWentLeft && setupGestureMinDelta <= -SETUP_GESTURE_THRESHOLD) {
      setupGestureWentLeft = true;
      Serial.println("[M5Dial] Setup gesture: left phase detected, turn right to confirm");
    }

    if (setupGestureWentLeft && (delta - setupGestureMinDelta) >= SETUP_GESTURE_THRESHOLD) {
      Serial.println("[M5Dial] Setup gesture completed, entering AP setup mode");
      // Must release port 80 before wifiSetupServer.startAP() binds its
      // own WebServer there - see TestInterfaceServer::stop()'s own
      // comment for what silently broke without this.
      testInterfaceServer.stop();
      setupModeActive = true;
      wifiSetupServer.startAP();
    }
  } else if (M5Dial.BtnA.wasReleased()) {
    setupGestureMinDelta = 0;
    setupGestureWentLeft = false;
  }

  delay(10);
}
