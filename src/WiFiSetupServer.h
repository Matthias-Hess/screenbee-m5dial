#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include "interfaces/IDisplay.h"
#include "interfaces/IConfigStorage.h"

// Lean counterpart of MqttEPaperDisplay2's UnifiedConfigurator - WiFi +
// MQTT setup only, no project upload (that's checkpoint 4's job, tied to
// a ProjectInstaller that doesn't exist for this device yet - see the
// project history for why this was deliberately split rather than
// porting the full dual-purpose class now). Same AP/STA auto-detect
// behavior, same /api/scan, /api/wifi, /api/mqtt, /api/mqtt-test routes.
class WiFiSetupServer {
public:
  WiFiSetupServer(IDisplay& display, IConfigStorage& config);
  ~WiFiSetupServer();

  // Auto-detects: STA mode (web server on the existing connection) if
  // WiFi is already connected, AP mode otherwise.
  bool start();
  // Force AP mode - for a long button-press while already connected.
  bool startAP();
  void stop();
  bool isRunning() const { return running_; }
  void handleClient();
  bool hasWiFiCredentials() const { return !configuredSSID_.isEmpty(); }

private:
  IDisplay& display_;
  IConfigStorage& config_;
  WebServer* webServer_;
  bool running_;
  bool apMode_;

  String configuredSSID_;

  static const char* AP_SSID;
  static const char* AP_PASSWORD;
  static const IPAddress AP_IP;
  static const IPAddress AP_GATEWAY;
  static const IPAddress AP_SUBNET;

  void handleRoot();
  void handleWiFiScan();
  void handleWiFiConfigure();
  void handleMqttConfigure();
  void handleMqttTest();
  void handleNotFound();

  String scanNetworks();
  bool saveWiFiCredentials(const String& ssid, const String& password);
  bool saveMqttConfig(const String& protocol, const String& host, int port, const String& username, const String& password);
  String testMqttConnection(const String& protocol, const String& host, int port, const String& username, const String& password);

  void showSetupScreenAP();
  void showSetupScreenSTA();
  void sendJSONResponse(bool success, const String& message);
};
