#include "ColorScreenRenderer.h"
#include <algorithm>

// Matches JavaScript's Math.round() exactly, including its tie-breaking
// direction (halves round toward +infinity) - see MqttEPaperDisplay2's
// ScreenRenderer.cpp's identical helper for the full rationale (a naive
// roundf() port disagreed with the designer's center-align formula by 1px
// for negative odd width differences).
static inline int16_t jsRound(float v) {
  return (int16_t)floorf(v + 0.5f);
}

ColorScreenRenderer::ColorScreenRenderer(IProjectLoader& projectLoader, ClippedCanvas16* canvas)
  : projectLoader_(projectLoader), canvas_(canvas) {
  // Without this, u8g2_'s internal gfx pointer stays uninitialized - every
  // other setFont/setCursor/print call on it silently "worked" (they just
  // write into the u8g2_font_t struct), but the first real glyph draw
  // dereferences that null/garbage pointer and crashes
  // (Guru Meditation LoadProhibited, confirmed live via serial log +
  // addr2line: u8g2_draw_hv_line -> ... -> drawTextBox -> u8g2_.print()).
  u8g2_.begin(*canvas_);
}

uint16_t ColorScreenRenderer::parseHexColor(const String& colorStr, bool* isTransparent) const {
  if (isTransparent) *isTransparent = false;

  String s = colorStr;
  s.trim();
  s.toLowerCase();

  if (s.isEmpty() || s == "transparent" || s == "#ffffff00" || s == "#00000000" ||
      s == "rgba(255,255,255,0)" || s == "rgba(0,0,0,0)") {
    if (isTransparent) *isTransparent = true;
    return 0;
  }
  // "#rrggbbaa" with alpha byte 00 is also fully transparent.
  if (s.length() == 9 && s.charAt(0) == '#') {
    String alpha = s.substring(7, 9);
    if (alpha == "00") {
      if (isTransparent) *isTransparent = true;
    }
  }

  if (s == "black") return 0x0000;
  if (s == "white") return 0xFFFF;

  if (s.length() >= 7 && s.charAt(0) == '#') {
    uint8_t r = strtol(s.substring(1, 3).c_str(), nullptr, 16);
    uint8_t g = strtol(s.substring(3, 5).c_str(), nullptr, 16);
    uint8_t b = strtol(s.substring(5, 7).c_str(), nullptr, 16);
    return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
  }

  // Unrecognized - black, same fallback the e-paper firmware's parseColor()
  // uses for anything it can't parse.
  return 0x0000;
}

const uint8_t* ColorScreenRenderer::getU8g2FontById(const String& fontId) const {
  const ProjectConfig& project = projectLoader_.getProject();
  for (const Font& font : project.fonts) {
    if (font.id == fontId) {
      if (font.internalName == "u8g2_font_helvR08_tf") return u8g2_font_helvR08_tf;
      if (font.internalName == "u8g2_font_helvR12_tf") return u8g2_font_helvR12_tf;
      if (font.internalName == "u8g2_font_helvR18_tf") return u8g2_font_helvR18_tf;
      if (font.internalName == "u8g2_font_helvR24_tf") return u8g2_font_helvR24_tf;
    }
  }
  return u8g2_font_helvR18_tf;
}

int ColorScreenRenderer::getFontAscentById(const String& fontId) const {
  const ProjectConfig& project = projectLoader_.getProject();
  for (const Font& font : project.fonts) {
    if (font.id == fontId) return font.ascent;
  }
  return 14;
}

int ColorScreenRenderer::getFontDescentById(const String& fontId) const {
  const ProjectConfig& project = projectLoader_.getProject();
  for (const Font& font : project.fonts) {
    if (font.id == fontId) return font.descent;
  }
  return 4;
}

int ColorScreenRenderer::getFontSizeById(const String& fontId) const {
  const ProjectConfig& project = projectLoader_.getProject();
  for (const Font& font : project.fonts) {
    if (font.id == fontId) return font.size;
  }
  return 18;
}

