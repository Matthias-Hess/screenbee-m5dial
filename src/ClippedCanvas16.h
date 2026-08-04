#pragma once
#include <Adafruit_GFX.h>

// GFXcanvas16 (RGB565) with an optional clip rectangle - the color
// (24bit/RGB565) counterpart to MqttEPaperDisplay2's ClippedCanvas1
// (1-bit/monochrome). Same reasoning applies: U8g2_for_Adafruit_GFX (and
// every other Adafruit_GFX drawing primitive) ultimately draws through
// drawPixel()/drawFastVLine()/drawFastHLine(), so overriding those here is
// enough to clip glyph rendering to an object's own bounds - Adafruit_GFX/
// U8g2_for_Adafruit_GFX have no built-in clip-window concept otherwise.
//
// setClip()/clearClip() are meant to bracket just the text-drawing calls
// for a single object, not the whole screen render - background fills and
// border strokes are already sized exactly to their object's bounds and
// don't need clipping.
class ClippedCanvas16 : public GFXcanvas16 {
public:
  ClippedCanvas16(int16_t w, int16_t h) : GFXcanvas16(w, h) {}

  void setClip(int16_t x, int16_t y, int16_t w, int16_t h) {
    clipActive_ = true;
    clipX_ = x;
    clipY_ = y;
    clipW_ = w;
    clipH_ = h;
  }

  void clearClip() {
    clipActive_ = false;
  }

  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (clipActive_ && (x < clipX_ || x >= clipX_ + clipW_ || y < clipY_ || y >= clipY_ + clipH_)) {
      return;
    }
    GFXcanvas16::drawPixel(x, y, color);
  }

  // GFXcanvas16 provides its own direct-to-buffer overrides of both these
  // (never routed through drawPixel) - see ClippedCanvas1.h's identical
  // comment for why overriding drawPixel alone would silently clip nothing
  // for text.
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override {
    if (clipActive_) {
      if (h < 0) {
        h = (int16_t)(-h);
        y = (int16_t)(y - (h - 1));
      }
      if (x < clipX_ || x >= clipX_ + clipW_) return;
      int16_t y0 = y < clipY_ ? clipY_ : y;
      int16_t y1 = (int16_t)(y + h) > (int16_t)(clipY_ + clipH_) ? (int16_t)(clipY_ + clipH_) : (int16_t)(y + h);
      if (y1 <= y0) return;
      y = y0;
      h = (int16_t)(y1 - y0);
    }
    GFXcanvas16::drawFastVLine(x, y, h, color);
  }

  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override {
    if (clipActive_) {
      if (w < 0) {
        w = (int16_t)(-w);
        x = (int16_t)(x - (w - 1));
      }
      if (y < clipY_ || y >= clipY_ + clipH_) return;
      int16_t x0 = x < clipX_ ? clipX_ : x;
      int16_t x1 = (int16_t)(x + w) > (int16_t)(clipX_ + clipW_) ? (int16_t)(clipX_ + clipW_) : (int16_t)(x + w);
      if (x1 <= x0) return;
      x = x0;
      w = (int16_t)(x1 - x0);
    }
    GFXcanvas16::drawFastHLine(x, y, w, color);
  }

private:
  bool clipActive_ = false;
  int16_t clipX_ = 0;
  int16_t clipY_ = 0;
  int16_t clipW_ = 0;
  int16_t clipH_ = 0;
};
