#include "WiFiSetupServer.h"
#include "WiFiSetupServerHTML.h"
#include <ArduinoJson.h>

const char* WiFiSetupServer::AP_SSID = "M5Dial-Setup";
const char* WiFiSetupServer::AP_PASSWORD = "screenbee12345";
const IPAddress WiFiSetupServer::AP_IP(192, 168, 4, 1);
const IPAddress WiFiSetupServer::AP_GATEWAY(192, 168, 4, 1);
const IPAddress WiFiSetupServer::AP_SUBNET(255, 255, 255, 0);
const unsigned long WiFiSetupServer::AP_AUTO_RESET_MS = 120000;  // 2 minutes

// Set by the WiFi.onEvent() handler below (ensureWiFiEventLogging(), a
// free function, not a WiFiSetupServer member - see its own comment for
// why it's registered lazily as a plain global callback) and read by
// tick(). A bare global rather than an instance member because there's
// only ever one WiFiSetupServer (wifiSetupServer in main.cpp) and the
// event callback has no way to reach a specific instance - matches this
// codebase's existing convention for other single-instance globals
// (configManager, mqttClient). Reset to false at the top of every AP-mode
// entry (startAP()/start()'s AP branch) so a *second* setup-mode session
// within the same boot doesn't inherit a stale "someone connected"
// verdict from the first one - true state as of the current AP session,
// not "ever, across this whole boot".
static bool s_apClientConnected = false;

WiFiSetupServer::WiFiSetupServer(IDisplay& display, IConfigStorage& config)
  : display_(display), config_(config), webServer_(nullptr), running_(false), apMode_(false) {}

WiFiSetupServer::~WiFiSetupServer() {
  stop();
}

// Registers WiFi.onEvent() lazily, not from this class's constructor -
// wifiSetupServer is a *global* object, so its constructor runs during C++
// static initialization, before Arduino's own setup() (and therefore
// before the WiFi/event subsystem is actually initialized) - touching
// WiFi.onEvent() that early is invalid timing and a real suspect for a
// crash found live on hardware (an early, unexplained RTC_SW_SYS_RST
// reset with no application log reached yet). Guarded so it only runs
// once even though start()/startAP() can both call this.
void ensureWiFiEventLogging() {
  static bool registered = false;
  if (registered) return;
  registered = true;
  WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
    if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
      Serial.println("[WiFiSetupServer] AP: station connected");
      s_apClientConnected = true;
    } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
      Serial.println("[WiFiSetupServer] AP: station disconnected");
    } else if (event == ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED) {
      Serial.println("[WiFiSetupServer] AP: IP assigned to station");
    }
  });
}

bool WiFiSetupServer::start() {
  Serial.println("[WiFiSetupServer] start() entry");
  if (running_) return true;
  if (!LittleFS.begin(false)) return false;
  Serial.println("[WiFiSetupServer] LittleFS ok, calling ensureWiFiEventLogging()");
  ensureWiFiEventLogging();
  Serial.println("[WiFiSetupServer] ensureWiFiEventLogging() done");

  bool wifiConnected = (WiFi.status() == WL_CONNECTED);
  apMode_ = !wifiConnected;
  Serial.printf("[WiFiSetupServer] apMode_=%d\n", apMode_);

  if (apMode_) {
    Serial.println("[WiFiSetupServer] calling WiFi.mode(WIFI_AP_STA)");
    WiFi.mode(WIFI_AP_STA);
    Serial.println("[WiFiSetupServer] calling WiFi.softAPConfig()");
    WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
    Serial.println("[WiFiSetupServer] calling WiFi.softAP()");
    if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) {
      Serial.println("[WiFiSetupServer] WiFi.softAP() failed");
      return false;
    }
    Serial.println("[WiFiSetupServer] WiFi.softAP() succeeded");
    s_apClientConnected = false;
    apEnteredAtMs_ = millis();
    lastRenderedCountdownState_ = -1;
  } else {
    WiFi.mode(WIFI_AP_STA);
  }

  Serial.println("[WiFiSetupServer] creating WebServer");
  webServer_ = new WebServer(80);
  webServer_->on("/", HTTP_GET, [this]() { handleRoot(); });
  webServer_->on("/api/scan", HTTP_GET, [this]() { handleWiFiScan(); });
  webServer_->on("/api/wifi", HTTP_POST, [this]() { handleWiFiConfigure(); });
  webServer_->on("/api/mqtt", HTTP_POST, [this]() { handleMqttConfigure(); });
  webServer_->on("/api/mqtt-test", HTTP_POST, [this]() { handleMqttTest(); });
  webServer_->onNotFound([this]() { handleNotFound(); });
  Serial.println("[WiFiSetupServer] calling webServer_->begin()");
  webServer_->begin();
  Serial.println("[WiFiSetupServer] webServer_->begin() done");

  if (apMode_) {
    Serial.println("[WiFiSetupServer] calling showSetupScreenAP()");
    int initialSecondsLeft = (int)(AP_AUTO_RESET_MS / 1000);
    showSetupScreenAP(initialSecondsLeft, false);
    lastRenderedCountdownState_ = initialSecondsLeft;
  } else {
    showSetupScreenSTA();
  }
  Serial.println("[WiFiSetupServer] start() complete");

  running_ = true;
  return true;
}

