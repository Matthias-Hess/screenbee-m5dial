#pragma once
#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <U8g2_for_Adafruit_GFX.h>
#include "../interfaces/IProjectLoader.h"
#include "ProjectTypes.h"
#include "../ClippedCanvas16.h"
#include "ColorAssetLoader.h"
#include <functional>

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
// ported: box, line (now full parity with the e-paper reference - fillet,
// thick strokes, fixed arrowheads), label, MqttDataField, level-indicator,
// MQTTIconField, icon, SoftwareButton, tab-control, panel, MqttDataLine.
// icon and SoftwareButton have no e-paper equivalent to port from -
// MqttEPaperDisplay2 never implemented either (no touchscreen, and its own
// "icon" object type was never wired up despite existing in the schema) -
// so their rendering (draw the already-exported bitmap via ColorAssetLoader,
// always the "normal" state for SoftwareButton until real touch tracking
// exists) was designed fresh from the designer's own export shape
// (lib/project-zip.ts's path/pathNormal/pathActive fields), not ported from
// existing firmware logic.
//
// tab-control/panel/MqttDataLine (2026-08-15 port from MqttEPaperDisplay2's
// ScreenRenderer) reuse that firmware's logic near-verbatim, swapping only
// parseColor()'s black/white result for parseHexColor()'s real RGB565 one -
// the geometry (fillet math, thick-line rasterization, arrowhead placement,
// panel-condition evaluation) is color-depth-independent. NOTE: touch
// hit-testing (main.cpp's findSoftwareButtonAt/findSwitchSegmentAt) is
// still top-level-only and does NOT recurse into a tab-control's active
// panel - a SoftwareButton or Switch nested inside a panel renders
// correctly but isn't tappable yet. Out of scope here (this port covers
// rendering only, per the request that introduced it); see those
// functions' own comments.
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
  // pendingSwitchId/pendingSwitchStateIndex: while a Switch's tapped
  // segment is awaiting MQTT confirmation (main.cpp's 3s pending/timeout
  // state machine), that one segment draws the pending look instead of its
  // normal active/inactive one - same "pass the one runtime highlight down
  // through render" pattern as pressedButtonId, just keyed by (object id,
  // segment index) instead of just an id, since a Switch has more than one
  // sub-region that can independently need this.
  // pressedSwitchId/pressedSwitchStateIndex: while a Switch segment is
  // actively being touched (finger still down, before release) - draws a
  // blended background/text color (halfway between the segment's normal
  // and active colors) so a press is felt immediately, distinct from both
  // the plain idle look and the thicker-bordered pending look that
  // replaces it the moment the finger lifts (2026-08-14 addition, agreed
  // in this session's follow-up design discussion).
  bool renderScreen(int screenIndex, const String& pressedButtonId = "", const String& pendingSwitchId = "",
                     int pendingSwitchStateIndex = -1, const String& pressedSwitchId = "",
                     int pressedSwitchStateIndex = -1);

