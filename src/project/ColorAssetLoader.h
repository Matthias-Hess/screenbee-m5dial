#pragma once
#include <Arduino.h>
#include "../ClippedCanvas16.h"

// Decodes a 24bpp BMP asset from LittleFS and draws it onto a
// ClippedCanvas16, converting BGR888 -> RGB565 per pixel. Matches
// lib/asset-export.ts's bitmapToBMP24Bit() output exactly (54-byte
// header, DIB header size 40, bottom-up row order, rows padded to a
// 4-byte boundary, BGR pixel order) - the same format every "24bit"
// colorDepth project's icon/SoftwareButton assets are actually exported
// as, so decoding this is decoding the real thing, not a stand-in format.
// Deliberately narrower than MqttEPaperDisplay2's AssetManager (which
// only ever needed 1bpp) - this only needs to handle what the M5 Dial DDF
// actually declares (colorDepth: "24bit"), so 24bpp is the only format
// supported; anything else fails loudly (returns false) rather than
// silently misinterpreting bytes.
namespace ColorAssetLoader {

bool drawBMPToCanvas(ClippedCanvas16* canvas, const String& path, int16_t x, int16_t y);

}