bool WiFiSetupServer::startAP() {
  if (running_) stop();
  if (!LittleFS.begin(false)) return false;
  ensureWiFiEventLogging();

  apMode_ = true;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) return false;
  s_apClientConnected = false;
  apEnteredAtMs_ = millis();

  webServer_ = new WebServer(80);
  webServer_->on("/", HTTP_GET, [this]() { handleRoot(); });
  webServer_->on("/api/scan", HTTP_GET, [this]() { handleWiFiScan(); });
  webServer_->on("/api/wifi", HTTP_POST, [this]() { handleWiFiConfigure(); });
  webServer_->on("/api/mqtt", HTTP_POST, [this]() { handleMqttConfigure(); });
  webServer_->on("/api/mqtt-test", HTTP_POST, [this]() { handleMqttTest(); });
  webServer_->onNotFound([this]() { handleNotFound(); });
  webServer_->begin();

  int initialSecondsLeft = (int)(AP_AUTO_RESET_MS / 1000);
  showSetupScreenAP(initialSecondsLeft, false);
  lastRenderedCountdownState_ = initialSecondsLeft;
  running_ = true;
  return true;
}

void WiFiSetupServer::stop() {
  if (!running_) return;
  if (webServer_) {
    webServer_->stop();
    delete webServer_;
    webServer_ = nullptr;
  }
  WiFi.softAPdisconnect(true);
  running_ = false;
}

void WiFiSetupServer::handleClient() {
  if (running_ && webServer_) webServer_->handleClient();
}

bool WiFiSetupServer::tick() {
  if (!running_ || !apMode_) return false;

  if (s_apClientConnected) {
    if (lastRenderedCountdownState_ != -2) {
      showSetupScreenAP(-1, true);
      lastRenderedCountdownState_ = -2;
    }
    return false;
  }

  unsigned long elapsed = millis() - apEnteredAtMs_;
  if (elapsed >= AP_AUTO_RESET_MS) return true;

  int secondsLeft = (int)((AP_AUTO_RESET_MS - elapsed) / 1000);
  if (secondsLeft != lastRenderedCountdownState_) {
    showSetupScreenAP(secondsLeft, false);
    lastRenderedCountdownState_ = secondsLeft;
  }
  return false;
}

// clientConnected true overrides secondsLeft entirely (once a client has
// connected during this AP session, the countdown is dead regardless of
// its actual remaining value - see tick()'s own comment). secondsLeft -1
// with clientConnected false suppresses the status line altogether (the
// two no-argument call sites this used to have before tick() existed -
// showSetupScreenSTA() has no equivalent status line at all, e.g.).
void WiFiSetupServer::showSetupScreenAP(int secondsLeft, bool clientConnected) {
  String statusLine;
  if (clientConnected) {
    statusLine = "1 client connected";
  } else if (secondsLeft >= 0) {
    statusLine = "Auto-reset in " + String(secondsLeft) + "s";
  }

  display_.showLines({
    "SCREENBEE SETUP",
    "",
    "1. Connect to WiFi:",
    "SSID: " + String(AP_SSID),
    "Pass: " + String(AP_PASSWORD),
    "",
    "2. Open browser:",
    "http://192.168.4.1",
    "",
    statusLine,
    "Press button to cancel",
  });
}

