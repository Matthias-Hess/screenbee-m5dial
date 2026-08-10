#include "M5DisplayAdapter.h"
#include <M5Dial.h>

void M5DisplayAdapter::showLines(std::initializer_list<String> lines) {
  M5Dial.Display.fillScreen(TFT_BLACK);
  M5Dial.Display.setTextDatum(top_center);
  M5Dial.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  // fonts::Font2 (M5GFX's compact built-in bitmap font, ~16px line height)
  // instead of FreeSans9pt7b - the screen is round (240px GC9A01), so the
  // usable *width* shrinks sharply away from vertical center, and
  // FreeSans9pt7b's wider glyphs plus taller line spacing pushed the
  // longer status lines (SSID, the setup URL) far enough toward the top/
  // bottom that the round bezel clipped them - found 2026-08-10 once the
  // AP setup screen grew past its original ~9 lines. Font2 is narrower per
  // character and the tighter 16px line height keeps every line closer to
  // the wide middle band.
  M5Dial.Display.setTextFont(&fonts::Font2);
  M5Dial.Display.setTextSize(1);

  int16_t y = 12;
  const int16_t lineHeight = 16;
  for (const String& line : lines) {
    M5Dial.Display.drawString(line, M5Dial.Display.width() / 2, y);
    y += lineHeight;
  }
}

void M5DisplayAdapter::clear() {
  M5Dial.Display.fillScreen(TFT_BLACK);
}
