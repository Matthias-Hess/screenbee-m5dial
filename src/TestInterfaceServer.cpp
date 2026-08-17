#include "TestInterfaceServer.h"
#include <WiFi.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "DeviceInfo.h"
#include "project/ProjectInstaller.h"
#include "config/ConfigManager.h"
#include "ddf_zip.h"

// The single global ConfigManager instance (main.cpp) - see
// handleMqttConfigure()'s own comment for why this server reaches into it
// directly instead of going through WiFiSetupServer.
extern ConfigManager configManager;

TestInterfaceServer::TestInterfaceServer(ClippedCanvas16* canvas, uint16_t port)
  : canvas_(canvas), port_(port), serverRunning_(false) {
  webServer_ = new WebServer(port_);
}

bool TestInterfaceServer::start() {
  if (!WiFi.isConnected()) {
    return false;
  }

  webServer_->on(
    "/api/project", HTTP_POST,
    [this]() { this->handleProjectUploadComplete(); },
    [this]() { this->handleProjectUploadChunk(); }
  );
  webServer_->on("/api/screen", HTTP_POST, [this]() { this->handleScreenSwitch(); });
  webServer_->on("/snapshot.bmp", HTTP_GET, [this]() { this->handleSnapshot(); });
  webServer_->on("/api/topic-values", HTTP_GET, [this]() { this->handleGetTopicValues(); });
  webServer_->on("/api/mqtt", HTTP_POST, [this]() { this->handleMqttConfigure(); });
  webServer_->on("/ddf.zip", HTTP_GET, [this]() { this->handleDdfZip(); });
  webServer_->on("/recovery-project", HTTP_GET, [this]() { this->handleRecoveryProject(); });

  webServer_->begin();
  serverRunning_ = true;
  return true;
}

void TestInterfaceServer::stop() {
  if (!serverRunning_) return;
  webServer_->stop();
  serverRunning_ = false;
}

void TestInterfaceServer::handleClient() {
  if (serverRunning_) {
    webServer_->handleClient();
  }
}

void TestInterfaceServer::sendJSONResponse(bool success, const String& message) {
  JsonDocument doc;
  doc["success"] = success;
  doc["message"] = message;
  String output;
  serializeJson(doc, output);
  webServer_->send(success ? 200 : 400, "application/json", output);
}

void TestInterfaceServer::handleProjectUploadChunk() {
  HTTPUpload& upload = webServer_->upload();

  if (upload.status == UPLOAD_FILE_START) {
    // A previous interrupted attempt may have left this open - see
    // UnifiedConfigurator::handleProjectUploadFile()'s identical comment on
    // the e-paper side for why this close-first matters (LittleFS refuses
    // to remove/reopen an already-open path otherwise).
    if (uploadFile_) {
      uploadFile_.close();
    }
    if (LittleFS.exists("/temp_upload.zip")) {
      LittleFS.remove("/temp_upload.zip");
    }
    uploadFile_ = LittleFS.open("/temp_upload.zip", FILE_WRITE);
    if (!uploadFile_) {
      Serial.println("[TestInterface] temp_upload.zip open failed");
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile_) {
      uploadFile_.write(upload.buf, upload.currentSize);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!uploadFile_) return;
    uploadFile_.close();

    if (!LittleFS.exists("/temp_upload.zip")) return;
    File checkFile = LittleFS.open("/temp_upload.zip", "r");
    if (!checkFile) return;
    size_t fileSize = checkFile.size();
    checkFile.close();
    if (fileSize < 100) {
      LittleFS.remove("/temp_upload.zip");
      Serial.println("[TestInterface] zip too small");
      return;
    }

    bool ok = validateAndExtractZip();

    if (ok) {
      LittleFS.remove("/temp_upload.zip");
      // Reboot to pick up the newly-installed /PROJECT/project.json fresh,
      // same convention as UnifiedConfigurator on the e-paper side - a
      // clean reload from scratch instead of trying to tear down and
      // rebuild live MQTT-subscription/render state mid-request. An HIL
      // orchestrator polling for the device to come back up already
      // expects this (see hil/README.md's e-paper upload flow).
      delay(2000);
      ESP.restart();
    } else {
      LittleFS.remove("/temp_upload.zip");
      Serial.println("[TestInterface] install failed");
    }
  }
}

void TestInterfaceServer::handleProjectUploadComplete() {
  sendJSONResponse(true, "Processing upload...");
}

