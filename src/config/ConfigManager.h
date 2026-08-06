#pragma once
#include "../interfaces/IConfigStorage.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

/**
 * Configuration manager using LittleFS for persistent storage
 * Implements IConfigStorage interface for dependency inversion
 */
class ConfigManager : public IConfigStorage {
public:
  ConfigManager(const char* configPath = "/config.json");
  
  // IConfigStorage interface implementation
  bool begin() override;
  bool saveWiFiCredentials(const WiFiCredentials& creds) override;
  bool loadWiFiCredentials(WiFiCredentials& creds) override;
  bool hasWiFiCredentials() override;
  void clearWiFiCredentials() override;

private:
  const char* configPath_;
};

