#pragma once
#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <U8g2_for_Adafruit_GFX.h>
#include "../interfaces/IProjectLoader.h"
#include "ProjectTypes.h"
#include "../ClippedCanvas16.h"
#include "ColorAssetLoader.h"

// Color (RGB565) counterpart of MqttEPaperDisplay2's ScreenRenderer -
// renders into a ClippedCanvas16 instead of a 1-bit ClippedCanvas1, real
// hex colors instead of black/white thresholding, otherwise mirrors that
// class's carefully HIL-tuned pixel logic (drawTextBox's background/
// border/text order and +1/center-align math, measureTrueTextWidth's
// dry-run measurement technique, u8g2 setFont-before-setFontMode
// ordering) as closely as possible - those fixes are color-depth-
// independent, so there's no reason to rediscover them here.
//
// Every object type in the M5 Dial DDF's supportedObjectTypes is now
// ported: box, line (2-point/multi-point straight segments only - no
// fillet/arrowhead/thick-line yet), label, MqttDataField, level-indicator,
// MQTTIconField, icon, SoftwareButton. icon and SoftwareButton have no
// e-paper equivalent to port from - MqttEPaperDisplay2 never implemented
// either (no touchscreen, and its own "icon" object type was never wired
// up despite existing in the schema) - so their rendering (draw the
// already-exported bitmap via ColorAssetLoader, always the "normal" state
// for SoftwareButton until real touch tracking exists) was designed fresh
// from the designer's own export shape (lib/project-zip.ts's
// path/pathNormal/pathActive fields), not ported from existing firmware
// logic. tab-control/panel and MqttDataLine are permanently out of scope,
// they aren't in the M5 Dial DDF's supportedObjectTypes.
class ColorScreenRenderer {
public:
  ColorScreenRenderer(IProjectLoader& projectLoader, ClippedCanvas16* canvas);

  // Renders every object on the given screen index into the canvas
  // (z-index order, matching sortChildrenByZIndex on the designer side).
  // Does NOT blit to the display - caller does that once after this
  // returns, same as the color-canvas spike (checkpoint 2a).
  // pressedButtonId: if non-empty and it matches a SoftwareButton's own id
  // on this screen, that one button draws pathActive instead of pathNormal
  // - see main.cpp's touch handling for why a full re-render (not a
  // partial single-object update) is used for this: cheap enough on a
  // color LCD with no e-paper ghosting concern to optimize around, same
  // reasoning as the existing MQTT-triggered redraw path.
  bool renderScreen(int screenIndex, const String& pressedButtonId = "");

private:
  IProjectLoader& projectLoader_;
  ClippedCanvas16* canvas_;
  U8G2_FOR_ADAFRUIT_GFX u8g2_;

  bool renderObject(const ScreenObject& obj, const String& pressedButtonId);
  bool renderBox(const ScreenObject& obj);
  bool renderLine(const ScreenObject& obj);
  bool renderLabel(const ScreenObject& obj);
  bool renderMQTTDataField(const ScreenObject& obj);
  bool renderLevelIndicator(const ScreenObject& obj);
  bool renderMQTTIconField(const ScreenObject& obj);
  bool renderIcon(const ScreenObject& obj);
  bool renderSoftwareButton(const ScreenObject& obj, bool isPressed);

  bool evaluateCondition(float value, const String& op, float threshold) const;
  String getIconPathForValue(const ScreenObject& obj, const String& valueStr) const;

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
