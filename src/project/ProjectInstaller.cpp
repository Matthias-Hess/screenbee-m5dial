#include "ProjectInstaller.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

#ifndef MINIZ_EXPORT
#define MINIZ_EXPORT
#endif

#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ZLIB_APIS

#include "miniz.h"

namespace {

// Iterative directory deletion to avoid stack overflow - ported verbatim
// from MqttEPaperDisplay2's ProjectInstaller.cpp.
void deleteDirectoryIterative(const String& path) {
  File root = LittleFS.open(path);
  if (root && root.isDirectory()) {
    File file = root.openNextFile();
    while (file) {
      String filePath = path + "/" + String(file.name());
      if (!file.isDirectory()) {
        LittleFS.remove(filePath);
      }
      file = root.openNextFile();
    }
    root.close();
  }

  root = LittleFS.open(path);
  if (root && root.isDirectory()) {
    File file = root.openNextFile();
    while (file) {
      String filePath = path + "/" + String(file.name());
      if (file.isDirectory()) {
        File subdir = LittleFS.open(filePath);
        if (subdir && subdir.isDirectory()) {
          File subfile = subdir.openNextFile();
          while (subfile) {
            String subFilePath = filePath + "/" + String(subfile.name());
            if (!subfile.isDirectory()) {
              LittleFS.remove(subFilePath);
            }
            subfile = subdir.openNextFile();
          }
          subdir.close();
        }
        LittleFS.rmdir(filePath);
      }
      file = root.openNextFile();
    }
    root.close();
  }

  LittleFS.rmdir(path);
}

// Ported verbatim from MqttEPaperDisplay2's ProjectInstaller.cpp.
void createDirectoriesForPath(const String& path) {
  int start = 1; // Skip leading /
  while (true) {
    int slashPos = path.indexOf('/', start);
    String dirPath = (slashPos == -1) ? path : path.substring(0, slashPos);

    if (!LittleFS.exists(dirPath)) {
      LittleFS.mkdir(dirPath);
    }

    if (slashPos == -1) break;
    start = slashPos + 1;
  }
}

} // namespace

namespace ProjectInstaller {

String peekProjectDeviceId(const String& zipPath) {
  File zipFile = LittleFS.open(zipPath, "r");
  if (!zipFile) return "";

  size_t zipSize = zipFile.size();
  Serial.printf("[ProjectInstaller] zipSize=%u\n", (unsigned)zipSize);
  uint8_t* zipData = (uint8_t*)malloc(zipSize);
  Serial.printf("[ProjectInstaller] malloc -> %p\n", (void*)zipData);
  if (!zipData) {
    zipFile.close();
    return "";
  }
  zipFile.readBytes((char*)zipData, zipSize);
  zipFile.close();
  Serial.println("[ProjectInstaller] readBytes done");

  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  Serial.println("[ProjectInstaller] calling mz_zip_reader_init_mem...");
  bool initOk = mz_zip_reader_init_mem(&zip, zipData, zipSize, 0);
  Serial.printf("[ProjectInstaller] mz_zip_reader_init_mem -> %d\n", (int)initOk);
  if (!initOk) {
    free(zipData);
    return "";
  }

  mz_uint32 fileIndex = 0;
  Serial.println("[ProjectInstaller] locating project.json...");
  bool located = mz_zip_reader_locate_file_v2(&zip, "project.json", nullptr, 0, &fileIndex);
  Serial.printf("[ProjectInstaller] locate -> %d, fileIndex=%u\n", (int)located, (unsigned)fileIndex);
  if (!located) {
    mz_zip_reader_end(&zip);
    free(zipData);
    return "";
  }

  mz_zip_archive_file_stat fileStat;
  memset(&fileStat, 0, sizeof(fileStat));
  bool statOk = mz_zip_reader_file_stat(&zip, fileIndex, &fileStat);
  Serial.printf("[ProjectInstaller] file_stat -> %d, comp_size=%u, uncomp_size=%u, method=%u\n",
                (int)statOk, (unsigned)fileStat.m_comp_size, (unsigned)fileStat.m_uncomp_size, (unsigned)fileStat.m_method);
  if (!statOk) {
    mz_zip_reader_end(&zip);
    free(zipData);
    return "";
  }

  // Deliberately over-allocate past what m_uncomp_size claims is needed,
  // with a canary region after it - diagnostic experiment (2026-08-09) for
  // the DEFLATE-entry crash inside mz_zip_reader_extract_file_to_heap():
  // that call sizes its destination buffer to *exactly* m_uncomp_size, so
  // if tinfl_decompress ever writes even slightly past the bound it's
  // told, it corrupts whatever heap allocation happens to sit right after
  // it - consistent with the varying-by-input-size crash signatures seen
  // (a heap-corruption fingerprint, not one fixed logic error). Calling
  // the lower-level mz_zip_reader_extract_to_mem_no_alloc() instead, into
  // a buffer padded well past m_uncomp_size, tells us two things at once:
  // whether extraction survives at all with headroom (works around the
  // bug if so), and by checking the canary bytes afterward, whether an
  // overrun actually happened even when it doesn't crash outright.
  const size_t kCanarySize = 256;
  size_t jsonSize = fileStat.m_uncomp_size;
  size_t paddedSize = jsonSize + kCanarySize;
  uint8_t* jsonData = (uint8_t*)malloc(paddedSize);
  Serial.printf("[ProjectInstaller] padded malloc(%u) -> %p\n", (unsigned)paddedSize, (void*)jsonData);
  if (!jsonData) {
    mz_zip_reader_end(&zip);
    free(zipData);
    return "";
  }
  memset(jsonData + jsonSize, 0xAA, kCanarySize);

  Serial.println("[ProjectInstaller] calling mz_zip_reader_extract_to_mem_no_alloc (padded buffer)...");
  bool extractOk = mz_zip_reader_extract_to_mem_no_alloc(&zip, fileIndex, jsonData, paddedSize, 0, nullptr, 0);
  Serial.printf("[ProjectInstaller] extract -> %d\n", (int)extractOk);

  bool canaryIntact = true;
  for (size_t i = 0; i < kCanarySize; i++) {
    if (jsonData[jsonSize + i] != 0xAA) {
      canaryIntact = false;
      break;
    }
  }
  Serial.printf("[ProjectInstaller] canary intact -> %d\n", (int)canaryIntact);

  mz_zip_reader_end(&zip);
  free(zipData);

  if (!extractOk) {
    free(jsonData);
    return "";
  }

  // Matches ProjectLoader.cpp's own NestingLimit(30) exactly - see the
  // e-paper ProjectInstaller's identical comment for why the default (10)
  // isn't enough for a real project once tab-control/panel nesting,
  // icon-pair arrays etc. are involved.
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, (const char*)jsonData, jsonSize, DeserializationOption::NestingLimit(30));
  free(jsonData);
  if (err) return "";