private:
  IProjectLoader& projectLoader_;
  ClippedCanvas16* canvas_;
  U8G2_FOR_ADAFRUIT_GFX u8g2_;

  bool renderObject(const ScreenObject& obj, const String& pressedButtonId, const String& pendingSwitchId,
                     int pendingSwitchStateIndex, const String& pressedSwitchId, int pressedSwitchStateIndex);
  bool renderBox(const ScreenObject& obj);
  bool renderLine(const ScreenObject& obj);
  bool renderLabel(const ScreenObject& obj);
  bool renderMQTTDataField(const ScreenObject& obj);
  bool renderLevelIndicator(const ScreenObject& obj);
  bool renderMQTTIconField(const ScreenObject& obj);
  bool renderIcon(const ScreenObject& obj);
  bool renderSoftwareButton(const ScreenObject& obj, bool isPressed);
  bool renderSwitch(const ScreenObject& obj, int pendingStateIndex, int pressedStateIndex);
  bool renderMqttDataLine(const ScreenObject& obj);

  // Recurses into the active panel's children (offset-copies with
  // obj.x/obj.y added, since child coordinates are relative to the
  // tab-control's own origin) via renderObject() - draws nothing itself if
  // no panel matches. Mirrors MqttEPaperDisplay2's ScreenRenderer::
  // renderTabControl() and components/canvas/renderers'
  // renderScreenObjects()'s "tab-control" case on the designer side. Takes
  // the same touch-state params renderObject() does, so a SoftwareButton/
  // Switch nested inside the active panel still renders its pressed/pending
  // look correctly even though it can't be hit-tested yet (see this file's
  // header comment).
  bool renderTabControl(const ScreenObject& obj, const String& pressedButtonId, const String& pendingSwitchId,
                         int pendingSwitchStateIndex, const String& pressedSwitchId, int pressedSwitchStateIndex);

  // Walks tabControl.children (each expected type "panel") in zIndex order,
  // returns a pointer to the first whose condition matches the
  // tab-control's own topic value, nullptr if none match. Pointer is valid
  // as long as tabControl's own children vector isn't mutated/destroyed -
  // callers only ever use it within the same renderObject() call. Mirrors
  // MqttEPaperDisplay2's ScreenRenderer::getActivePanel() exactly.
  const ScreenObject* getActivePanel(const ScreenObject& tabControl) const;

  bool evaluateCondition(float value, const String& op, float threshold) const;

  // Generalization of evaluateCondition() for panel matching: "==" / "!="
  // compare as trimmed Strings (an enum mode like "TEMP" isn't numeric, so
  // the float-only evaluateCondition() can't distinguish "TEMP" from
  // "POWER" - both toFloat() to 0.0), the four numeric operators delegate
  // to the existing evaluateCondition() via .toFloat() on both sides.
  // Mirrors MqttEPaperDisplay2's ScreenRenderer::evaluateVisibilityCondition()
  // and lib/render-screen.ts's evaluateCondition() on the designer side
  // exactly, the 0.0-not-NaN fallback included.
  bool evaluateVisibilityCondition(const String& actualValue, const String& op, const String& comparisonValue) const;

  String getIconPathForValue(const ScreenObject& obj, const String& valueStr) const;
  // Index into obj.properties.states whose readValue (trimmed) equals the
  // Switch's own topic's current value (also trimmed) - mirrors the
  // designer's getActiveSwitchStateIndex() (render-switch.ts) exactly:
  // plain trimmed String equality, not evaluateCondition()'s numeric
  // operators (a state's readValue is arbitrary text, e.g. "high", not
  // necessarily a number). -1 if no topic is set, no state matches, or no
  // retained value has arrived yet.
  int getActiveSwitchStateIndex(const ScreenObject& obj) const;

  void drawTextBox(const ScreenObject& obj, const String& displayText, bool drawBorder = true);

  // Line-geometry helpers, ported near-verbatim from MqttEPaperDisplay2's
  // ScreenRenderer (see that class's own comments for the full derivation
  // of each - fillet tangent/arc math, thick-line rasterization via a
  // rotated-rectangle inside-test, arrowhead placement/shortening). Shared
  // by renderLine() (fixed arrowStart/arrowEnd) and renderMqttDataLine()
  // (data-driven arrow conditions, data-driven stroke width) - identical to
  // how the e-paper reference shares them between its own two callers.

  // Fills a strokeWidth>1 line as a rotated rectangle with butt caps.
  void fillThickLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, float width, uint16_t color);

  // Rounds every interior vertex of a multi-point line via a true circular
  // arc between tangent points (not a Bezier approximation - see the e-paper
  // reference's 2026-07-30 finding for why a Bezier looked visibly wrong at
  // sharp angles). `drawSegment` is whatever renderLine()/renderMqttDataLine()
  // would otherwise call directly per straight segment.
  void renderFilletedLine(const std::vector<Point>& points, int filletRadius,
                           const std::function<void(int16_t, int16_t, int16_t, int16_t)>& drawSegment);

  // Straight segments when filletRadius<=0 or too few points, else
  // delegates to renderFilletedLine().
  void renderLineBody(const std::vector<Point>& points, int filletRadius,
                       const std::function<void(int16_t, int16_t, int16_t, int16_t)>& drawSegment);

  // A filled-triangle arrowhead pointing from `from` toward `tip`.
  void drawArrowhead(const Point& tip, const Point& from, float strokeWidth, uint16_t color);

  // The point a strokeWidth-wide line body should actually stop at when
  // drawing toward an end that shows an arrowhead - short of the true tip,
  // so the body's straight edges don't poke out past the arrowhead's
  // tapering sides.
  Point shortenForArrow(const Point& tip, const Point& from, float strokeWidth);

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