bool TestInterfaceServer::validateAndExtractZip() {
  // Empty means the zip's project.json has no deviceId at all (older
  // export, or a non-project zip) - not proof it's wrong, so it isn't
  // rejected on that basis alone, matching UnifiedConfigurator's identical
  // reasoning on the e-paper side.
  String uploadedDeviceId = ProjectInstaller::peekProjectDeviceId("/temp_upload.zip");
  if (!uploadedDeviceId.isEmpty() && uploadedDeviceId != DEVICE_ID) {
    Serial.println("[TestInterface] deviceId mismatch");
    return false;
  }

  String error;
  bool ok = ProjectInstaller::installProjectZipFromFile("/temp_upload.zip", error);
  if (!ok) {
    Serial.println("[TestInterface] install error:");
    Serial.println(error);
  }
  return ok;
}

void TestInterfaceServer::handleScreenSwitch() {
  if (!webServer_->hasArg("index")) {
    sendJSONResponse(false, "Missing 'index' parameter");
    return;
  }
  if (!screenSwitchHandler_) {
    sendJSONResponse(false, "Screen switching is not available on this device");
    return;
  }

  int index = webServer_->arg("index").toInt();
  bool success = screenSwitchHandler_(index);

  if (success) {
    JsonDocument doc;
    doc["success"] = true;
    doc["screenIndex"] = index;
    String output;
    serializeJson(doc, output);
    webServer_->send(200, "application/json", output);
  } else {
    sendJSONResponse(false, "Invalid screen index or render failed: " + String(index));
  }
}

void TestInterfaceServer::handleSnapshot() {
  sendBMP();
}

// Standard 24-bit BITMAPINFOHEADER, no palette - unlike the e-paper
// firmware's 1-bit-plus-palette header, since this device's DDF declares
// 24bit colorDepth. RGB565 -> 8-bit-per-channel uses bit replication
// (top bits repeated into the low bits, e.g. r8 = (r5<<3)|(r5>>2)) rather
// than naive left-shift-only scaling, so 0x1F (max 5-bit) maps to 0xFF
// (max 8-bit) instead of 0xF8 - the standard, more accurate expansion.
void TestInterfaceServer::generateBMPHeader(uint8_t* header, int width, int height, int dataSize) {
  int fileSize = 54 + dataSize;

  header[0] = 'B';
  header[1] = 'M';
  header[2] = fileSize & 0xFF;
  header[3] = (fileSize >> 8) & 0xFF;
  header[4] = (fileSize >> 16) & 0xFF;
  header[5] = (fileSize >> 24) & 0xFF;
  header[6] = 0;
  header[7] = 0;
  header[8] = 0;
  header[9] = 0;
  header[10] = 54; // Offset to pixel data (14 + 40, no palette)
  header[11] = 0;
  header[12] = 0;
  header[13] = 0;

  header[14] = 40; // DIB header size
  header[15] = 0;
  header[16] = 0;
  header[17] = 0;
  header[18] = width & 0xFF;
  header[19] = (width >> 8) & 0xFF;
  header[20] = (width >> 16) & 0xFF;
  header[21] = (width >> 24) & 0xFF;
  header[22] = height & 0xFF;
  header[23] = (height >> 8) & 0xFF;
  header[24] = (height >> 16) & 0xFF;
  header[25] = (height >> 24) & 0xFF;
  header[26] = 1; // Planes
  header[27] = 0;
  header[28] = 24; // Bits per pixel
  header[29] = 0;
  header[30] = 0; // Compression (none)
  header[31] = 0;
  header[32] = 0;
  header[33] = 0;
  header[34] = dataSize & 0xFF;
  header[35] = (dataSize >> 8) & 0xFF;
  header[36] = (dataSize >> 16) & 0xFF;
  header[37] = (dataSize >> 24) & 0xFF;
  header[38] = 0x13; // X pixels per meter (72 DPI)
  header[39] = 0x0B;
  header[40] = 0;
  header[41] = 0;
  header[42] = 0x13; // Y pixels per meter
  header[43] = 0x0B;
  header[44] = 0;
  header[45] = 0;
  header[46] = 0; // Colors used (n/a, no palette)
  header[47] = 0;
  header[48] = 0;
  header[49] = 0;
  header[50] = 0; // Important colors
  header[51] = 0;
  header[52] = 0;
  header[53] = 0;
}