  const char* deviceId = doc["deviceId"] | "";
  return String(deviceId);
}

bool installProjectZipFromFile(const String& zipPath, String& errorOut) {
  if (LittleFS.exists("/PROJECT")) {
    deleteDirectoryIterative("/PROJECT");
  }
  if (!LittleFS.mkdir("/PROJECT")) {
    errorOut = "Failed to create /PROJECT directory";
    return false;
  }

  File zipFile = LittleFS.open(zipPath, "r");
  if (!zipFile) {
    errorOut = "Staged zip file missing";
    return false;
  }

  size_t zipSize = zipFile.size();
  uint8_t* zipData = (uint8_t*)malloc(zipSize);
  if (!zipData) {
    zipFile.close();
    errorOut = "Out of memory reading zip";
    return false;
  }
  zipFile.readBytes((char*)zipData, zipSize);
  zipFile.close();

  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  if (!mz_zip_reader_init_mem(&zip, zipData, zipSize, 0)) {
    free(zipData);
    errorOut = "Invalid zip archive";
    return false;
  }

  int numFiles = (int)mz_zip_reader_get_num_files(&zip);
  bool success = true;
  for (int i = 0; i < numFiles && success; i++) {
    mz_zip_archive_file_stat file_stat;
    if (!mz_zip_reader_file_stat(&zip, i, &file_stat)) continue;
    if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;

    String outPath = "/PROJECT/" + String(file_stat.m_filename);
    int lastSlash = outPath.lastIndexOf('/');
    if (lastSlash > 0) {
      createDirectoriesForPath(outPath.substring(0, lastSlash));
    }

    File outFile = LittleFS.open(outPath, FILE_WRITE);
    if (!outFile) {
      success = false;
      break;
    }

    const size_t BUFFER_SIZE = 4096;
    uint8_t* buffer = (uint8_t*)malloc(BUFFER_SIZE);
    if (!buffer) {
      outFile.close();
      success = false;
      break;
    }

    mz_zip_reader_extract_iter_state* pState = mz_zip_reader_extract_iter_new(&zip, i, 0);
    if (!pState) {
      free(buffer);
      outFile.close();
      success = false;
      break;
    }

    while (true) {
      size_t bytesRead = mz_zip_reader_extract_iter_read(pState, buffer, BUFFER_SIZE);
      if (bytesRead == 0) {
        if (mz_zip_reader_extract_iter_free(pState) != MZ_TRUE) success = false;
        break;
      }
      outFile.write(buffer, bytesRead);
    }

    free(buffer);
    outFile.close();
  }

  mz_zip_reader_end(&zip);
  free(zipData);

  if (!success) {
    errorOut = "Zip extraction failed";
    return false;
  }

  if (!LittleFS.exists("/PROJECT/project.json")) {
    errorOut = "Extracted project has no project.json";
    return false;
  }

  return true;
}

} // namespace ProjectInstaller
