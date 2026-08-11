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

// Decodes an 8-bit PGM (P5, binary/raw) grayscale mask and draws it
// blended per-pixel between `bgColor` (white=255 in the source, fully
// background) and `fgColor` (black=0, the icon's full foreground color) -
// see lib/asset-converter.ts's convertToGrayscale()/bitmapToPGM() for the
// exact source format this decodes. Originally a hard 1-bit PBM stencil
// (draw fgColor or draw nothing) - replaced 2026-08-11 because a hard
// per-pixel on/off choice threw away the antialiasing the designer's own
// SVG rasterization already produced, which looked visibly blocky at the
// small sizes the M5 Dial's screen-switch navigator tablets actually use
// (see ScreenNavigatorOverlay). Blends against a caller-supplied bgColor
// rather than reading the canvas back, because the caller (drawTablets())
// already knows exactly which color it just filled the tablet with -
// cheaper and simpler than a real read-modify-blend over whatever's
// already drawn.
bool drawGrayscaleMaskToCanvas(ClippedCanvas16* canvas, const String& path, int16_t x, int16_t y, uint16_t fgColor, uint16_t bgColor);

// Drops drawGrayscaleMaskToCanvas()'s in-RAM icon-bytes cache. Icon paths
// are screen-id-based, not content-hashed (see lib/asset-export.ts's
// exportPageIcon()), so the same path can point at different bytes after a
// redeploy - call this whenever a project is (re)loaded so a changed icon
// doesn't keep showing its old cached pixels.
void clearGrayscaleMaskCache();

}