String ColorScreenRenderer::formatNumber(const String& valueStr, int decimals, const String& thousandsSeparator) const {
  float value = valueStr.toFloat();
  String formatted = String(value, decimals);

  if (!thousandsSeparator.isEmpty()) {
    int decimalPos = formatted.indexOf('.');
    if (decimalPos == -1) decimalPos = formatted.length();

    int insertPos = decimalPos - 3;
    while (insertPos > 0) {
      if (insertPos == 1 && formatted.charAt(0) == '-') break;
      formatted = formatted.substring(0, insertPos) + thousandsSeparator + formatted.substring(insertPos);
      insertPos -= 3;
      decimalPos += thousandsSeparator.length();
    }
  }
  return formatted;
}

float ColorScreenRenderer::interpolateCalibration(float value, const std::vector<CalibrationPoint>& points) const {
  if (points.empty()) return value;
  if (points.size() == 1) return points[0].barSizePercent;

  std::vector<CalibrationPoint> sortedPoints = points;
  std::sort(sortedPoints.begin(), sortedPoints.end(),
            [](const CalibrationPoint& a, const CalibrationPoint& b) { return a.value < b.value; });

  if (value <= sortedPoints[0].value) return sortedPoints[0].barSizePercent;
  if (value >= sortedPoints[sortedPoints.size() - 1].value) return sortedPoints[sortedPoints.size() - 1].barSizePercent;

  for (size_t i = 0; i + 1 < sortedPoints.size(); i++) {
    if (value >= sortedPoints[i].value && value <= sortedPoints[i + 1].value) {
      float x1 = sortedPoints[i].value, y1 = sortedPoints[i].barSizePercent;
      float x2 = sortedPoints[i + 1].value, y2 = sortedPoints[i + 1].barSizePercent;
      float percent = (value - x1) / (x2 - x1);
      return y1 + percent * (y2 - y1);
    }
  }
  return 0;
}

// See MqttEPaperDisplay2's ScreenRenderer::measureTrueTextWidth() for why
// u8g2_.getUTF8Width() isn't used directly (it disagrees with the real
// glyph-drawing cursor advance by 1px) and why this uses a throwaway
// U8G2_FOR_ADAFRUIT_GFX instance rather than the shared member.
int16_t ColorScreenRenderer::measureTrueTextWidth(const String& text, const uint8_t* u8font) {
  U8G2_FOR_ADAFRUIT_GFX measureU8g2;
  measureU8g2.begin(*canvas_);
  measureU8g2.setFont(u8font);
  measureU8g2.setFontMode(1);

  canvas_->setClip(0, 0, 0, 0);
  measureU8g2.setCursor(0, 0);
  measureU8g2.print(text);
  int16_t width = measureU8g2.getCursorX();
  canvas_->clearClip();

  return width;
}

void ColorScreenRenderer::drawTextBox(const ScreenObject& obj, const String& displayText, bool drawBorder) {
  const uint8_t* u8font = getU8g2FontById(obj.properties.fontId);
  // setFont() must come before setFontMode(1) - u8g2_SetFont() resets
  // transparency to opaque whenever the font pointer changes (see
  // checkpoint 2a's commit message for how this was found live on
  // hardware, and MqttEPaperDisplay2's identical 2026-07-20 finding).
  u8g2_.setFont(u8font);
  u8g2_.setFontMode(1);

  int fontAscent = getFontAscentById(obj.properties.fontId);
  // Box height for background/border/clip comes from the font's own size
  // (ascent+descent), not obj.height - matches the designer's
  // boundingBoxHeight, not the independently-resizable JSON height field.
  int boxHeight = getFontSizeById(obj.properties.fontId);

  String textColorStr = obj.properties.color.isEmpty() ? obj.properties.textColor : obj.properties.color;
  uint16_t textColor = parseHexColor(textColorStr);

  bool bgTransparent = false;
  uint16_t bgColor = parseHexColor(obj.properties.backgroundColor, &bgTransparent);

  u8g2_.setForegroundColor(textColor);
  u8g2_.setBackgroundColor(bgColor);

  // Background -> border -> text, in that order (text last, always on top)
  // - matches the designer's draw order exactly, see MqttEPaperDisplay2's
  // drawTextBox() comment for the HIL mismatch this order avoids.
  if (!bgTransparent) {
    canvas_->fillRect(obj.x, obj.y, obj.width, boxHeight, bgColor);
  }

  if (drawBorder) {
    bool borderTransparent = false;
    uint16_t borderColor = parseHexColor(obj.properties.borderColor, &borderTransparent);
    if (!obj.properties.borderColor.isEmpty() && !borderTransparent) {
      canvas_->drawRect(obj.x, obj.y, obj.width, boxHeight, borderColor);
    }
  }

  int16_t baselineY = obj.y + fontAscent;
  int16_t textWidth = measureTrueTextWidth(displayText, u8font);

  int16_t textX = obj.x + 1;
  if (obj.properties.textAlign == "center") {
    textX = obj.x + 1 + jsRound((obj.width - textWidth) / 2.0f);
  } else if (obj.properties.textAlign == "right") {
    textX = obj.x + 1 + obj.width - textWidth;
  }

  canvas_->setClip(obj.x, obj.y, obj.width, boxHeight);
  u8g2_.setCursor(textX, baselineY);
  u8g2_.print(displayText);
  canvas_->clearClip();
}