void WiFiSetupServer::showSetupScreenSTA() {
  String ip = WiFi.localIP().toString();
  String ssid = WiFi.SSID();
  display_.showLines({
    "SCREENBEE SETUP",
    "",
    "Your WiFi: " + ssid,
    "Device IP: " + ip,
    "",
    "Open browser:",
    "http://" + ip,
    "",
    "Hold Push (3s) for AP",
  });
}

void WiFiSetupServer::handleRoot() {
  Serial.println("[WiFiSetupServer] GET /");
  webServer_->send(200, "text/html", String(WIFI_SETUP_HTML));
  Serial.println("[WiFiSetupServer] GET / sent");
}

void WiFiSetupServer::handleWiFiScan() {
  Serial.println("[WiFiSetupServer] GET /api/scan - starting scanNetworks()");
  String result = scanNetworks();
  Serial.println("[WiFiSetupServer] scanNetworks() done, sending response");
  sendJSONResponse(true, result);
}

void WiFiSetupServer::handleWiFiConfigure() {
  Serial.println("[WiFiSetupServer] POST /api/wifi");
  if (!webServer_->hasArg("ssid") || !webServer_->hasArg("password")) {
    sendJSONResponse(false, "Missing SSID or password");
    return;
  }
  String ssid = webServer_->arg("ssid");
  String password = webServer_->arg("password");

  if (saveWiFiCredentials(ssid, password)) {
    configuredSSID_ = ssid;
    sendJSONResponse(true, "WiFi credentials saved! Restart to connect.");
  } else {
    sendJSONResponse(false, "Failed to save credentials");
  }
}

void WiFiSetupServer::handleMqttConfigure() {
  if (!webServer_->hasArg("protocol") || !webServer_->hasArg("host") || !webServer_->hasArg("port")) {
    sendJSONResponse(false, "Missing MQTT protocol, host, or port");
    return;
  }
  String protocol = webServer_->arg("protocol");
  String host = webServer_->arg("host");
  int port = webServer_->arg("port").toInt();
  String username = webServer_->arg("username");
  String password = webServer_->arg("password");

  if (port <= 0 || port > 65535) {
    sendJSONResponse(false, "Invalid port number");
    return;
  }

  if (saveMqttConfig(protocol, host, port, username, password)) {
    sendJSONResponse(true, "MQTT configuration saved!");
  } else {
    sendJSONResponse(false, "Failed to save MQTT configuration");
  }
}

void WiFiSetupServer::handleMqttTest() {
  if (!webServer_->hasArg("protocol") || !webServer_->hasArg("host") || !webServer_->hasArg("port")) {
    sendJSONResponse(false, "Missing MQTT protocol, host, or port");
    return;
  }
  String protocol = webServer_->arg("protocol");
  String host = webServer_->arg("host");
  int port = webServer_->arg("port").toInt();
  String username = webServer_->arg("username");
  String password = webServer_->arg("password");

  String result = testMqttConnection(protocol, host, port, username, password);
  if (result.indexOf("SUCCESS") >= 0) {
    sendJSONResponse(true, result);
  } else {
    sendJSONResponse(false, result);
  }
}

void WiFiSetupServer::handleNotFound() {
  Serial.print("[WiFiSetupServer] 404 for: ");
  Serial.println(webServer_->uri());
  webServer_->send(404, "text/plain", "Not Found");
}

