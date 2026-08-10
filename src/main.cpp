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
#ifdef JTAG_DEBUG_TEST
#include "project/ProjectInstaller.h"
#include "test_deflate_zip.h"
#endif

ClippedCanvas16 canvas(240, 240);
ProjectLoader projectLoader;
ColorScreenRenderer* screenRenderer = nullptr;

ConfigManager configManager;
M5DisplayAdapter displayAdapter;
WiFiSetupServer wifiSetupServer(displayAdapter, configManager);
MqttClient mqttClient;
TestInterfaceServer testInterfaceServer(&canvas);

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
void onMqttMessage(const String& topic, const String& payload) {
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
  String payload;
  serializeJson(doc, payload);
  mqttClient.publish(String(TOPIC_PREFIX) + "/" + mqttClient.getClientId() + "/hello", payload, true);
  Serial.println("[M5Dial] Published hello");
}

void setupMQTT() {
  WiFiCredentials creds;
  if (configManager.loadWiFiCredentials(creds)) {
    mqttClient.configure(creds.mqttHost, static_cast<uint16_t>(creds.mqttPort), creds.mqttUsername, creds.mqttPassword);
  } else {
    mqttClient.configure("", 0, "", "");
  }
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

void setup() {
  auto cfg = M5.config();
  M5Dial.begin(cfg, /*enableEncoder=*/true, /*enableRFID=*/false);
  M5Dial.Display.setBrightness(150);

  Serial.begin(115200);
  Serial.println("[M5Dial] Checkpoint 5 - HIL test-interface endpoints");

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

  mqttClient.loop();
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
