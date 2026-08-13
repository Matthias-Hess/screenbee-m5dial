// Checkpoint 5: HIL test-interface HTTP endpoints (TestInterfaceServer -
// project upload, forced screen switch, snapshot, topic-value readback),
// on top of checkpoint 4's live MQTT-driven redraw and checkpoint 3's WiFi
// provisioning (WiFiSetupServer, a lean WiFi+MQTT-only AP configurator -
// see its own header for why the full dual-purpose UnifiedConfigurator
// wasn't ported as-is) + MQTT hello/status (MqttClient, ported ~verbatim
// from MqttEPaperDisplay2 - LWT/status is baked into its own connect(),
// nothing extra needed here). No saved credentials, or a failed connect,
// enters AP setup mode automatically; a project-configured "Go to Setup
// Mode" button (see ButtonAction's own comment in ProjectTypes.h) or the
// hardcoded rear-button fallback (loop(), bottom of file) re-enters it once
// connected. Once connected, subscribes to every topic the loaded
// project's objects are bound to, renders screen 0, and starts the test
// interface server; from then on, onMqttMessage() keeps whatever's on
// screen live, and touch/button input runs through dispatchButtonAction()
// (SoftwareButton hit-testing, front-button press) - see those functions'
// own comments and docs/device-contract.md (designer repo) §3/§4/§6 for the
// rendering-parity/MQTT/testInterface contract this ports from the e-paper
// firmware.
#include <M5Dial.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "ClippedCanvas16.h"
#include "project/ProjectLoader.h"
#include "project/ColorScreenRenderer.h"
#include "project/ColorAssetLoader.h"
#include "test_project.h"
#include "test_icon_bmp.h"
#include "DeviceInfo.h"
#include "config/ConfigManager.h"
#include "M5DisplayAdapter.h"
#include "WiFiSetupServer.h"
#include "MqttClient.h"
#include "TestInterfaceServer.h"
#include "DeployManager.h"
#include "ScreenNavigatorOverlay.h"
#include "RotaryEncoderReader.h"
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
ScreenNavigatorOverlay screenNavigatorOverlay(&canvas, projectLoader);

bool setupModeActive = false;

// Entering setup mode used to be a plain 3s button hold, then (2026-08-10)
// a hold+rotate-left-then-right compound gesture on the front button - both
// replaced the same day by two cleaner mechanisms once "hold + rotate" was
// wanted for a *different* purpose (adjusting a value on screen), which
// would have collided with the gesture claiming that same input:
//   1. A "Go to Setup Mode" ButtonAction (see ProjectTypes.h's ButtonAction
//      comment) - assignable in the designer to the front button (btn-2)
//      or any SoftwareButton, dispatched like any other button action via
//      dispatchButtonAction() below. Makes setup-mode entry a visible,
//      project-configured feature instead of a hidden gesture.
//   2. A hardcoded rear-button (GPIO0) fallback - see its own handling in
//      loop() - guarantees a way in even with no project loaded or no
//      button configured for it, without depending on any project config.
// setupModeActive is cleared the same way it always was: nothing here
// clears it live, every exit path (credentials configured, button-press
// cancel, AP timeout) goes through ESP.restart() - see loop()'s
// setupModeActive branch.

// Auto-sleep (backlight + WiFi off) after IDLE_SLEEP_MS with no button
// press or encoder movement - see enterIdleSleep()/wakeFromIdleSleep()'s
// own comments. 2 minutes (2026-08-10, tuned down from an initial 20 -
// confirmed working end-to-end on real hardware first, then shortened
// per feedback that 20 was too long in practice).
const unsigned long IDLE_SLEEP_MS = 2UL * 60UL * 1000UL;
unsigned long lastActivityMs = 0;
bool deviceSleeping = false;
long lastEncoderPosForIdle = 0;