bool ColorScreenRenderer::renderLabel(const ScreenObject& obj) {
  drawTextBox(obj, obj.properties.text, true);
  return true;
}

bool ColorScreenRenderer::renderBox(const ScreenObject& obj) {
  // fillColor, not backgroundColor - box's own fill is a distinct property
  // from backgroundColor (used by label/MqttDataField's text-box
  // background), matching render-box.ts's identical `obj.properties.
  // fillColor` read on the designer side and ObjectProperties.fillColor's
  // own "Color of filled portion" field. Reading backgroundColor here
  // instead (defaulting to opaque "#ffffff" per parseObjectProperties())
  // meant every box with a border rendered a white interior no matter what
  // fillColor actually said, never transparent even when fillColor was
  // never set at all - found 2026-08-10 building the M5 Dial HIL fixture.
  bool fillTransparent = false;
  uint16_t fillColor = parseHexColor(obj.properties.fillColor, &fillTransparent);
  bool strokeTransparent = false;
  uint16_t strokeColor = parseHexColor(obj.properties.strokeColor, &strokeTransparent);
  int strokeWidth = obj.properties.strokeWidth;
  int cornerRadius = obj.properties.cornerRadius;
  if (cornerRadius < 0) cornerRadius = 0;
  bool hasBorder = strokeWidth > 0 && !obj.properties.strokeColor.isEmpty() && !strokeTransparent;

  if (fillTransparent && !hasBorder) return true;

  // The e-paper original's technique (fill the whole box with border color,
  // then paint a smaller inner rect in fill color to "cut out" the
  // interior) only works when fill color is a real paintable color - that
  // firmware's parseColor() never had a transparent concept at all (a
  // monochrome canvas is always definitely black or white). Color mode
  // does have one, and painting a transparent interior by "cutting it out"
  // after already painting over it isn't possible - a transparent-fill
  // bordered box has to draw ONLY the border outline instead, leaving the
  // interior untouched (found live on hardware: a bordered box with a
  // transparent fill rendered fully filled with its own border color,
  // hiding an orange label text drawn on top of it).
  if (fillTransparent) {
    for (int i = 0; i < strokeWidth; i++) {
      int ringRadius = cornerRadius - i;
      if (ringRadius < 0) ringRadius = 0;
      int ringW = obj.width - 2 * i;
      int ringH = obj.height - 2 * i;
      if (ringW <= 0 || ringH <= 0) break;
      if (ringRadius > 0) {
        canvas_->drawRoundRect(obj.x + i, obj.y + i, ringW, ringH, ringRadius, strokeColor);
      } else {
        canvas_->drawRect(obj.x + i, obj.y + i, ringW, ringH, strokeColor);
      }
    }
    return true;
  }

  if (cornerRadius > 0) {
    canvas_->fillRoundRect(obj.x, obj.y, obj.width, obj.height, cornerRadius, hasBorder ? strokeColor : fillColor);
  } else {
    canvas_->fillRect(obj.x, obj.y, obj.width, obj.height, hasBorder ? strokeColor : fillColor);
  }

  if (hasBorder) {
    int innerX = obj.x + strokeWidth;
    int innerY = obj.y + strokeWidth;
    int innerWidth = obj.width - 2 * strokeWidth;
    int innerHeight = obj.height - 2 * strokeWidth;
    int innerRadius = cornerRadius - strokeWidth;
    if (innerRadius < 0) innerRadius = 0;
    if (innerWidth > 0 && innerHeight > 0) {
      if (innerRadius > 0) {
        canvas_->fillRoundRect(innerX, innerY, innerWidth, innerHeight, innerRadius, fillColor);
      } else {
        canvas_->fillRect(innerX, innerY, innerWidth, innerHeight, fillColor);
      }
    }
  }
  return true;
}

