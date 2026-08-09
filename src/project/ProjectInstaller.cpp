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

// Custom allocator wired into every mz_zip_archive used for DEFLATE
// extraction. Two separate bugs had to be found and fixed here (2026-08-09,
// full history in docs/device-contract.md (designer repo) §8):
//
// 1. A genuine upstream miniz bug: mz_zip_reader_extract_iter_new()'s OOM
//    cleanup path frees a non-heap pointer for in-memory archives - see
//    lib/miniz/miniz_zip.c's patch comment. That was the actual cause of
//    every DEFLATE-extraction crash chased earlier in this investigation
//    (a stack-based tinfl_decompressor overflow was suspected first and
//    "fixed" by switching APIs and adding safety padding - neither was
//    the real bug, though the API switch to the iterator-based extraction
//    functions is still worth keeping).
//
// 2. Once (1) stopped the crash, extraction still failed cleanly with
//    MZ_ZIP_ALLOC_FAILED: the OOM path was still being *hit*, just no
//    longer corrupting memory when it was. mz_zip_reader_extract_iter_new()
//    needs one 32KB *contiguous* block (TINFL_LZ_DICT_SIZE, the DEFLATE
//    dictionary window) - by the time a project upload reaches this code,
//    WiFi/WebServer/LittleFS have fragmented the heap enough that no single
//    32KB block survives even with 100KB+ total free (confirmed via
//    heap_caps_get_largest_free_block()). A dedicated FreeRTOS task with
//    its own large stack was tried as a fix and made this *worse* (the
//    task's own stack is itself a large contiguous allocation, competing
//    for the same scarce blocks) before being removed entirely - see git
//    history on TestInterfaceServer::runValidateAndExtractOnDedicatedTask().
//
// The fix: reserve both fixed-size buffers miniz needs for DEFLATE
// extraction as *padded static* buffers (BSS, not heap - allocated once at
// link time, never competes with runtime fragmentation, and immune to the
// tinfl_decompress overrun found below since there's guard space on both
// sides): the ~9.5KB iterator state struct (mz_zip_reader_extract_iter_state
// is a fixed compile-time size, always the same for every call) and the
// 32KB DEFLATE dictionary window (TINFL_LZ_DICT_SIZE). Every other
// allocation miniz makes here (central directory bookkeeping - small,
// varies with entry count) goes through plain malloc()/free()/realloc(),
// unaffected by any of this.
//
// Why padding is needed at all: the original DEFLATE-extraction
// investigation (before the two miniz bugs above were isolated) measured
// tinfl_decompress writing 100-270 bytes past the end of the dictionary
// window via a padded-canary probe. That overrun is real, independent of
// the two bugs above, and - per the 2026-08-09 A/B test that removed
// padding entirely to isolate bug (1) - lands on *whichever* of these two
// buffers' true end happens to be reachable, corrupting the heap block
// that follows it (first observed as pWrite_buf's neighbor, later as
// pState's neighbor once pWrite_buf became a guarded static buffer and
// pState was still plain malloc()). Both need guarding, not just one.
// Kept generous since it's pure BSS, not heap - no fragmentation cost
// either way, unlike heap-based padding (tried first, see git history:
// scaled from 1KB up to 40KB and was *still* sometimes insufficient,
// because a heap-based pad competes with the same fragmentation this
// buffer exists to avoid in the first place).
const size_t kGuardPad = 4096;

uint8_t g_dictWindowRegion[TINFL_LZ_DICT_SIZE + kGuardPad * 2];
uint8_t* const g_dictWindowBuffer = g_dictWindowRegion + kGuardPad;
bool g_dictWindowInUse = false;

const size_t kIterStateSize = sizeof(mz_zip_reader_extract_iter_state);
uint8_t g_iterStateRegion[kIterStateSize + kGuardPad * 2];
uint8_t* const g_iterStateBuffer = g_iterStateRegion + kGuardPad;
bool g_iterStateInUse = false;