// Encoder-detent dispatch for btn-0 ("Rotate Left")/btn-1 ("Rotate Right") -
// the DDF declares these as configurable hardware buttons (same
// name/defaultAction/screen-override machinery as btn-2 "Push", editable
// identically in the designer's Hardware Buttons settings), but nothing
// ever dispatched an action for them - only btn-2 (a real physical switch)
// got wired to dispatchButtonAction() when this session built that
// function. RotaryEncoderReader::read() returns raw quadrature increments,
// not "clicks" - reusing the same ~4-increments-per-detent estimate the old
// (now-removed) setup gesture used. Which physical direction is positive
// vs negative hasn't been hardware-verified - btn-1 is assigned to
// positive delta (guessed as "clockwise = right") here; swap the two
// branches below if left/right come out backwards on real hardware.
const long ENCODER_CLICK_INCREMENTS = 4;
long lastDispatchedEncoderPos = 0;

// Continuous encoder-to-navigator coupling (2026-08-11) - only used while
// btn-0/btn-1 are configured as previous-screen/next-screen (see loop()'s
// continuousNavActive check); ENCODER_CLICK_INCREMENTS-based discrete
// dispatch above still handles every other button-action assignment on
// those two buttons unchanged. referenceEncoderPos/referenceScreenIndex is
// the "zero point" the continuous scroll position is computed from -
// recalibrateEncoderReference() re-zeroes it to (current raw encoder
// position, current screen) every time the screen changes some way OTHER
// than this continuous coupling itself (touch, front button, goto-screen,
// project reload), so the next turn of the dial always starts from
// wherever the dial and the screen actually agree "now" is - never a stale
// point left over from before an unrelated jump.
long referenceEncoderPos = 0;
int referenceScreenIndex = 0;

// Which screen is currently on the display - changed by dispatchButtonAction()
// (next-screen/previous-screen/goto-screen) since 2026-08-10, previously
// only ever 0 (no navigation wired up yet). onMqttMessage() needs to know
// it to decide whether an incoming value affects what's visible right now,
// matching Application::currentScreenIndex_ in the e-paper firmware.
int currentScreenIndex = 0;

// Re-zeroes the continuous-encoder reference point (see its own comment
// above) to "right now" - call this immediately after every currentScreenIndex
// change that ISN'T the continuous coupling itself, so the next dial turn
// always starts from wherever the dial and the screen actually agree "now"
// is.
void recalibrateEncoderReference() {
  referenceEncoderPos = RotaryEncoderReader::read();
  referenceScreenIndex = currentScreenIndex;
}

// Mirrors Application::topicsSubscribed_ - subscribeToAllTopics() is a
// no-op once this is true, so a project reload can force a clean
// resubscribe by clearing it first (not done anywhere yet - only one
// project load path exists today).
bool topicsSubscribed = false;

// Id of the SoftwareButton currently held down by touch (empty = none) -
// drives which button renders pathActive this frame, and which one's
// action fires on release, only if the release point is still over this
// same button (drag-off-to-cancel, see loop()'s touch handling).
String pressedSoftButtonId = "";
// Set at touch-down, read at touch-up (a different loop() iteration, often
// many frames later - can't be a per-frame local) - true if THIS press is
// the one that woke the device from idle sleep, so its eventual release
// won't also fire the button's action. See loop()'s own comment for why
// this only applies to touch, not the front/rear buttons.
bool suppressPressedButtonAction = false;

// Id of the Switch segment currently held down by touch (empty = none) -
// same drag-off-to-cancel semantics as pressedSoftButtonId, but a Switch
// doesn't get a distinct "pressed" visual (unlike SoftwareButton's 3D
// press effect) - nothing redraws on touch-down for a Switch, only on
// touch-up (a tap is a single discrete "select this segment" gesture, see
// this session's 2026-08-12 Switch design discussion for why cycling/
// press-and-hold was explicitly rejected in favor of direct selection).
String pressedSwitchId = "";
int pressedSwitchStateIndex = -1;