// Straight-segment lines only so far (properties.points if set, else the
// (x,y)-to-(x+width,y+height) fallback - matches the designer's
// getLinePoints()) - no fillet/arrowhead/thick-line yet, tracked as
// follow-up work alongside the other not-yet-ported object types (see
// this class's header comment).
bool ColorScreenRenderer::renderLine(const ScreenObject& obj) {
  uint16_t color = parseHexColor(obj.properties.color);

  std::vector<Point> points = obj.properties.points;
  if (points.size() < 2) {
    points.clear();
    points.push_back({obj.x, obj.y});
    points.push_back({obj.x + obj.width, obj.y + obj.height});
  }

  for (size_t i = 0; i + 1 < points.size(); i++) {
    canvas_->drawLine(points[i].x, points[i].y, points[i + 1].x, points[i + 1].y, color);
  }
  return true;
}

bool ColorScreenRenderer::renderMQTTDataField(const ScreenObject& obj) {
  String valueStr = projectLoader_.getTopicValue(obj.properties.topic);

  String trimmedCheck = valueStr;
  trimmedCheck.trim();
  bool isBlank = trimmedCheck.isEmpty();
  if (isBlank) valueStr = "";

  String formattedValue = valueStr;
  if (!isBlank && obj.properties.displayAs == "Formatted Number") {
    formattedValue = formatNumber(valueStr, obj.properties.numberOfDecimals, obj.properties.thousandsSeparator);
  }

  String displayText = obj.properties.prefix + formattedValue + obj.properties.postfix;
  drawTextBox(obj, displayText, true);
  return true;
}

