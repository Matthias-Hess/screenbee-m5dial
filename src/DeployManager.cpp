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
// RECOVERY_PROJECT_PATH itself is #defined in DeviceInfo.h, not local to
// this file - see that definition's own comment for why (TestInterfaceServer.cpp
// needs to agree on the same path to serve it back). The verified download
// gets renamed there once it's passed CRC32/schemaVersion/deviceId, below,
// and stays there regardless of whether extraction into /PROJECT
// afterward succeeds. Holds the same nested-provenance chain the export
// carries: the baked bitmaps live in /PROJECT for rendering, but this
// file's own _source/project.zip entry is the actual editable project
// (which itself carries _source/ddf.zip) - the thing a recovery request
// hands back.

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

  // schemaVersion and deviceId checks happen here, still before
  // installProjectZipFromFile() ever wipes /PROJECT -
  // ProjectInstaller::installProjectZipFromFile()'s own header comment is
  // explicit that it's "not rollback-safe on its own", so every check
  // that could reject this deploy has to run first, same "never touch
  // /PROJECT until verified" guarantee hil/README.md documents for the
  // e-paper target's identical flow.
  //
  // schemaVersion first: if the export's own file shape can't be trusted,
  // there's no point reading anything else out of it, deviceId included -
  // see docs/nested-provenance.md's "Version compatibility" > Fall 2,
  // step 1 (designer repo).
  long uploadedSchemaVersion = ProjectInstaller::peekProjectSchemaVersion(DEPLOY_DOWNLOAD_PATH);
  if (uploadedSchemaVersion > EXPORT_SCHEMA_VERSION) {
    Serial.printf("[DeployManager] schemaVersion too new (%ld > %d) - not touching /PROJECT\n",
                   uploadedSchemaVersion, EXPORT_SCHEMA_VERSION);
    publishStatus_(deployId, "error", "Project file format is newer than this firmware understands", -1);
    LittleFS.remove(DEPLOY_DOWNLOAD_PATH);
    busy_ = false;
    return;
  }

  String uploadedDeviceId = ProjectInstaller::peekProjectDeviceId(DEPLOY_DOWNLOAD_PATH);
  if (!uploadedDeviceId.isEmpty() && uploadedDeviceId != DEVICE_ID) {
    Serial.printf("[DeployManager] deviceId mismatch (\"%s\" != \"%s\") - not touching /PROJECT\n",
                   uploadedDeviceId.c_str(), DEVICE_ID);
    publishStatus_(deployId, "error", "Project built for a different device (incompatible)", -1);
    LittleFS.remove(DEPLOY_DOWNLOAD_PATH);
    busy_ = false;
    return;
  }

  // Promote the staged, now-verified download to the permanent recovery
  // slot before attempting extraction, not after - see this file's
  // "backup is the retained download, not a separate write" reasoning
  // (docs/nested-provenance.md, designer repo). LittleFS.rename()
  // atomically replaces any existing file at the destination (a core
  // littlefs power-loss-safety guarantee - see littlefs's own design
  // docs), so there's never a window with zero recovery copies, and this
  // costs nothing extra in flash or time: the file's bytes don't move,
  // only its directory entry does. Everything below reads from
  // RECOVERY_PROJECT_PATH now, not DEPLOY_DOWNLOAD_PATH, which no longer
  // exists once the rename succeeds - and deliberately survives even if
  // extraction below fails, so a bad /PROJECT extraction never also costs
  // the recovery copy.
  bool promoted = LittleFS.rename(DEPLOY_DOWNLOAD_PATH, RECOVERY_PROJECT_PATH);
  if (!promoted) {
    Serial.println("[DeployManager] Failed to promote download to the recovery slot - deploy continues, but this attempt won't be recoverable later");
  }
  const char* installPath = promoted ? RECOVERY_PROJECT_PATH : DEPLOY_DOWNLOAD_PATH;

  publishStatus_(deployId, "applying", "", -1);
  String installError;
  if (!ProjectInstaller::installProjectZipFromFile(installPath, installError)) {
    Serial.printf("[DeployManager] Install failed: %s\n", installError.c_str());
    publishStatus_(deployId, "error", installError, -1);
    // Deliberately not deleting installPath: if the promote above
    // succeeded, this is the recovery slot and must survive a failed
    // install; if the promote itself failed, this is still
    // DEPLOY_DOWNLOAD_PATH, which the next deploy attempt's own
    // LittleFS.remove() at the top of this function cleans up anyway.
    busy_ = false;
    return;
  }

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
    // Checked, not assumed (2026-08-11) - LittleFS.write() can write fewer
    // bytes than asked (or 0) when the filesystem fills up mid-download,
    // and `written` used to be incremented by `got` regardless. That let a
    // disk-full download still report "Downloaded N bytes (expected N) -
    // ok" (byte COUNT matched, since it counted what was read from the
    // network, not what actually landed on flash) and only fail much later
    // at CRC32 verification - surfacing as a confusing "Checksum mismatch"
    // instead of an honest out-of-space error. Found live: LittleFS logged
    // "No more free space" repeatedly during a real deploy while this loop
    // kept reporting normal progress the whole time.
    size_t wroteNow = f.write(buf, got);
    if (wroteNow != (size_t)got) {
      Serial.printf("[DeployManager] Write failed at byte %d (wrote %u of %d) - likely out of space\n",
                    written, (unsigned)wroteNow, got);
      f.close();
      http.end();
      return false;
    }
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

  // The per-write check above can't catch everything on its own: LittleFS
  // caches, so a write can be acknowledged in full and only actually fail
  // when that cache is flushed at close() - which returns void here, so
  // there's nothing to check. Reading the file's real size back is the one
  // check that can't be fooled, and it closes exactly the assumption that
  // made the original bug so confusing: `written` counts bytes taken off
  // the network, not bytes that reached flash.
  size_t onDisk = 0;
  File check = LittleFS.open(destPath, "r");
  if (check) {
    onDisk = check.size();
    check.close();
  }
  if (onDisk != (size_t)written) {
    Serial.printf("[DeployManager] Only %u of %d bytes reached flash - likely out of space\n",
                  (unsigned)onDisk, written);
    return false;
  }

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
