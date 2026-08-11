#include "ColorAssetLoader.h"
#include <LittleFS.h>
#include <vector>

namespace ColorAssetLoader {

namespace {
// Raw-bytes cache for drawGrayscaleMaskToCanvas() - added 2026-08-11 after
// ScreenNavigatorOverlay's continuous encoder-coupled scrolling made "smooth
// but laggy" visible on real hardware: with several tablets on-canvas at
// once, every single animation frame was re-opening and re-reading each
// one's PGM file from LittleFS from scratch, even though the same handful
// of icons repeat every frame while a project is loaded. Icon bytes never
// change at runtime, so caching them in RAM after the first read removes
// the flash I/O from the per-frame cost entirely - only the (cheap)
// per-pixel blend still runs every frame. A simple linear-scan vector, not
// a hash map: the number of distinct icon paths is bounded by screen count,
// which is small (tens, not thousands) for any project this firmware runs.
struct CacheEntry {
  String path;
  int32_t width = 0;
  int32_t height = 0;
  std::vector<uint8_t> gray;
};
std::vector<CacheEntry> maskCache;

// Returns the cached raw grayscale bytes for path, reading+caching from
// LittleFS on first request for that path. nullptr if the file can't be
// read or isn't a well-formed P5 PGM.
const CacheEntry* getCachedMask(const String& path) {
  for (const auto& entry : maskCache) {
    if (entry.path == path) return &entry;
  }

  File pgmFile = LittleFS.open(path, "r");
  if (!pgmFile) {
    Serial.printf("[ColorAssetLoader] Could not open \"%s\"\n", path.c_str());
    return nullptr;
  }

  String magic = pgmFile.readStringUntil('\n');
  magic.trim();
  if (magic != "P5") {
    Serial.printf("[ColorAssetLoader] \"%s\" is not a P5 PGM (bad magic \"%s\")\n", path.c_str(), magic.c_str());
    pgmFile.close();
    return nullptr;
  }

  // Skip comment lines ("#...") between the magic and the dimensions line -
  // same allowance the e-paper firmware's own PBM parser makes, even though
  // nothing in this pipeline actually emits comments today.
  String line;
  do {
    line = pgmFile.readStringUntil('\n');
    line.trim();
  } while (line.startsWith("#"));

  int spacePos = line.indexOf(' ');
  int32_t width = line.substring(0, spacePos).toInt();
  int32_t height = line.substring(spacePos + 1).toInt();

  // Maxval line - always "255" from bitmapToPGM(), the only producer of
  // this format today. Read and discard rather than hardcode-skip a fixed
  // byte count, so a stray comment line here (unlikely, but PGM technically
  // allows one) doesn't desync the parser.
  pgmFile.readStringUntil('\n');

  CacheEntry entry;
  entry.path = path;
  entry.width = width;
  entry.height = height;
  entry.gray.resize((size_t)width * (size_t)height);
  pgmFile.readBytes((char*)entry.gray.data(), entry.gray.size());
  pgmFile.close();

  maskCache.push_back(std::move(entry));
  return &maskCache.back();
}
}  // namespace

bool drawBMPToCanvas(ClippedCanvas16* canvas, const String& path, int16_t x, int16_t y) {
  if (path.isEmpty()) return false;

  File bmpFile = LittleFS.open(path, "r");
  if (!bmpFile) {
    Serial.printf("[ColorAssetLoader] Could not open \"%s\"\n", path.c_str());
    return false;
  }

  uint16_t signature = bmpFile.read() | (bmpFile.read() << 8);
  if (signature != 0x4D42) {  // 'BM'
    Serial.printf("[ColorAssetLoader] \"%s\" is not a BMP (bad signature)\n", path.c_str());
    bmpFile.close();
    return false;
  }

  bmpFile.seek(10);
  uint32_t dataOffset = bmpFile.read() | (bmpFile.read() << 8) | (bmpFile.read() << 16) | ((uint32_t)bmpFile.read() << 24);

  bmpFile.seek(18);
  int32_t width = bmpFile.read() | (bmpFile.read() << 8) | (bmpFile.read() << 16) | (bmpFile.read() << 24);
  int32_t height = bmpFile.read() | (bmpFile.read() << 8) | (bmpFile.read() << 16) | (bmpFile.read() << 24);

  bmpFile.seek(28);
  uint16_t bpp = bmpFile.read() | (bmpFile.read() << 8);

  if (bpp != 24) {
    Serial.printf("[ColorAssetLoader] \"%s\" is %dbpp, only 24bpp is supported\n", path.c_str(), bpp);
    bmpFile.close();
    return false;
  }

  uint32_t rowSize = ((width * 3 + 3) / 4) * 4;  // padded to 4-byte boundary

  bool flip = height > 0;  // positive height = bottom-up (the normal case)
  if (height < 0) height = -height;

  uint8_t* rowBuffer = (uint8_t*)malloc(rowSize);
  if (!rowBuffer) {
    Serial.println("[ColorAssetLoader] rowBuffer malloc failed");
    bmpFile.close();
    return false;
  }

  for (int32_t row = 0; row < height; row++) {
    int32_t readRow = flip ? (height - 1 - row) : row;
    bmpFile.seek(dataOffset + (readRow * rowSize));
    bmpFile.readBytes((char*)rowBuffer, rowSize);

    for (int32_t col = 0; col < width; col++) {
      uint8_t b = rowBuffer[col * 3];
      uint8_t g = rowBuffer[col * 3 + 1];
      uint8_t r = rowBuffer[col * 3 + 2];
      uint16_t rgb565 = ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
      canvas->drawPixel(x + col, y + row, rgb565);
    }
  }

  free(rowBuffer);
  bmpFile.close();
  return true;
}

bool drawGrayscaleMaskToCanvas(ClippedCanvas16* canvas, const String& path, int16_t x, int16_t y, uint16_t fgColor, uint16_t bgColor) {
  if (path.isEmpty()) return false;

  const CacheEntry* entry = getCachedMask(path);
  if (!entry) return false;

  // Unpacked once, outside the pixel loop - RGB565: bits [15:11]=R5,
  // [10:5]=G6, [4:0]=B5.
  uint16_t fgR = (fgColor >> 11) & 0x1F, fgG = (fgColor >> 5) & 0x3F, fgB = fgColor & 0x1F;
  uint16_t bgR = (bgColor >> 11) & 0x1F, bgG = (bgColor >> 5) & 0x3F, bgB = bgColor & 0x1F;

  for (int32_t row = 0; row < entry->height; row++) {
    for (int32_t col = 0; col < entry->width; col++) {
      uint8_t gray = entry->gray[(size_t)row * entry->width + col];  // 0 = full fgColor, 255 = full bgColor (see convertToGrayscale())
      if (gray == 255) continue;  // fully background - skip, the tablet fill already drawn there is correct as-is
      uint16_t r = (fgR * (255 - gray) + bgR * gray) / 255;
      uint16_t g = (fgG * (255 - gray) + bgG * gray) / 255;
      uint16_t b = (fgB * (255 - gray) + bgB * gray) / 255;
      canvas->drawPixel(x + col, y + row, (r << 11) | (g << 5) | b);
    }
  }

  return true;
}

void clearGrayscaleMaskCache() {
  maskCache.clear();
}

}  // namespace ColorAssetLoader