void TestInterfaceServer::sendBMP() {
  int width = canvas_->width();
  int height = canvas_->height();
  int rowBytes = width * 3; // 24-bit, no padding needed at width=240 (already a multiple of 4)
  int paddedRowBytes = ((rowBytes + 3) / 4) * 4;
  int imageDataSize = paddedRowBytes * height;
  int fileSize = 54 + imageDataSize;

  uint8_t header[54];
  generateBMPHeader(header, width, height, imageDataSize);

  webServer_->setContentLength(fileSize);
  webServer_->sendHeader("Content-Type", "image/bmp");
  webServer_->sendHeader("Cache-Control", "no-cache");
  webServer_->send(200, "image/bmp", "");
  webServer_->client().write(header, 54);

  uint16_t* buffer = canvas_->getBuffer();
  uint8_t* rowBuffer = (uint8_t*)malloc(paddedRowBytes);
  if (!rowBuffer) return;

  // BMP rows are stored bottom-up.
  for (int y = height - 1; y >= 0; y--) {
    memset(rowBuffer, 0, paddedRowBytes);
    for (int x = 0; x < width; x++) {
      uint16_t px = buffer[y * width + x];
      uint8_t r5 = (px >> 11) & 0x1F;
      uint8_t g6 = (px >> 5) & 0x3F;
      uint8_t b5 = px & 0x1F;
      uint8_t r8 = (r5 << 3) | (r5 >> 2);
      uint8_t g8 = (g6 << 2) | (g6 >> 4);
      uint8_t b8 = (b5 << 3) | (b5 >> 2);
      rowBuffer[x * 3 + 0] = b8;
      rowBuffer[x * 3 + 1] = g8;
      rowBuffer[x * 3 + 2] = r8;
    }
    webServer_->client().write(rowBuffer, paddedRowBytes);
  }

  free(rowBuffer);
}

void TestInterfaceServer::handleGetTopicValues() {
  if (!getTopicValueHandler_) {
    sendJSONResponse(false, "Topic value lookup is not available on this device");
    return;
  }

  String topicsParam = webServer_->arg("topics");
  JsonDocument doc;
  int start = 0;
  while (start <= (int)topicsParam.length()) {
    int comma = topicsParam.indexOf(',', start);
    String topic = (comma == -1) ? topicsParam.substring(start) : topicsParam.substring(start, comma);
    topic.trim();
    if (!topic.isEmpty()) {
      doc[topic] = getTopicValueHandler_(topic);
    }
    if (comma == -1) break;
    start = comma + 1;
  }

  String output;
  serializeJson(doc, output);
  webServer_->send(200, "application/json", output);
}

void TestInterfaceServer::handleMqttConfigure() {
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

  WiFiCredentials creds;
  configManager.loadWiFiCredentials(creds);  // ok to fail - WiFi is already up, so credentials already exist; only MQTT fields are being overwritten below
  creds.mqttProtocol = protocol;
  creds.mqttHost = host;
  creds.mqttPort = port;
  creds.mqttUsername = username;
  creds.mqttPassword = password;

  if (configManager.saveWiFiCredentials(creds)) {
    sendJSONResponse(true, "MQTT configuration saved - reboot to apply");
  } else {
    sendJSONResponse(false, "Failed to save MQTT configuration");
  }
}

void TestInterfaceServer::handleDdfZip() {
  // send_P(), not send() - the zip's raw bytes contain embedded 0x00s
  // throughout, and every send() overload takes a null-terminated
  // String/const char* with no explicit length, which would silently
  // truncate at the first one. Only send_P()'s 4-arg overload accepts an
  // explicit content length.
  webServer_->send_P(200, "application/zip", (const char*)DDF_ZIP, sizeof(DDF_ZIP));
}

// GET /recovery-project - the last successfully-verified deploy
// DeployManager.cpp promoted to RECOVERY_PROJECT_PATH (DeviceInfo.h),
// unchanged since (survives a failed extraction, see DeployManager.cpp's
// own comment). docs/nested-provenance.md's "Version compatibility" >
// Fall 3 (designer repo) - this is the designer's actual recovery entry
// point once it grows a "Recover project from device" UI; nothing calls
// this yet.
//
// Streamed via client().write() in chunks, same pattern as sendBMP()
// above, rather than reading the whole file into one malloc()'d buffer
// first - this file can be tens to low hundreds of KB (a full editable
// project plus its own embedded DDF), and there's no reason to hold it
// all in RAM at once just to hand it to the TCP stack.
void TestInterfaceServer::handleRecoveryProject() {
  if (!LittleFS.exists(RECOVERY_PROJECT_PATH)) {
    webServer_->send(404, "text/plain", "No recovery copy available on this device yet");
    return;
  }

  File f = LittleFS.open(RECOVERY_PROJECT_PATH, "r");
  if (!f) {
    webServer_->send(500, "text/plain", "Failed to open the recovery copy");
    return;
  }

  webServer_->setContentLength(f.size());
  webServer_->sendHeader("Content-Disposition", "attachment; filename=\"recovered_project.zip\"");
  webServer_->send(200, "application/zip", "");

  uint8_t buffer[1024];
  while (f.available()) {
    size_t n = f.read(buffer, sizeof(buffer));
    if (n == 0) break;
    webServer_->client().write(buffer, n);
  }
  f.close();
}