// The Switch segment awaiting MQTT confirmation after a tap (empty id = no
// Switch pending). Set on touch-up (after publishing writeValue to
// writeTopic, non-retained), cleared by checkPendingSwitchConfirmation()
// either when a matching value arrives on the read topic within 3s
// (confirmed - the render then just shows it "active" via the normal
// topic-driven getActiveSwitchStateIndex() path, no separate "confirmed"
// bookkeeping needed) or when 3s elapse without one (silent rollback to
// whatever the topic actually says - no error UI, matching the "stiller
// Rückfall" design decision). Deliberately not cleared on screen
// navigation - a genuinely rare edge case (tap a Switch, navigate away,
// navigate back, all within 3s) that isn't worth the extra bookkeeping to
// chase across every screen-switch code path.
String pendingSwitchId = "";
int pendingSwitchStateIndex = -1;
unsigned long pendingSwitchStartMs = 0;
const unsigned long PENDING_SWITCH_TIMEOUT_MS = 3000;

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
  // Passes the current pending-Switch state through (not defaults) - an
  // unrelated topic update arriving during a Switch's up-to-3s pending
  // window would otherwise redraw with pendingSwitchId="" and visibly (if
  // briefly) hide the pending indicator before the next pending-aware
  // redraw restored it. checkPendingSwitchConfirmation() (called from
  // loop() right after mqttClient.loop(), which is what's dispatching this
  // very callback) is still what actually decides whether pending should
  // clear - this redraw just has to stay honest about whatever that
  // decision currently is.
  if (!screenRenderer->renderScreen(currentScreenIndex, "", pendingSwitchId, pendingSwitchStateIndex)) {
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
  recalibrateEncoderReference();
  // Icon paths are screen-id-based, not content-hashed - a redeploy can
  // change what a path's bytes actually are, so the cache must not survive
  // across a reload. See ColorAssetLoader::clearGrayscaleMaskCache()'s own
  // comment.
  ColorAssetLoader::clearGrayscaleMaskCache();
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
  recalibrateEncoderReference();
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

// Shared by the "goto-setup-mode" ButtonAction (dispatchButtonAction()
// below) and the hardcoded rear-button fallback (loop()) - both need the
// exact same teardown/entry sequence TestInterfaceServer::stop()'s own
// comment explains (release port 80 before WiFiSetupServer::startAP()
// binds its own WebServer there).
void enterSetupMode() {
  Serial.println("[M5Dial] Entering setup mode");
  testInterfaceServer.stop();
  setupModeActive = true;
  wifiSetupServer.startAP();
}

// Runs a ButtonAction the same way regardless of source (front button
// btn-2, a SoftwareButton touch) - one dispatch function, not two parallel
// ones, matching the designer's own preview-mode comment (project-
// editor.tsx's handlePreviewButtonAction) that this was always meant to
// mirror. Empty type = nothing configured, silently does nothing (a
// SoftwareButton with no action, or a btn-2 with no default/override, is a
// valid, common configuration - not an error).
void dispatchButtonAction(const ButtonAction& action) {
  if (action.type.isEmpty()) return;
  if (!projectLoader.isLoaded()) return;

  if (action.type == "next-screen" || action.type == "previous-screen") {
    const ProjectConfig& project = projectLoader.getProject();
    int count = (int)project.screens.size();
    if (count == 0) return;
    int delta = (action.type == "next-screen") ? 1 : -1;
    int newIndex = ((currentScreenIndex + delta) % count + count) % count;
    // Deliberately does NOT call handleTestScreenSwitch (which renders AND
    // blits the destination screen without the overlay) - that produced a
    // visible one-frame flash of the bare screen before
    // screenNavigatorOverlay.update() (called later this same loop()
    // iteration) redrew it WITH tablets and blitted again, found live
    // 2026-08-11 as the actual cause of "flickers when jumping between
    // tablets" - on every jump, not just the first. currentScreenIndex is
    // updated directly here; the overlay's update() is the only render+
    // blit for this frame, screen content and tablets composited together
    // in one pass, matching how the touch-press pathActive redraw already
    // avoids the same double-blit for a different case.
    currentScreenIndex = newIndex;
    recalibrateEncoderReference();
    // Only next-screen/previous-screen trigger the animated navigator -
    // goto-screen and every other action type are direct jumps, no
    // orientation aid needed. See ScreenNavigatorOverlay's own header
    // comment.
    screenNavigatorOverlay.trigger(newIndex);
  } else if (action.type == "goto-screen") {
    const ProjectConfig& project = projectLoader.getProject();
    for (int i = 0; i < (int)project.screens.size(); i++) {
      if (project.screens[i].id == action.targetScreenId) {
        handleTestScreenSwitch(i);
        break;
      }
    }
  } else if (action.type == "send-mqtt") {
    mqttClient.publish(action.mqttTopic, action.mqttMessage, false);
  } else if (action.type == "goto-setup-mode") {
    enterSetupMode();
  }
}

// Topmost (highest zIndex) SoftwareButton on the current screen whose rect
// contains (x, y), or nullptr if none - used by loop()'s touch handling.
// Top-level objects only: tab-control/panel nesting isn't in the M5 Dial
// DDF's supportedObjectTypes yet (see docs/device-contract.md §1's own gap
// note), so there's nothing under a SoftwareButton to recurse past, and
// nothing above it either - every object on an M5 Dial screen today is a
// direct child of the screen.
const ScreenObject* findSoftwareButtonAt(int x, int y) {
  if (!projectLoader.isLoaded()) return nullptr;
  const ProjectConfig& project = projectLoader.getProject();
  if (currentScreenIndex < 0 || currentScreenIndex >= (int)project.screens.size()) return nullptr;

  const ScreenObject* best = nullptr;
  for (const auto& obj : project.screens[currentScreenIndex].objects) {
    if (obj.type != "SoftwareButton") continue;
    if (x < obj.x || x >= obj.x + obj.width || y < obj.y || y >= obj.y + obj.height) continue;
    if (!best || obj.zIndex >= best->zIndex) best = &obj;
  }
  return best;
}

// Topmost Switch on the current screen whose rect contains (x, y), same
// top-level-only scope as findSoftwareButtonAt (see its own comment) -
// writes which segment within it via outStateIndex (segment width =
// obj.width / states.size(), same division renderSwitch() uses). Returns
// nullptr (outStateIndex untouched) if no Switch is hit, or the hit Switch
// has zero states (nothing to select).
const ScreenObject* findSwitchSegmentAt(int x, int y, int* outStateIndex) {
  if (!projectLoader.isLoaded()) return nullptr;
  const ProjectConfig& project = projectLoader.getProject();
  if (currentScreenIndex < 0 || currentScreenIndex >= (int)project.screens.size()) return nullptr;

  const ScreenObject* best = nullptr;
  for (const auto& obj : project.screens[currentScreenIndex].objects) {
    if (obj.type != "Switch") continue;
    if (obj.properties.states.empty()) continue;
    if (x < obj.x || x >= obj.x + obj.width || y < obj.y || y >= obj.y + obj.height) continue;
    if (!best || obj.zIndex >= best->zIndex) best = &obj;
  }
  if (!best) return nullptr;

  size_t stateCount = best->properties.states.size();
  int segmentWidth = best->width / (int)stateCount;
  int index = (x - best->x) / segmentWidth;
  if (index >= (int)stateCount) index = (int)stateCount - 1;  // last segment absorbs the remainder, see renderSwitch()
  if (outStateIndex) *outStateIndex = index;
  return best;
}

// True if `obj`'s state at `stateIndex` is currently the one the read
// topic's value actually resolves to - the single check both the
// touch-up confirmation path and checkPendingSwitchConfirmation()'s
// periodic poll need, kept in one place so they can't drift.
bool switchStateMatchesTopic(const ScreenObject& obj, int stateIndex) {
  if (stateIndex < 0 || stateIndex >= (int)obj.properties.states.size()) return false;
  String topicValue = projectLoader.getTopicValue(obj.properties.topic);
  topicValue.trim();
  String readValue = obj.properties.states[stateIndex].readValue;
  readValue.trim();
  return readValue == topicValue;
}

// Called every loop() iteration (after mqttClient.loop(), so a
// just-arrived retained confirmation is already reflected in
// projectLoader's topic-value cache) - clears pendingSwitchId either
// because the read topic now actually confirms pendingStateIndex, or
// because PENDING_SWITCH_TIMEOUT_MS elapsed without that happening. Both
// cases redraw once, immediately, rather than waiting for the next
// MQTT-triggered or touch-triggered redraw - a pending indicator that only
// disappeared "eventually, next time something else redraws" would be a
// worse version of the exact "no feedback" problem the pending indicator
// exists to solve.
void checkPendingSwitchConfirmation() {
  if (pendingSwitchId.isEmpty()) return;
  if (!projectLoader.isLoaded()) return;

  const ProjectConfig& project = projectLoader.getProject();
  if (currentScreenIndex < 0 || currentScreenIndex >= (int)project.screens.size()) return;

  bool confirmed = false;
  for (const auto& obj : project.screens[currentScreenIndex].objects) {
    if (obj.id != pendingSwitchId) continue;
    confirmed = switchStateMatchesTopic(obj, pendingSwitchStateIndex);
    break;
  }
  bool expired = millis() - pendingSwitchStartMs >= PENDING_SWITCH_TIMEOUT_MS;

  if (confirmed || expired) {
    pendingSwitchId = "";
    pendingSwitchStateIndex = -1;
    if (screenRenderer) {
      screenRenderer->renderScreen(currentScreenIndex);
      blitCanvasToDisplay();
    }
  }
}

void setup() {
  auto cfg = M5.config();
  // enableEncoder=false - M5Dial's own ENCODER class silently degrades to
  // poll-only on this board (GPIO 40/41 aren't in its interrupt pin
  // table), which was found live to drop/miscount fast physical clicks.
  // RotaryEncoderReader::begin() below sets up a real interrupt-driven
  // decoder on the same pins instead - see its own header comment.
  M5Dial.begin(cfg, /*enableEncoder=*/false, /*enableRFID=*/false);
  RotaryEncoderReader::begin();
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
  lastEncoderPosForIdle = RotaryEncoderReader::read();
  lastDispatchedEncoderPos = lastEncoderPosForIdle;
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
  // comments. "Activity" is deliberately limited to a button press, an
  // encoder movement, or a touch, not a background MQTT-driven redraw -
  // presence of new data isn't presence of a person. Unlike this feature's
  // first version, sleeping no longer skips mqttClient.loop()/
  // testInterfaceServer.handleClient() below at all - WiFi/MQTT/HTTP stay
  // fully live through a "sleep", only the backlight actually changes.
  auto touchDetail = M5Dial.Touch.getDetail();
  bool touchDown = touchDetail.wasPressed();
  bool touchUp = touchDetail.wasReleased();

  // Captured before wakeFromIdleSleep() runs below: true only when this
  // touch-down is the one waking the device from a dark screen. Stashed in
  // suppressPressedButtonAction (a persistent flag, not a local - touch-up
  // arrives on a later loop() iteration) so a blind tap (you can't see
  // what's under your finger on a screen that was just off) only ever
  // wakes the display, never also fires whatever SoftwareButton happened
  // to be there - see this session's own design discussion for why this
  // deliberately does NOT extend to the front/rear buttons, which have no
  // "blind aim" ambiguity (one fixed physical location each, not several
  // overlapping on-screen targets).
  if (touchDown) suppressPressedButtonAction = deviceSleeping;

  long currentEncoderPosForIdle = RotaryEncoderReader::read();
  bool encoderMoved = (currentEncoderPosForIdle != lastEncoderPosForIdle);
  bool userActivity = M5Dial.BtnA.wasPressed() || touchDown || encoderMoved;
  lastEncoderPosForIdle = currentEncoderPosForIdle;

  if (userActivity) {
    lastActivityMs = millis();
    if (deviceSleeping) wakeFromIdleSleep();
  } else if (!deviceSleeping && millis() - lastActivityMs >= IDLE_SLEEP_MS) {
    enterIdleSleep();
  }

  // Continuous coupling only kicks in when btn-0/btn-1 are configured
  // exactly as previous-screen/next-screen - the DDF/designer lets either
  // be assigned any ButtonAction (send-mqtt, goto-screen, ...), and the
  // "scroll position tracks the dial in real time" feel below only makes
  // sense for plain screen navigation. Anything else on those two buttons
  // keeps the original discrete, once-per-detent dispatch further down.
  // Re-checked every iteration (cheap - two small getButtonAction lookups)
  // since the active screen's own override can change which action is
  // configured without a reboot.
  bool continuousNavActive = projectLoader.isLoaded() &&
      projectLoader.getButtonAction(currentScreenIndex, "btn-0").type == "previous-screen" &&
      projectLoader.getButtonAction(currentScreenIndex, "btn-1").type == "next-screen";

  if (continuousNavActive) {
    // scrollPosition is a real-valued screen index computed straight from
    // the raw encoder position relative to referenceEncoderPos/
    // referenceScreenIndex (re-zeroed by recalibrateEncoderReference()
    // whenever the screen changes some other way - see that function's own
    // comment) - so the tablet strip visibly slides with the dial instead
    // of jumping once per detent. The screen itself switches the moment
    // rounding crosses to a different whole index - roughly half a click,
    // found live 2026-08-11 to feel more directly responsive than waiting
    // for a full detent.
    const ProjectConfig& project = projectLoader.getProject();
    int count = (int)project.screens.size();
    if (encoderMoved && count > 0) {
      // Minus, not plus - turning the dial right visually rotates the
      // tablet strip right (the tablet that was to the left slides into
      // the center), the opposite of "right = toward higher indices" -
      // found live 2026-08-11 to be the expected direction for this
      // feature specifically (unrelated to next-screen/previous-screen's
      // own direction elsewhere, which this bypasses entirely).
      float rawScrollPosition = (float)referenceScreenIndex -
          (float)(currentEncoderPosForIdle - referenceEncoderPos) / (float)ENCODER_CLICK_INCREMENTS;
      // Clamped, not wrapped (2026-08-11, replacing an earlier wrap-around
      // version) - the strip ends at the first/last screen instead of
      // cycling back.
      float scrollPosition = constrain(rawScrollPosition, 0.0f, (float)(count - 1));
      int newIndex = (int)roundf(scrollPosition);
      screenNavigatorOverlay.triggerContinuous(scrollPosition);
      // Deliberately does NOT call recalibrateEncoderReference() here on
      // every tick - doing so would re-zero the reference to the CURRENT
      // raw position every frame, which collapses the fractional part back
      // to exactly newIndex every time and defeats the whole point of
      // tracking a continuous in-between position.
      //
      // BUT: right when clamping actually kicks in (rawScrollPosition !=
      // scrollPosition, i.e. the dial has been turned past an end), the
      // reference DOES need re-zeroing to the clamped position, right now -
      // found live 2026-08-11 as a real bug otherwise: without this,
      // continuing to turn past the end keeps accumulating an invisible
      // backlog in rawScrollPosition alone (scrollPosition itself was
      // already clamped, but referenceEncoderPos/referenceScreenIndex never
      // moved), so reversing direction required winding back through that
      // same backlog - the same number of clicks it took to overshoot -
      // before the strip visibly started moving again. Re-zeroing every
      // frame the dial is still pinned against an end keeps that backlog
      // from ever building up, so reversing responds immediately.
      if (scrollPosition != rawScrollPosition) {
        referenceEncoderPos = currentEncoderPosForIdle;
        referenceScreenIndex = newIndex;
      }
      currentScreenIndex = newIndex;
    }
    // Keeps the discrete dispatch's own bookkeeping from accumulating a
    // large backlog while continuous mode is active, so if the active
    // screen's override switches back to a non-navigation action mid-turn,
    // the discrete path below doesn't replay a burst of stale detents.
    lastDispatchedEncoderPos = currentEncoderPosForIdle;
  } else {
    // Shows the navigator on the very first raw increment, not just once a
    // full detent completes - marks the CURRENT screen's tablet (no switch
    // yet), so you see you're "in" the navigator and where you are before
    // the actual jump happens at the next full click. Also doubles as the
    // overlay's hold-timer reset: trigger() restarts the same countdown
    // ScreenNavigatorOverlay uses to fly the tablets back out, so "tablets
    // disappear after 1.5s with no further increment" falls out of this
    // same call without a second, separate timer - found live 2026-08-11
    // this was nicer than waiting for a full click before showing anything.
    // If this same movement also completes a full detent below,
    // dispatchButtonAction()'s own trigger() call (with the new index) runs
    // right after and simply overwrites which tablet ends up marked - no
    // visible inconsistency, only the final state after both calls in this
    // iteration ever gets rendered.
    if (encoderMoved) {
      screenNavigatorOverlay.trigger(currentScreenIndex);
    }

    // Rotate Left/Right (btn-0/btn-1) dispatch - see ENCODER_CLICK_INCREMENTS'
    // own comment. A while loop, not if, so a fast spin that crosses several
    // detents in one loop() iteration still dispatches once per detent
    // instead of dropping the extras. No idle-wake suppression here (unlike
    // touch) - rotating always does the same predictable thing regardless of
    // whether the screen was visible, same reasoning as the front/rear
    // buttons.
    long encoderDeltaSinceDispatch = currentEncoderPosForIdle - lastDispatchedEncoderPos;
    while (encoderDeltaSinceDispatch >= ENCODER_CLICK_INCREMENTS) {
      dispatchButtonAction(projectLoader.getButtonAction(currentScreenIndex, "btn-1"));
      lastDispatchedEncoderPos += ENCODER_CLICK_INCREMENTS;
      encoderDeltaSinceDispatch -= ENCODER_CLICK_INCREMENTS;
    }
    while (encoderDeltaSinceDispatch <= -ENCODER_CLICK_INCREMENTS) {
      dispatchButtonAction(projectLoader.getButtonAction(currentScreenIndex, "btn-0"));
      lastDispatchedEncoderPos -= ENCODER_CLICK_INCREMENTS;
      encoderDeltaSinceDispatch += ENCODER_CLICK_INCREMENTS;
    }
  }

  // Soft-button touch handling - see findSoftwareButtonAt()'s own comment
  // for the hit-test, ColorScreenRenderer::renderScreen()'s for why a full
  // redraw (not a partial single-object update) is used for the pathNormal/
  // pathActive swap. Release only fires the action if still over the same
  // button it went down on (touchUp's stillHit check) - lets a touch be
  // cancelled by dragging off before lifting.
  if (touchDown) {
    const ScreenObject* hit = findSoftwareButtonAt(touchDetail.x, touchDetail.y);
    pressedSoftButtonId = hit ? hit->id : "";
    if (hit && screenRenderer) {
      screenRenderer->renderScreen(currentScreenIndex, pressedSoftButtonId);
      blitCanvasToDisplay();
    }
  } else if (touchUp && !pressedSoftButtonId.isEmpty()) {
    const ScreenObject* stillHit = findSoftwareButtonAt(touchDetail.x, touchDetail.y);
    bool fire = stillHit && stillHit->id == pressedSoftButtonId && !suppressPressedButtonAction;
    ButtonAction actionToFire = fire ? stillHit->properties.action : ButtonAction();
    pressedSoftButtonId = "";
    if (screenRenderer) {
      screenRenderer->renderScreen(currentScreenIndex);
      blitCanvasToDisplay();
    }
    if (fire) dispatchButtonAction(actionToFire);
  }

  // Switch tap handling - see findSwitchSegmentAt()'s own comment for the
  // hit-test. Unlike SoftwareButton, nothing redraws on touch-down (a
  // Switch has no distinct "pressed" visual) - only touch-up matters, and
  // only if it lands on the same segment the touch-down did (same
  // drag-off-to-cancel semantics as SoftwareButton). A confirmed tap
  // immediately: publishes writeValue to writeTopic (not retained), marks
  // that segment pending, and redraws once so the pending indicator shows
  // up right away rather than waiting for the next unrelated redraw.
  if (touchDown) {
    int stateIndex = -1;
    const ScreenObject* hit = findSwitchSegmentAt(touchDetail.x, touchDetail.y, &stateIndex);
    pressedSwitchId = hit ? hit->id : "";
    pressedSwitchStateIndex = hit ? stateIndex : -1;
  } else if (touchUp && !pressedSwitchId.isEmpty()) {
    int stillIndex = -1;
    const ScreenObject* stillHit = findSwitchSegmentAt(touchDetail.x, touchDetail.y, &stillIndex);
    bool fire = stillHit && stillHit->id == pressedSwitchId && stillIndex == pressedSwitchStateIndex &&
                !suppressPressedButtonAction;

    if (fire) {
      const SwitchState& state = stillHit->properties.states[stillIndex];
      // Already matches the read topic (e.g. a stale tap on the segment
      // that's already active) - nothing to publish or wait for.
      if (!switchStateMatchesTopic(*stillHit, stillIndex)) {
        mqttClient.publish(stillHit->properties.writeTopic, state.writeValue, false);
        pendingSwitchId = stillHit->id;
        pendingSwitchStateIndex = stillIndex;
        pendingSwitchStartMs = millis();
      }
    }

    pressedSwitchId = "";
    pressedSwitchStateIndex = -1;
    if (fire && screenRenderer) {
      screenRenderer->renderScreen(currentScreenIndex, "", pendingSwitchId, pendingSwitchStateIndex);
      blitCanvasToDisplay();
    }
  }

  mqttClient.loop();
  checkPendingSwitchConfirmation();

  // Handled here, not inside onMqttMessage() itself - see that function's
  // own comment for why. mqttClient.loop() above has fully returned by
  // this point, so DeployManager's own publish() calls are safe.
  if (pendingDeploy) {
    pendingDeploy = false;
    deployManager.handleDeployMessage(pendingDeployPayload);
  }

  testInterfaceServer.handleClient();

  // Front button (btn-2 in the DDF's hardwareButtons list) - fires its
  // configured ButtonAction (screen override, else project-wide default,
  // else nothing) on release, same trigger point as SoftwareButton touches
  // above for one consistent rule across every button type.
  if (M5Dial.BtnA.wasReleased()) {
    dispatchButtonAction(projectLoader.getButtonAction(currentScreenIndex, "btn-2"));
  }

  // Rear button (GPIO0, the same pin used for forcing USB download mode) -
  // M5Dial.h doesn't alias this one (only index 0 is exposed, as BtnA),
  // but M5Unified already reads and debounces it every M5Dial.update()
  // call exactly like BtnA (see M5Unified.cpp's board_M5Dial case, bit 1
  // of btn_rawstate_bits) - M5.getButton(1) is the same Button_Class
  // mechanism, just not given a friendly name in this board's own header.
  // Deliberately NOT exposed anywhere in the DDF or designer (see
  // docs/device-contract.md) - a fixed, project-independent way into setup
  // mode so a factory-fresh or misconfigured device (no project loaded, or
  // no button assigned "Go to Setup Mode") is never locked out. Plain
  // press, no hold required - the rear location already makes it hard to
  // trigger by accident, and this is a recovery path where a forgettable
  // hold-timing requirement would cost more than it protects.
  if (M5.getButton(1).wasReleased()) {
    Serial.println("[M5Dial] Rear button pressed, entering setup mode");
    enterSetupMode();
  }

  // Runs every iteration, not just right after trigger() - a no-op
  // (returns false immediately) whenever the overlay isn't animating/
  // holding. Placed last so it reflects a next-screen/previous-screen
  // dispatched earlier in this very same loop() call.
  if (screenNavigatorOverlay.update(screenRenderer, currentScreenIndex)) {
    blitCanvasToDisplay();
  }

  delay(10);
}