// Same bar/text layout math as MqttEPaperDisplay2's renderLevelIndicator()
// (padding=4, direction handling, center-align via a single combined
// jsRound division, ascent+descent-based vertical centering) - all of
// that is color-depth-independent. What differs: that firmware's
// "text visible against both the bar and its background" effect is a
// 1-bit pixel-invert trick (draw a tiny GFXcanvas1, then swap every 0<->1
// while blitting) - there's no equivalent "invert" for arbitrary RGB565
// colors. The color equivalent here draws the text twice instead: once
// across the whole object in fillColor, then the bar rect painted solid
// over it (erasing that portion), then the SAME text redrawn a second
// time but clipped to just the bar rect, in bgColor - same visual result
// (fillColor text outside the bar, bgColor text inside it), no invert
// needed.
bool ColorScreenRenderer::renderLevelIndicator(const ScreenObject& obj) {
  String valueStr = projectLoader_.getTopicValue(obj.properties.topic);
  float value = valueStr.isEmpty() ? 0.0f : valueStr.toFloat();

  float fillPercent = interpolateCalibration(value, obj.properties.calibrationPoints);
  if (fillPercent < 0) fillPercent = 0;
  if (fillPercent > 100) fillPercent = 100;

  bool bgTransparent = false;
  uint16_t bgColor = parseHexColor(obj.properties.backgroundColor, &bgTransparent);
  uint16_t fillColor = parseHexColor(obj.properties.fillColor);
  bool borderTransparent = false;
  uint16_t borderColor = parseHexColor(obj.properties.borderColor, &borderTransparent);

  if (!bgTransparent) {
    canvas_->fillRect(obj.x, obj.y, obj.width, obj.height, bgColor);
  }
  if (!borderTransparent) {
    canvas_->drawRect(obj.x, obj.y, obj.width, obj.height, borderColor);
  }

  const int padding = 4;
  int innerX = obj.x + padding;
  int innerY = obj.y + padding;
  int innerWidth = obj.width - padding * 2;
  int innerHeight = obj.height - padding * 2;

  int fillWidth = innerWidth, fillHeight = innerHeight, fillX = innerX, fillY = innerY;
  if (obj.properties.barDirection == "left-to-right") {
    fillWidth = (innerWidth * fillPercent) / 100;
  } else if (obj.properties.barDirection == "right-to-left") {
    fillWidth = (innerWidth * fillPercent) / 100;
    fillX = innerX + innerWidth - fillWidth;
  } else if (obj.properties.barDirection == "bottom-to-top") {
    fillHeight = (innerHeight * fillPercent) / 100;
    fillY = innerY + innerHeight - fillHeight;
  } else if (obj.properties.barDirection == "top-to-bottom") {
    fillHeight = (innerHeight * fillPercent) / 100;
  }
  int barX = fillX, barY = fillY, barWidth = fillWidth, barHeight = fillHeight;

  if (obj.properties.displayValue == "none") return true;

  String displayText;
  if (obj.properties.displayValue == "percentage") {
    displayText = String((int)(fillPercent + 0.5f)) + "%";
  } else if (obj.properties.displayValue == "value") {
    displayText = valueStr;
  }
  if (displayText.isEmpty()) return true;

  const uint8_t* u8font = getU8g2FontById(obj.properties.fontId);
  u8g2_.setFont(u8font);
  u8g2_.setFontMode(1);

  int16_t textWidth = measureTrueTextWidth(displayText, u8font);
  int16_t fontAscent = getFontAscentById(obj.properties.fontId);
  int16_t fontDescent = getFontDescentById(obj.properties.fontId);

  int16_t textX = obj.x + 1 + jsRound((obj.width - textWidth) / 2.0f);
  int16_t fontHeight = fontAscent + fontDescent;
  int16_t verticalCenterOffset = jsRound((obj.height - fontHeight) / 2.0f);
  int16_t textY = obj.y + verticalCenterOffset + fontAscent;

  u8g2_.setFontDirection(0);

  // Pass 1: text in fillColor across the whole object.
  u8g2_.setForegroundColor(fillColor);
  canvas_->setClip(obj.x, obj.y, obj.width, obj.height);
  u8g2_.setCursor(textX, textY);
  u8g2_.print(displayText);
  canvas_->clearClip();

  // Bar, painted solid over whatever text pass 1 drew there.
  if (barWidth > 0 && barHeight > 0) {
    canvas_->fillRect(barX, barY, barWidth, barHeight, fillColor);

    // Pass 2: same text again, clipped to just the bar, in bgColor - font
    // is unchanged since pass 1 (same pointer), so no setFont() call here
    // (that would silently reset transparency - see this class's other
    // setFont-before-setFontMode comments for why).
    u8g2_.setForegroundColor(bgColor);
    canvas_->setClip(barX, barY, barWidth, barHeight);
    u8g2_.setCursor(textX, textY);
    u8g2_.print(displayText);
    canvas_->clearClip();
  }

  return true;
}

bool ColorScreenRenderer::evaluateCondition(float value, const String& op, float threshold) const {
  if (op == ">") return value > threshold;
  if (op == ">=") return value >= threshold;
  if (op == "<") return value < threshold;
  if (op == "<=") return value <= threshold;
  // MQTTIconField's valueIconPairs use a single "=" (see the designer's
  // mqtt-icon-field-properties.tsx) - both spellings accepted, matching
  // MqttEPaperDisplay2's identical fix for this exact confusion.
  if (op == "==" || op == "=") return value == threshold;
  if (op == "!=") return value != threshold;
  return false;
}

String ColorScreenRenderer::getIconPathForValue(const ScreenObject& obj, const String& valueStr) const {
  float value = valueStr.toFloat();
  for (const auto& pair : obj.properties.valueIconPairs) {
    if (evaluateCondition(value, pair.comparisonOperator, pair.value)) {
      return pair.path;
    }
  }
  return "";
}

