// Checkpoint 2a: color rendering pipeline spike. U8g2_for_Adafruit_GFX
// (needed for pixel-accurate BDF-derived text, matching the designer's own
// font rendering exactly like the sibling e-paper firmware) only accepts a
// real Adafruit_GFX&, and M5GFX is a different class hierarchy entirely
// (LovyanGFX-based) - not Adafruit_GFX-compatible. So: render into an
// off-screen ClippedCanvas16 (GFXcanvas16, RGB565 - the color counterpart
// of the e-paper firmware's ClippedCanvas1) via u8g2 exactly like that
// firmware already does, then bulk-blit the finished frame to
// M5Dial.Display via pushImage() once per frame - much faster than the
// e-paper's per-pixel copy loop, which was only necessary there because
// GxEPD2 has no equivalent bulk RGB source push.
//
// This spike only proves the pipeline (canvas -> u8g2 text -> blit) works
// end to end on real hardware before ScreenRenderer/ProjectLoader get
// ported over to use it for real project data.
#include <M5Dial.h>
#include <U8g2_for_Adafruit_GFX.h>
#include "ClippedCanvas16.h"

ClippedCanvas16 canvas(240, 240);
U8G2_FOR_ADAFRUIT_GFX u8g2;

// GFXcanvas16 (this Adafruit_GFX version) has no built-in RGB->565 helper.
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
}

void renderTestFrame() {
  canvas.fillScreen(rgb565(255, 255, 255));

  // Box - M5Stack orange, matching the DDF adornment's accent color.
  canvas.fillRoundRect(20, 20, 200, 60, 8, rgb565(255, 102, 0));

  // Line
  canvas.drawLine(20, 100, 220, 100, rgb565(0, 0, 0));
  canvas.drawLine(20, 101, 220, 101, rgb565(0, 0, 0));

  // Text via u8g2 (BDF-derived font, same u8g2_font_helvR12_tf the DDF
  // declares as font-helvR12's internalName) - the actual pixel-accuracy
  // requirement this whole spike exists to prove out.
  // u8g2_SetFont() unconditionally resets transparency to opaque whenever
  // the font pointer actually changes (see u8g2_for_Adafruit_GFX.cpp's
  // u8g2_SetFont()) - setFont() must always come *before* setFontMode(1),
  // never after, or the "transparent" call gets silently undone the moment
  // a font is selected. Matches the order MqttEPaperDisplay2's
  // ScreenRenderer already uses per-object.
  u8g2.begin(canvas);
  u8g2.setFont(u8g2_font_helvR18_tf);
  u8g2.setFontMode(1);  // transparent - don't overwrite the box's own fill
  u8g2.setForegroundColor(rgb565(255, 255, 255));
  u8g2.setCursor(35, 60);
  u8g2.print("M5 Dial");

  u8g2.setFont(u8g2_font_helvR12_tf);
  u8g2.setFontMode(1);
  u8g2.setForegroundColor(rgb565(0, 0, 0));
  u8g2.setCursor(30, 130);
  u8g2.print("Color canvas -> blit spike");
}

void setup() {
  auto cfg = M5.config();
  M5Dial.begin(cfg, /*enableEncoder=*/true, /*enableRFID=*/false);
  M5Dial.Display.setBrightness(150);

  Serial.begin(115200);
  Serial.println("[M5Dial] Checkpoint 2a - color canvas + u8g2 text + blit spike");

  renderTestFrame();

  M5Dial.Display.startWrite();
  M5Dial.Display.setSwapBytes(true);
  M5Dial.Display.pushImage(0, 0, 240, 240, canvas.getBuffer());
  M5Dial.Display.endWrite();
}

void loop() {
  M5Dial.update();
  delay(10);
}