String WiFiSetupServer::scanNetworks() {
  int n = WiFi.scanNetworks();
  JsonDocument doc;
  JsonArray networks = doc["networks"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject network = networks.add<JsonObject>();
    network["ssid"] = WiFi.SSID(i);
    network["rssi"] = WiFi.RSSI(i);
    network["encryption"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "open" : "encrypted";
  }
  String output;
  serializeJson(doc, output);
  return output;
}

bool WiFiSetupServer::saveWiFiCredentials(const String& ssid, const String& password) {
  WiFiCredentials creds;
  creds.ssid = ssid;
  creds.password = password;
  creds.timestamp = millis();
  return config_.saveWiFiCredentials(creds);
}

bool WiFiSetupServer::saveMqttConfig(const String& protocol, const String& host, int port, const String& username, const String& password) {
  WiFiCredentials creds;
  if (!config_.loadWiFiCredentials(creds)) {
    creds.ssid = "";
    creds.password = "";
    creds.timestamp = millis();
  }
  creds.mqttProtocol = protocol;
  creds.mqttHost = host;
  creds.mqttPort = port;
  creds.mqttUsername = username;
  creds.mqttPassword = password;
  return config_.saveWiFiCredentials(creds);
}

String WiFiSetupServer::testMqttConnection(const String& protocol, const String& host, int port, const String& username, const String& password) {
  if (WiFi.status() != WL_CONNECTED) {
    return "FAILED: No WiFi connection. Connect to WiFi first.";
  }

  WiFiClient wifiClient;
  PubSubClient mqttClient(wifiClient);
  mqttClient.setServer(host.c_str(), port);
  mqttClient.setSocketTimeout(5);

  String clientId = "ScreenBeeTest_" + String(random(0xffff), HEX);

  bool connected = false;
  unsigned long startTime = millis();
  unsigned long timeout = 5000;

  if (username.length() > 0 && password.length() > 0) {
    connected = mqttClient.connect(clientId.c_str(), username.c_str(), password.c_str());
  } else {
    connected = mqttClient.connect(clientId.c_str());
  }

  while (!connected && (millis() - startTime) < timeout) {
    mqttClient.loop();
    delay(100);
    if (mqttClient.connected()) {
      connected = true;
      break;
    }
    if (mqttClient.state() == MQTT_CONNECT_FAILED) break;
  }

  unsigned long duration = millis() - startTime;

  if (connected) {
    String testTopic = "screenbee/test/" + clientId;
    String testMessage = "Test message from ScreenBee";
    if (mqttClient.publish(testTopic.c_str(), testMessage.c_str(), false)) {
      mqttClient.disconnect();
      return "SUCCESS: Connected to MQTT broker in " + String(duration) + "ms. Test publish successful.";
    } else {
      mqttClient.disconnect();
      return "PARTIAL: Connected to MQTT broker in " + String(duration) + "ms, but publish failed.";
    }
  } else {
    String stateStr;
    switch (mqttClient.state()) {
      case MQTT_CONNECTION_TIMEOUT: stateStr = "Connection timeout"; break;
      case MQTT_CONNECTION_LOST: stateStr = "Connection lost"; break;
      case MQTT_CONNECT_FAILED: stateStr = "Connection failed"; break;
      case MQTT_DISCONNECTED: stateStr = "Disconnected"; break;
      case MQTT_CONNECT_BAD_PROTOCOL: stateStr = "Bad protocol"; break;
      case MQTT_CONNECT_BAD_CLIENT_ID: stateStr = "Bad client ID"; break;
      case MQTT_CONNECT_UNAVAILABLE: stateStr = "Server unavailable"; break;
      case MQTT_CONNECT_BAD_CREDENTIALS: stateStr = "Bad credentials"; break;
      case MQTT_CONNECT_UNAUTHORIZED: stateStr = "Unauthorized"; break;
      default: stateStr = "Unknown error (" + String(mqttClient.state()) + ")"; break;
    }
    return "FAILED: " + stateStr + " after " + String(duration) + "ms";
  }
}

void WiFiSetupServer::sendJSONResponse(bool success, const String& message) {
  JsonDocument doc;
  doc["success"] = success;
  doc["message"] = message;
  String output;
  serializeJson(doc, output);
  webServer_->send(success ? 200 : 400, "application/json", output);
}
