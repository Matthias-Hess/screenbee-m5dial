// Checkpoint 3: WiFi provisioning (WiFiSetupServer, a lean WiFi+MQTT-only
// AP configurator - see its own header for why the full dual-purpose
// UnifiedConfigurator wasn't ported as-is) + MQTT hello/status
// (MqttClient, ported ~verbatim from MqttEPaperDisplay2 - LWT/status is
// baked into its own connect(), nothing extra needed here). No saved
// credentials, or a failed connect, enters AP setup mode automatically;
// holding the push button for 3s while connected re-enters it. Once
// connected, renders the checkpoint 2 test project same as before.
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

ClippedCanvas16 canvas(240, 240);
ProjectLoader projectLoader;
ColorScreenRenderer* screenRenderer = nullptr;

ConfigManager configManager;
M5DisplayAdapter displayAdapter;
WiFiSetupServer wifiSetupServer(displayAdapter, configManager);
MqttClient mqttClient;

bool setupModeActive = false;
const unsigned long BUTTON_LONG_PRESS_MS = 3000;
bool longPressTriggered = false;

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

void loadAndRenderProject() {
  if (!projectLoader.loadProject("/test_project.json")) {
    Serial.println("[M5Dial] Failed to load test project");
    return;
  }
  Serial.printf("[M5Dial] Loaded project \"%s\" with %d screen(s)\n",
                projectLoader.getProject().name.c_str(), projectLoader.getProject().screens.size());

  if (!screenRenderer) screenRenderer = new ColorScreenRenderer(projectLoader, &canvas);
  if (!screenRenderer->renderScreen(0)) {
    Serial.println("[M5Dial] renderScreen(0) failed");
    return;
  }
  blitCanvasToDisplay();
  Serial.println("[M5Dial] Rendered screen 0 to display");
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
      setupMQTT();
      loadAndRenderProject();
      return;
    }
    Serial.println("[M5Dial] WiFi connect failed, entering setup mode");
  }

  Serial.println("[M5Dial] No saved WiFi credentials, calling wifiSetupServer.start()");
  setupModeActive = true;
  wifiSetupServer.start();
  Serial.println("[M5Dial] wifiSetupServer.start() returned");
}

void setup() {
  auto cfg = M5.config();
  M5Dial.begin(cfg, /*enableEncoder=*/true, /*enableRFID=*/false);
  M5Dial.Display.setBrightness(150);

  Serial.begin(115200);
  Serial.println("[M5Dial] Checkpoint 3 - WiFi provisioning + MQTT hello/status");

  if (!LittleFS.begin(true)) {
    Serial.println("[M5Dial] LittleFS mount failed");
    return;
  }
  writeTestProject();
  writeTestAssets();

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
    delay(10);
    return;
  }

  mqttClient.loop();

  // Hold the push button 3s to re-enter setup mode (e.g. to change
  // WiFi/MQTT settings) without a full reflash.
  if (M5Dial.BtnA.pressedFor(BUTTON_LONG_PRESS_MS)) {
    if (!longPressTriggered) {
      longPressTriggered = true;
      Serial.println("[M5Dial] Long press detected, entering AP setup mode");
      setupModeActive = true;
      wifiSetupServer.startAP();
    }
  } else if (M5Dial.BtnA.isReleased()) {
    longPressTriggered = false;
  }

  delay(10);
}