bool ColorScreenRenderer::renderMQTTIconField(const ScreenObject& obj) {
  String valueStr = projectLoader_.getTopicValue(obj.properties.topic);
  if (valueStr.isEmpty()) return false;

  String iconPath = getIconPathForValue(obj, valueStr);
  if (iconPath.isEmpty()) return false;

  if (!ColorAssetLoader::drawBMPToCanvas(canvas_, iconPath, obj.x, obj.y)) {
    // Placeholder so a missing/bad asset is visibly wrong, not invisible.
    canvas_->fillRect(obj.x + 2, obj.y + 2, obj.width - 4, obj.height - 4, 0x0000);
    canvas_->drawRect(obj.x, obj.y, obj.width, obj.height, 0x0000);
  }
  return true;
}

bool ColorScreenRenderer::renderIcon(const ScreenObject& obj) {
  if (obj.path.isEmpty()) return false;
  if (!ColorAssetLoader::drawBMPToCanvas(canvas_, obj.path, obj.x, obj.y)) {
    canvas_->fillRect(obj.x + 2, obj.y + 2, obj.width - 4, obj.height - 4, 0x0000);
    canvas_->drawRect(obj.x, obj.y, obj.width, obj.height, 0x0000);
    return false;
  }
  return true;
}

// Draws pathActive while pressed (isPressed, driven by main.cpp's touch
// hit-testing - see ColorScreenRenderer.h's renderScreen() comment), else
// pathNormal. Both bitmaps are already fully rendered by the designer's
// export (drop shadow / 3D-press offset, border, label text baked in) -
// this only needs to decode and blit whichever one applies, not
// reconstruct any of that.
bool ColorScreenRenderer::renderSoftwareButton(const ScreenObject& obj, bool isPressed) {
  const String& path = isPressed ? obj.pathActive : obj.pathNormal;
  if (path.isEmpty()) return false;
  if (!ColorAssetLoader::drawBMPToCanvas(canvas_, path, obj.x, obj.y)) {
    canvas_->fillRect(obj.x + 2, obj.y + 2, obj.width - 4, obj.height - 4, 0x0000);
    canvas_->drawRect(obj.x, obj.y, obj.width, obj.height, 0x0000);
    return false;
  }
  return true;
}

bool ColorScreenRenderer::renderObject(const ScreenObject& obj, const String& pressedButtonId) {
  if (obj.type == "box") return renderBox(obj);
  if (obj.type == "line") return renderLine(obj);
  if (obj.type == "label") return renderLabel(obj);
  if (obj.type == "MqttDataField" || obj.type == "field") return renderMQTTDataField(obj);
  if (obj.type == "level-indicator") return renderLevelIndicator(obj);
  if (obj.type == "MQTTIconField") return renderMQTTIconField(obj);
  if (obj.type == "icon") return renderIcon(obj);
  if (obj.type == "SoftwareButton") return renderSoftwareButton(obj, !pressedButtonId.isEmpty() && obj.id == pressedButtonId);

  Serial.printf("[ColorScreenRenderer] Object type \"%s\" not implemented yet, skipping (id=%s)\n",
                obj.type.c_str(), obj.id.c_str());
  return false;
}

bool ColorScreenRenderer::renderScreen(int screenIndex, const String& pressedButtonId) {
  if (!projectLoader_.isLoaded()) return false;

  const ProjectConfig& project = projectLoader_.getProject();
  if (screenIndex < 0 || screenIndex >= (int)project.screens.size()) return false;

  const Screen& screen = project.screens[screenIndex];

  bool bgTransparent = false;
  uint16_t screenBg = parseHexColor(screen.backgroundColor.isEmpty() ? "#ffffff" : screen.backgroundColor, &bgTransparent);
  canvas_->fillScreen(bgTransparent ? 0xFFFF : screenBg);

  std::vector<ScreenObject> sortedObjects = screen.objects;
  std::sort(sortedObjects.begin(), sortedObjects.end(),
            [](const ScreenObject& a, const ScreenObject& b) { return a.zIndex < b.zIndex; });

  for (const auto& obj : sortedObjects) {
    renderObject(obj, pressedButtonId);
  }

  return true;
}
