#include "DeployManager.h"
#include <WiFi.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ZLIB_APIS
#include "miniz.h"

#include "DeviceInfo.h"
#include "project/ProjectInstaller.h"

namespace {
const char* DEPLOY_DOWNLOAD_PATH = "/deploy_download.zip";
}

void DeployManager::handleDeployMessage(const String& payload) {
  if (!publishStatus_) {
    Serial.println("[DeployManager] No status handler installed yet, ignoring deploy message");
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    Serial.println("[DeployManager] Malformed deploy payload, ignoring");
    return;
  }

  String deployId = doc["deployId"] | "";
  String url = doc["url"] | "";
  uint32_t expectedCrc32 = doc["crc32"] | 0UL;
  if (deployId.isEmpty() || url.isEmpty()) {
    Serial.println("[DeployManager] deploy message missing deployId/url, ignoring");
    return;
  }

  // Clear the retained trigger FIRST, before any other processing - see
  // this class's own header comment for why this is what actually breaks
  // the reboot loop, not just tidiness. The clearing publish itself
  // arrives back at this same handler (still subscribed to the topic),
  // but with an empty payload that fails deserializeJson() above and
  // returns harmlessly - not a second recursive deploy attempt.
  if (clearTrigger_) clearTrigger_();

  if (busy_) {
    Serial.printf("[DeployManager] Already deploying, rejecting %s as busy\n", deployId.c_str());
    publishStatus_(deployId, "busy", "", -1);
    return;
  }
  busy_ = true;

  Serial.printf("[DeployManager] Starting deploy %s from %s\n", deployId.c_str(), url.c_str());
  publishStatus_(deployId, "downloading", "", 0);

  if (LittleFS.exists(DEPLOY_DOWNLOAD_PATH)) LittleFS.remove(DEPLOY_DOWNLOAD_PATH);

  bool downloadOk = downloadToFile(url, DEPLOY_DOWNLOAD_PATH, [&](int percent) {
    publishStatus_(deployId, "downloading", "", percent);
  });
  if (!downloadOk) {
    Serial.println("[DeployManager] Download failed");
    publishStatus_(deployId, "error", "Download failed", -1);
    LittleFS.remove(DEPLOY_DOWNLOAD_PATH);
    busy_ = false;
    return;
  }
  publishStatus_(deployId, "download_complete", "", 100);

  publishStatus_(deployId, "verifying", "", -1);
  if (!verifyCrc32(DEPLOY_DOWNLOAD_PATH, expectedCrc32)) {
    Serial.println("[DeployManager] CRC32 mismatch - not touching /PROJECT");
    publishStatus_(deployId, "error", "Checksum mismatch", -1);
    LittleFS.remove(DEPLOY_DOWNLOAD_PATH);
    busy_ = false;
    return;
  }

  // deviceId check happens here, still before installProjectZipFromFile()
  // ever wipes /PROJECT - ProjectInstaller::installProjectZipFromFile()'s
  // own header comment is explicit that it's "not rollback-safe on its
  // own", so every check that could reject this deploy has to run first,
  // same "never touch /PROJECT until verified" guarantee
  // hil/README.md documents for the e-paper target's identical flow.
  String uploadedDeviceId = ProjectInstaller::peekProjectDeviceId(DEPLOY_DOWNLOAD_PATH);
  if (!uploadedDeviceId.isEmpty() && uploadedDeviceId != DEVICE_ID) {
    Serial.printf("[DeployManager] deviceId mismatch (\"%s\" != \"%s\") - not touching /PROJECT\n",
                   uploadedDeviceId.c_str(), DEVICE_ID);
    publishStatus_(deployId, "error", "Project built for a different device (incompatible)", -1);
    LittleFS.remove(DEPLOY_DOWNLOAD_PATH);
    busy_ = false;
    return;
  }

  publishStatus_(deployId, "applying", "", -1);
  String installError;
  if (!ProjectInstaller::installProjectZipFromFile(DEPLOY_DOWNLOAD_PATH, installError)) {
    Serial.printf("[DeployManager] Install failed: %s\n", installError.c_str());
    publishStatus_(deployId, "error", installError, -1);
    LittleFS.remove(DEPLOY_DOWNLOAD_PATH);
    busy_ = false;
    return;
  }

  LittleFS.remove(DEPLOY_DOWNLOAD_PATH);
  publishStatus_(deployId, "rebooting", "", -1);
  Serial.println("[DeployManager] Deploy applied, restarting");
  delay(500);  // let the "rebooting" MQTT publish actually flush before the restart tears down the radio
  ESP.restart();
}

bool DeployManager::downloadToFile(const String& url, const String& destPath, std::function<void(int percent)> onProgress) {
  HTTPClient http;
  if (!http.begin(url)) return false;

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[DeployManager] GET %s -> HTTP %d\n", url.c_str(), code);
    http.end();
    return false;
  }

  File f = LittleFS.open(destPath, FILE_WRITE);
  if (!f) {
    http.end();
    return false;
  }

  int totalLen = http.getSize();
  int written = 0;
  int lastReportedPercent = -1;
  uint8_t buf[1024];
  WiFiClient* stream = http.getStreamPtr();

  while (http.connected() && (totalLen < 0 || written < totalLen)) {
    size_t avail = stream->available();
    if (avail == 0) {
      delay(1);
      continue;
    }
    size_t toRead = avail < sizeof(buf) ? avail : sizeof(buf);
    int got = stream->readBytes(buf, toRead);
    if (got <= 0) break;
    f.write(buf, got);
    written += got;

    // Report every 10%, not every 1% - a progress bar can't tell the
    // difference, and publishing ~100 MQTT messages back to back with no
    // delay/yield between them is suspected (2026-08-10, still being
    // isolated on real hardware) to overwhelm the underlying TCP send
    // path silently - deploy-status messages stopped being observed by
    // any external subscriber at all despite every local publish() call
    // itself reporting success.
    if (totalLen > 0 && onProgress) {
      int percent = ((written * 100) / totalLen / 10) * 10;
      if (percent != lastReportedPercent) {
        lastReportedPercent = percent;
        onProgress(percent);
      }
    }
  }

  f.close();
  http.end();

  bool ok = (totalLen < 0) || (written == totalLen);
  Serial.printf("[DeployManager] Downloaded %d bytes (expected %d) - %s\n", written, totalLen, ok ? "ok" : "short read");
  return ok;
}

bool DeployManager::verifyCrc32(const String& path, uint32_t expectedCrc32) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;

  mz_uint32 crc = MZ_CRC32_INIT;
  uint8_t buf[1024];
  while (true) {
    size_t n = f.read(buf, sizeof(buf));
    if (n == 0) break;
    crc = mz_crc32(crc, buf, n);
  }
  f.close();

  Serial.printf("[DeployManager] CRC32: got 0x%08X, expected 0x%08X\n", (unsigned)crc, (unsigned)expectedCrc32);
  return crc == expectedCrc32;
}