void* dictReservingAlloc(void* opaque, size_t items, size_t size) {
  (void)opaque;
  size_t realSize = items * size;
  if (realSize == TINFL_LZ_DICT_SIZE && !g_dictWindowInUse) {
    g_dictWindowInUse = true;
    return g_dictWindowBuffer;
  }
  if (realSize == kIterStateSize && !g_iterStateInUse) {
    g_iterStateInUse = true;
    return g_iterStateBuffer;
  }
  return malloc(realSize);
}
void dictReservingFree(void* opaque, void* address) {
  (void)opaque;
  if (!address) return;
  if (address == g_dictWindowBuffer) {
    g_dictWindowInUse = false;
    return;
  }
  if (address == g_iterStateBuffer) {
    g_iterStateInUse = false;
    return;
  }
  free(address);
}
void* dictReservingRealloc(void* opaque, void* address, size_t items, size_t size) {
  // miniz never reallocs either of the two buffers above (both allocated
  // once at their final size), only the smaller central-directory
  // bookkeeping arrays - plain realloc is always correct for those, since
  // they never pass through either static buffer.
  (void)opaque;
  return realloc(address, items * size);
}
void useDictReservingAllocator(mz_zip_archive& zip) {
  zip.m_pAlloc = dictReservingAlloc;
  zip.m_pFree = dictReservingFree;
  zip.m_pRealloc = dictReservingRealloc;
}

namespace ProjectInstaller {

String peekProjectDeviceId(const String& zipPath) {
  File zipFile = LittleFS.open(zipPath, "r");
  if (!zipFile) return "";

  size_t zipSize = zipFile.size();
  uint8_t* zipData = (uint8_t*)malloc(zipSize);
  if (!zipData) {
    zipFile.close();
    return "";
  }
  zipFile.readBytes((char*)zipData, zipSize);
  zipFile.close();

  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  useDictReservingAllocator(zip);
  if (!mz_zip_reader_init_mem(&zip, zipData, zipSize, 0)) {
    free(zipData);
    return "";
  }

  mz_uint32 fileIndex = 0;
  if (!mz_zip_reader_locate_file_v2(&zip, "project.json", nullptr, 0, &fileIndex)) {
    mz_zip_reader_end(&zip);
    free(zipData);
    return "";
  }

  mz_zip_archive_file_stat fileStat;
  memset(&fileStat, 0, sizeof(fileStat));
  if (!mz_zip_reader_file_stat(&zip, fileIndex, &fileStat)) {
    mz_zip_reader_end(&zip);
    free(zipData);
    return "";
  }

  // See useDictReservingAllocator() above for why the zip's allocator is
  // overridden. Separately, don't trust mz_zip_reader_extract_iter_free()'s
  // own return value for success/failure here - verify against
  // fileStat.m_crc32 ourselves instead, over jsonData (a plain, unrelated
  // allocation unaffected by anything miniz's internal bookkeeping gets
  // wrong).
  size_t jsonSize = fileStat.m_uncomp_size;
  uint8_t* jsonData = (uint8_t*)malloc(jsonSize);
  if (!jsonData) {
    mz_zip_reader_end(&zip);
    free(zipData);
    return "";
  }

  bool extractOk = false;
  mz_zip_reader_extract_iter_state* pState = mz_zip_reader_extract_iter_new(&zip, fileIndex, 0);
  if (pState) {
    size_t totalRead = 0;
    while (totalRead < jsonSize) {
      size_t bytesRead = mz_zip_reader_extract_iter_read(pState, jsonData + totalRead, jsonSize - totalRead);
      if (bytesRead == 0) break;
      totalRead += bytesRead;
    }
    mz_zip_reader_extract_iter_free(pState);
    extractOk = (totalRead == jsonSize) && (mz_crc32(MZ_CRC32_INIT, jsonData, jsonSize) == fileStat.m_crc32);
  }

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
  useDictReservingAllocator(zip);
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
      Serial.printf("[ProjectInstaller] extract_iter_new failed for %s (err=%d)\n", file_stat.m_filename, (int)mz_zip_get_last_error(&zip));
      free(buffer);
      outFile.close();
      success = false;
      break;
    }

    // Don't trust mz_zip_reader_extract_iter_free()'s own return value for
    // DEFLATE entries - see the matching comment in peekProjectDeviceId()
    // above for why. Verify against file_stat.m_crc32 ourselves instead.
    mz_uint32 crc = MZ_CRC32_INIT;
    mz_uint64 totalWritten = 0;
    while (true) {
      size_t bytesRead = mz_zip_reader_extract_iter_read(pState, buffer, BUFFER_SIZE);
      if (bytesRead == 0) {
        mz_zip_reader_extract_iter_free(pState);
        break;
      }
      crc = mz_crc32(crc, buffer, bytesRead);
      totalWritten += bytesRead;
      outFile.write(buffer, bytesRead);
    }
    if (totalWritten != file_stat.m_uncomp_size || crc != file_stat.m_crc32) {
      Serial.printf("[ProjectInstaller] extract verification failed for %s\n", file_stat.m_filename);
      success = false;
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
