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

// mz_zip_reader_init_mem() needs the ENTIRE zip file sitting in one
// contiguous heap block up front (the previous approach here: malloc(
// zipSize) + read the whole file into it). That's fine for the tiny test
// zips this was originally verified against, but a real exported project
// (fonts/icons, tens of KB) routinely exceeds what a single malloc can get
// on this device - confirmed 2026-08-10 building the M5 Dial HIL fixture:
// a 21KB zip failed ("Out of memory reading zip") once the device was
// actually running normally (project loaded, MQTT connected, canvas
// drawn) even though the *same size* zip installed fine right after a
// fresh boot - this board has no PSRAM at all (internal SRAM only), so
// there's no "just allocate more" fix, only "stop needing one big
// allocation in the first place."
//
// mz_zip_reader_init() (the callback-based variant, as opposed to
// _init_mem()) reads the archive through a caller-supplied m_pRead
// function instead, in small on-demand chunks (central directory entries,
// then each file's compressed bytes during extraction) - no upper bound on
// zip size beyond LittleFS itself. The source zip File has to stay open
// for the whole call (miniz seeks around it non-sequentially while parsing
// the central directory), unlike the old malloc-then-close-immediately
// pattern.
struct ZipFileReadContext {
  File* file;
};

size_t zipFileReadCallback(void* opaque, mz_uint64 fileOfs, void* pBuf, size_t n) {
  ZipFileReadContext* ctx = (ZipFileReadContext*)opaque;
  if (!ctx->file->seek((uint32_t)fileOfs)) return 0;
  return ctx->file->read((uint8_t*)pBuf, n);
}

void useStreamingReader(mz_zip_archive& zip, ZipFileReadContext& ctx) {
  zip.m_pRead = zipFileReadCallback;
  zip.m_pIO_opaque = &ctx;
}

namespace ProjectInstaller {

String peekProjectDeviceId(const String& zipPath) {
  File zipFile = LittleFS.open(zipPath, "r");
  if (!zipFile) return "";

  size_t zipSize = zipFile.size();
  ZipFileReadContext readCtx{&zipFile};

  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  useDictReservingAllocator(zip);
  useStreamingReader(zip, readCtx);
  if (!mz_zip_reader_init(&zip, zipSize, 0)) {
    zipFile.close();
    return "";
  }

  mz_uint32 fileIndex = 0;
  if (!mz_zip_reader_locate_file_v2(&zip, "project.json", nullptr, 0, &fileIndex)) {
    mz_zip_reader_end(&zip);
    zipFile.close();
    return "";
  }

  mz_zip_archive_file_stat fileStat;
  memset(&fileStat, 0, sizeof(fileStat));
  if (!mz_zip_reader_file_stat(&zip, fileIndex, &fileStat)) {
    mz_zip_reader_end(&zip);
    zipFile.close();
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
    zipFile.close();
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
  zipFile.close();

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

// Mirrors peekProjectDeviceId() above almost verbatim (same zip-open,
// locate, extract, verify steps) rather than sharing a helper with it -
// peekProjectDeviceId() is exercised by main.cpp's JTAG memory-corruption
// probe (see its own comment) and by every real deploy via
// DeployManager.cpp, so it stays untouched here rather than risking a
// refactor neither a compiler nor real hardware was available to verify
// against this session (2026-08-15).
//
// Returns -1 if the file couldn't be read/parsed at all (fails open, same
// as peekProjectDeviceId() returning "" - a peek failure isn't itself
// evidence anything is wrong, installProjectZipFromFile()'s own robust
// parsing is still the real gate). Returns 1 (not -1) when the field is
// simply absent - see docs/nested-provenance.md's "Version compatibility"
// section: an export predating this field is schemaVersion 1 by
// definition, same "optional field, default when omitted" convention as
// everywhere else this project uses it.
long peekProjectSchemaVersion(const String& zipPath) {
  File zipFile = LittleFS.open(zipPath, "r");
  if (!zipFile) return -1;

  size_t zipSize = zipFile.size();
  ZipFileReadContext readCtx{&zipFile};

  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  useDictReservingAllocator(zip);
  useStreamingReader(zip, readCtx);
  if (!mz_zip_reader_init(&zip, zipSize, 0)) {
    zipFile.close();
    return -1;
  }

  mz_uint32 fileIndex = 0;
  if (!mz_zip_reader_locate_file_v2(&zip, "project.json", nullptr, 0, &fileIndex)) {
    mz_zip_reader_end(&zip);
    zipFile.close();
    return -1;
  }

  mz_zip_archive_file_stat fileStat;
  memset(&fileStat, 0, sizeof(fileStat));
  if (!mz_zip_reader_file_stat(&zip, fileIndex, &fileStat)) {
    mz_zip_reader_end(&zip);
    zipFile.close();
    return -1;
  }

  size_t jsonSize = fileStat.m_uncomp_size;
  uint8_t* jsonData = (uint8_t*)malloc(jsonSize);
  if (!jsonData) {
    mz_zip_reader_end(&zip);
    zipFile.close();
    return -1;
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
  zipFile.close();

  if (!extractOk) {
    free(jsonData);
    return -1;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, (const char*)jsonData, jsonSize, DeserializationOption::NestingLimit(30));
  free(jsonData);
  if (err) return -1;

  return doc["schemaVersion"] | 1L;
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
  ZipFileReadContext readCtx{&zipFile};

  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  useDictReservingAllocator(zip);
  useStreamingReader(zip, readCtx);
  if (!mz_zip_reader_init(&zip, zipSize, 0)) {
    zipFile.close();
    errorOut = "Invalid zip archive";
    return false;
  }

  int numFiles = (int)mz_zip_reader_get_num_files(&zip);
  bool success = true;
  for (int i = 0; i < numFiles && success; i++) {
    mz_zip_archive_file_stat file_stat;
    if (!mz_zip_reader_file_stat(&zip, i, &file_stat)) continue;
    if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;

    // Nested-provenance recovery payload (designer repo's
    // docs/nested-provenance.md, "Nesting is zip-in-zip") - an opaque
    // embedded project/DDF blob under this fixed prefix, never needed at
    // runtime and never extracted here. Two reasons, not one: it wastes
    // flash unpacking a second copy of data that already sits safely
    // compressed inside the retained staged zip (see DeployManager.cpp's
    // handling of DEPLOY_DOWNLOAD_PATH), and it would otherwise run
    // through the same fragile DEFLATE-extraction path
    // (mz_zip_reader_extract_iter_*, the 32KB dictionary window above)
    // that a multi-day investigation on 2026-08-09 stabilized against heap
    // fragmentation - no reason to push more bytes through it than the
    // screen actually needs to render.
    if (String(file_stat.m_filename).startsWith("_source/")) continue;

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
  zipFile.close();

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
