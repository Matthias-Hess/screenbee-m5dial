#include "M5DisplayAdapter.h"
#include <M5Dial.h>

void M5DisplayAdapter::showLines(std::initializer_list<String> lines) {
  M5Dial.Display.fillScreen(TFT_BLACK);
  M5Dial.Display.setTextDatum(top_center);
  M5Dial.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Dial.Display.setTextFont(&fonts::FreeSans9pt7b);
  M5Dial.Display.setTextSize(1);

  int16_t y = 12;
  const int16_t lineHeight = 20;
  for (const String& line : lines) {
    M5Dial.Display.drawString(line, M5Dial.Display.width() / 2, y);
    y += lineHeight;
  }
}

void M5DisplayAdapter::clear() {
  M5Dial.Display.fillScreen(TFT_BLACK);
}
