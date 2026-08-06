#include "ConfigManager.h"

ConfigManager::ConfigManager(const char* configPath) 
  : configPath_(configPath) {}

bool ConfigManager::begin() {
  if (!LittleFS.begin(true)) {  // true = format if mount fails
    return false;
  }
  return true;
}

bool ConfigManager::saveWiFiCredentials(const WiFiCredentials& creds) {
  // Create JSON document
  JsonDocument doc;
  doc["ssid"] = creds.ssid;
  doc["password"] = creds.password;  // Save password (needed for reconnection)
  doc["lastIP"] = creds.lastIP;
  doc["lastRSSI"] = creds.lastRSSI;
  doc["timestamp"] = creds.timestamp;
  
  // MQTT Configuration
  doc["mqttProtocol"] = creds.mqttProtocol;
  doc["mqttHost"] = creds.mqttHost;
  doc["mqttPort"] = creds.mqttPort;
  doc["mqttUsername"] = creds.mqttUsername;
  doc["mqttPassword"] = creds.mqttPassword;
  
  // Open file for writing
  File configFile = LittleFS.open(configPath_, "w");
  if (!configFile) {
    return false;
  }
  
  // Serialize JSON to file
  if (serializeJson(doc, configFile) == 0) {
    configFile.close();
    return false;
  }
  
  configFile.close();
  return true;
}

bool ConfigManager::loadWiFiCredentials(WiFiCredentials& creds) {
  if (!LittleFS.exists(configPath_)) {
    return false;
  }
  
  File configFile = LittleFS.open(configPath_, "r");
  if (!configFile) {
    return false;
  }
  
  // Parse JSON
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, configFile);
  configFile.close();
  
  if (error) {
    return false;
  }
  
  // Read WiFi values
  creds.ssid = doc["ssid"] | "";
  creds.password = doc["password"] | "";
  creds.lastIP = doc["lastIP"] | "";
  creds.lastRSSI = doc["lastRSSI"] | 0;
  creds.timestamp = doc["timestamp"] | 0;
  
  // Read MQTT values (with defaults)
  creds.mqttProtocol = doc["mqttProtocol"] | "mqtt://";
  creds.mqttHost = doc["mqttHost"] | "";
  creds.mqttPort = doc["mqttPort"] | 1883;
  creds.mqttUsername = doc["mqttUsername"] | "";
  creds.mqttPassword = doc["mqttPassword"] | "";
  
  return true;
}

bool ConfigManager::hasWiFiCredentials() {
  return LittleFS.exists(configPath_);
}

void ConfigManager::clearWiFiCredentials() {
  if (LittleFS.exists(configPath_)) {
    LittleFS.remove(configPath_);
  }
}

