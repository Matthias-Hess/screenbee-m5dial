#pragma once
#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <U8g2_for_Adafruit_GFX.h>
#include "../interfaces/IProjectLoader.h"
#include "ProjectTypes.h"
#include "../ClippedCanvas16.h"

// Color (RGB565) counterpart of MqttEPaperDisplay2's ScreenRenderer -
// renders into a ClippedCanvas16 instead of a 1-bit ClippedCanvas1, real
// hex colors instead of black/white thresholding, otherwise mirrors that
// class's carefully HIL-tuned pixel logic (drawTextBox's background/
// border/text order and +1/center-align math, measureTrueTextWidth's
// dry-run measurement technique, u8g2 setFont-before-setFontMode
// ordering) as closely as possible - those fixes are color-depth-
// independent, so there's no reason to rediscover them here.
//
// Only a subset of MqttEPaperDisplay2's object types are ported so far:
// box, line (2-point/multi-point straight segments only - no fillet/
// arrowhead/thick-line yet), label, MqttDataField, level-indicator.
// MQTTIconField, icon and SoftwareButton are still TODO (need real asset/
// BMP loading, not built yet) - tab-control/panel and MqttDataLine are
// permanently out of scope, they aren't in the M5 Dial DDF's
// supportedObjectTypes.
class ColorScreenRenderer {
public:
  ColorScreenRenderer(IProjectLoader& projectLoader, ClippedCanvas16* canvas);

  // Renders every object on the given screen index into the canvas
  // (z-index order, matching sortChildrenByZIndex on the designer side).
  // Does NOT blit to the display - caller does that once after this
  // returns, same as the color-canvas spike (checkpoint 2a).
  bool renderScreen(int screenIndex);

private:
  IProjectLoader& projectLoader_;
  ClippedCanvas16* canvas_;
  U8G2_FOR_ADAFRUIT_GFX u8g2_;

  bool renderObject(const ScreenObject& obj);
  bool renderBox(const ScreenObject& obj);
  bool renderLine(const ScreenObject& obj);
  bool renderLabel(const ScreenObject& obj);
  bool renderMQTTDataField(const ScreenObject& obj);
  bool renderLevelIndicator(const ScreenObject& obj);

  void drawTextBox(const ScreenObject& obj, const String& displayText, bool drawBorder = true);

  const uint8_t* getU8g2FontById(const String& fontId) const;
  int getFontAscentById(const String& fontId) const;
  int getFontDescentById(const String& fontId) const;
  int getFontSizeById(const String& fontId) const;
  int16_t measureTrueTextWidth(const String& text, const uint8_t* u8font);
  String formatNumber(const String& valueStr, int decimals, const String& thousandsSeparator) const;
  float interpolateCalibration(float value, const std::vector<CalibrationPoint>& points) const;

  // Parses "#rrggbb" (or "#rrggbbaa", alpha ignored except to detect
  // full transparency) into a real RGB565 value - unlike the e-paper
  // firmware's parseColor(), which only ever needed to decide black vs.
  // white. "transparent"/"#ffffff00"/similar all resolve to isTransparent
  // = true via the out-param, matching drawTextBox's existing
  // transparent-detection conventions (see its own comment for the exact
  // string set this mirrors).
  uint16_t parseHexColor(const String& colorStr, bool* isTransparent = nullptr) const;
};
