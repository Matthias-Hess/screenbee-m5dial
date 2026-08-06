#pragma once
#include <Arduino.h>

/**
 * WiFi credentials data structure
 */
struct WiFiCredentials {
  String ssid;
  String password;
  String lastIP;
  int lastRSSI;
  unsigned long timestamp;
  
  // MQTT Configuration
  String mqttProtocol;  // "mqtt://" or "mqtts://"
  String mqttHost;
  int mqttPort;
  String mqttUsername;
  String mqttPassword;
  
  WiFiCredentials() : lastRSSI(0), timestamp(0), mqttPort(1883) {
    mqttProtocol = "mqtt://";
    mqttHost = "";
    mqttUsername = "";
    mqttPassword = "";
  }
};

/**
 * Interface for configuration storage
 * Allows storage implementations to be swapped (LittleFS, EEPROM, etc.)
 */
class IConfigStorage {
public:
  virtual ~IConfigStorage() = default;
  
  /**
   * Initialize the storage system
   * @return true if initialization succeeded
   */
  virtual bool begin() = 0;
  
  /**
   * Save WiFi credentials
   * @return true if save succeeded
   */
  virtual bool saveWiFiCredentials(const WiFiCredentials& creds) = 0;
  
  /**
   * Load WiFi credentials
   * @return true if load succeeded
   */
  virtual bool loadWiFiCredentials(WiFiCredentials& creds) = 0;
  
  /**
   * Check if credentials exist
   */
  virtual bool hasWiFiCredentials() = 0;
  
  /**
   * Clear stored credentials
   */
  virtual void clearWiFiCredentials() = 0;
};


