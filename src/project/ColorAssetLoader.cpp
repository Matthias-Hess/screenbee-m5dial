#include "ColorAssetLoader.h"
#include <LittleFS.h>

namespace ColorAssetLoader {

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

}  // namespace ColorAssetLoader
