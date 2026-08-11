#pragma once
#include <Arduino.h>
#include <vector>

/**
 * Data structures for project.json configuration
 * Represents screens, objects, and properties
 */

// Value-Icon pair for conditional icon display
struct ValueIconPair {
  String id;
  String comparisonOperator;  // ">", "<", "==", ">=", "<=", "!="
  float value;
  String thenShowIcon;        // Icon ID to show
  String path;                // Path to BMP file
};

// Calibration point for level indicators
struct CalibrationPoint {
  float value;                // Input value from MQTT
  float barSizePercent;       // Bar fill percentage (0-100)
};

// One vertex of a (possibly multi-segment) line - mirrors the designer's
// LinePoint (render-line.ts). Absolute screen coordinates, same as every
// other object's own x/y.
struct Point {
  int x;
  int y;
};

// A single button action - mirrors the designer's HardwareButtonAction
// (components/project-editor.tsx) field-for-field. `type` is one of
// "next-screen" / "previous-screen" / "goto-screen" / "send-mqtt" /
// "goto-setup-mode", or empty to mean "nothing configured" (the default-
// constructed value - callers treat an empty type as a no-op). Shared
// between the physical push button (HardwareButtonDef::defaultAction,
// Screen::buttonActions) and a SoftwareButton object's own
// ObjectProperties::action below - both dispatch through main.cpp's
// dispatchButtonAction(), one shared function rather than two parallel
// ones. "goto-setup-mode" takes no extra fields (like next-screen/
// previous-screen) - it's the only supported way into setup mode from a
// project-configured button; the rear GPIO0 button is a separate,
// hardcoded, project-independent fallback that bypasses this entirely
// (deliberately not exposed here or in the DDF - see main.cpp's own
// comment on it).
struct ButtonAction {
  String type;
  String targetScreenId;   // for "goto-screen"
  String mqttTopic;        // for "send-mqtt"
  String mqttMessage;      // for "send-mqtt"
};

// Base properties for screen objects
struct ObjectProperties {
  // Common properties
  String topic;                           // MQTT topic (e.g., "Freshwater/Level")
  String displayAs;                       // "Display as-is", "Formatted Number"
  String backgroundColor;
  String borderColor;
  String textColor;
  String color;                           // Alternate text color property (for labels)
  String textAlign;                       // "left", "center", "right"
  String fontId;                          // Font identifier
  String fontWeight;                      // "normal", "bold" (for labels)
  
  // Label properties
  String text;                            // Static text for labels
  int fontSize;                           // Font size in pixels (for labels)
  
  // MqttDataField properties
  String prefix;                          // Text before value
  String postfix;                         // Text after value
  String thousandsSeparator;
  int numberOfDecimals;                   // Number of decimal places for formatted numbers
  
  // MQTTIconField properties
  std::vector<ValueIconPair> valueIconPairs;
  
  // level-indicator properties
  String barDirection;                    // "left-to-right", "right-to-left", "bottom-to-top", "top-to-bottom"
  String displayValue;                    // "none", "percentage", "value"
  String fillColor;                       // Color of filled portion of bar
  std::vector<CalibrationPoint> calibrationPoints;

  // line properties
  int strokeWidth;                        // Line thickness in px (1-10); box border thickness (0-10, 0 = no border)
  String strokeStyle;                     // "solid", "dashed", "dotted" - only "solid" is rendered so far
  // A line's real vertices (segmented/multi-point line) - empty for every
  // line saved before this existed, or a plain two-point line that hasn't
  // been touched by the segmented-line tool; renderLine() then falls back
  // to the (x, y)-to-(x+width, y+height) derivation every line used before,
  // exactly like the designer's own getLinePoints() fallback.
  std::vector<Point> points;
  int filletRadius;                       // Rounds interior vertices (0 = sharp corners)
  bool arrowStart;                        // Filled-triangle arrowhead at points[0] (plain "line" only)
  bool arrowEnd;                          // Filled-triangle arrowhead at points[last] (plain "line" only)

  // MqttDataLine properties - a flow-visualization line: `calibrationPoints`
  // (reused verbatim from level-indicator, see ScreenRenderer::
  // renderMqttDataLine() for why the field stays named barSizePercent) maps
  // abs(topic value) to a stroke width in px; these two independent
  // operator+value conditions decide, each on its own, whether that same
  // topic's signed value shows an arrowhead at that end - unlike `line`'s
  // fixed arrowStart/arrowEnd booleans, or `panel`'s single shared
  // comparisonOperator/Value (one condition is enough there since a panel
  // only ever needs to be shown or not) - two ends need two independent
  // conditions evaluated simultaneously, so this can't reuse that single
  // pair.
  String arrowStartOperator;
  String arrowStartValue;
  String arrowEndOperator;
  String arrowEndValue;

  // box properties
  String strokeColor;                     // Box border color - empty means "no border", same as the
                                           // designer's `if (strokeColor && strokeWidth > 0)` check
  int cornerRadius;                       // Corner radius in px (0 = square corners)

  // panel properties - matched against the parent tab-control's `topic`
  // value (that object's own `topic` field, reused rather than duplicated).
  // Same shape as ValueIconPair's comparisonOperator (">", "<", "==", ">=",
  // "<=", "!=") but comparisonValue is a String, not a float - a mode enum
  // like "TEMP" isn't numeric. For the four numeric operators both sides
  // are parsed with toFloat() at evaluation time; for "==" / "!=" the
  // comparison is a trimmed String comparison instead.
  String comparisonOperator;
  String comparisonValue;

  // SoftwareButton properties - the button's own configured action (empty
  // type = configured with no action, still renders/presses visually but
  // dispatchButtonAction() is a no-op for it). Parsed from properties.action
  // by ProjectLoader::parseObjectProperties() via the same parseButtonAction()
  // hardware buttons already use - see ButtonAction's own comment above.
  ButtonAction action;

  ObjectProperties()
    : numberOfDecimals(2), fontSize(12), strokeWidth(1), cornerRadius(0), filletRadius(0),
      arrowStart(false), arrowEnd(false) {}
};

// Screen object (can be MQTTIconField, MqttDataField, tab-control, panel, etc.)
struct ScreenObject {
  String type;                // "MQTTIconField", "MqttDataField", "tab-control", "panel", etc.
  String id;
  int x;
  int y;
  int width;
  int height;
  int zIndex;
  ObjectProperties properties;
  // Bitmap paths the designer's export flattens onto the object itself
  // (not nested in `properties`) - see lib/project-zip.ts: an "icon"
  // object gets `path`, a "SoftwareButton" gets `pathNormal`/`pathActive`.
  // Not present in MqttEPaperDisplay2's ProjectTypes.h - that firmware
  // never implemented either object type (no touchscreen, e-paper icons
  // were never wired up either), so these are new here for M5 Dial.
  String path;
  String pathNormal;
  String pathActive;
  // Only meaningful for "tab-control" (children must all be "panel") and
  // "panel" (children are arbitrary regular objects) - every other type is
  // always a leaf (empty). Child x/y are relative to this object's own
  // (x, y) origin, not absolute screen coordinates - the renderer
  // accumulates an offset as it descends. A std::vector member of the
  // struct's own type is standard, well-defined C++ (the vector only needs
  // a complete element type at instantiation, not at struct-definition
  // time), no forward-declaration trick required.
  std::vector<ScreenObject> children;
};

// One screen's override for a single button id (e.g. "btn-0") - see
// Screen::buttonActions below. A vector of (key, value) pairs rather than
// std::map, matching this codebase's existing convention of linear-scanning
// small vectors (see ProjectLoader::getTopicValue) instead of pulling in
// map's overhead for lists that are realistically a handful of entries.
struct ScreenButtonAction {
  String buttonId;
  ButtonAction action;
};

// Project-wide default action for a physical button, from the device's
// hardware-buttons list (public/ddf/*.ddf.zip on the designer side). Only
// `id` (e.g. "btn-0") and `defaultAction` matter to firmware - the rest of
// the designer's HardwareButton (name, svgElementId, shape, x/y/width/
// height) is on-canvas rendering metadata for the button overlay only.
struct HardwareButtonDef {
  String id;
  ButtonAction defaultAction;
};

// Screen definition
struct Screen {
  String id;
  String name;
  String path;                            // Background BMP path (optional)
  String backgroundColor;                 // Screen fill color, e.g. "#ffffff" (optional, defaults to white)
  std::vector<ScreenObject> objects;
  // Per-screen override of a button's action, keyed by button id (e.g.
  // "btn-0") - takes priority over ProjectConfig::hardwareButtons's
  // defaultAction for the same id. Absent for older projects (additive
  // JSON field), in which case every button on this screen just falls back
  // to the project-wide default, same as if this were empty.
  std::vector<ScreenButtonAction> buttonActions;
  // Present only when the designer's device.json declared
  // needsPageIconsInSize AND this screen has an icon assigned - a plain
  // square 8-bit grayscale PGM mask (see
  // ColorAssetLoader::drawGrayscaleMaskToCanvas), for ScreenNavigatorOverlay's
  // screen-switch navigator. Empty for every screen otherwise (most
  // projects, and any project built before this field existed) - callers
  // must treat empty as "no icon", not an error.
  String pageIconPath;
};

// One field pulled out of a "json"-type topic's payload - mirrors the
// designer's JsonSubtopic (components/screenman-editor.tsx) field-for-
// field. `path` uses JSONPath's shorthand member/index syntax (RFC 9535) -
// a genuine *subset* of JSONPath, not an invented look-alike: "temp",
// "$.temp", "nested.temp", "readings[0].value", "['odd key.with.dots']"
// are all valid standard JSONPath that a real JSONPath engine would
// evaluate identically (see ProjectLoader::extractJsonField / its
// tokenizeJsonPath() helper for the parser, mirrored exactly in the
// designer's lib/json-path.ts). What's intentionally not implemented is
// the rest of the query language - wildcards, recursive descent, filter
// expressions, slices, unions - which a "pull one named field out of a
// flat sensor payload" use case doesn't need, and which has no
// well-maintained ESP32/Arduino implementation to build on anyway
// (researched 2026-07-25: ArduinoJson's own JSONPath support request,
// upstream issue #821, is still open/unimplemented; the one alternative
// with path-style access, FirebaseJson, would just be a second JSON
// parser pulled in for the same member/index capability ArduinoJson,
// already a dependency, already provides). An object binds to one via the
// composite string "<topic>#<path>" stored in its own properties.topic -
// see ProjectLoader::getTopicValue, which is where that string actually
// gets split and the field extracted.
struct JsonSubtopic {
  String id;
  String path;
  String type;                            // "numeric" or "text" - the VALUE type once extracted
};

// MQTT Topic definition
struct MQTTTopic {
  String id;
  String topic;
  String type;                            // "numeric", "string", "boolean", "json"
  std::vector<String> examples;
  String currentValue;                    // Current value (updated by MQTT later) - for "json", the whole raw payload
  std::vector<JsonSubtopic> subtopics;     // only meaningful when type == "json"
};

// Font definition with metrics
struct Font {
  String id;                              // Font ID (e.g., "font-4")
  String name;                            // Full font name
  String displayName;                     // Display name for UI
  String internalName;                    // U8g2 font name (e.g., "u8g2_font_helvR08_tf")
  int size;                               // Total line height in pixels
  int ascent;                             // Height above baseline
  int descent;                            // Height below baseline
};

// Main project configuration
struct ProjectConfig {
  String name;
  int screenWidth;
  int screenHeight;
  String exportColorDepth;
  std::vector<MQTTTopic> topics;
  std::vector<Font> fonts;
  std::vector<Screen> screens;
  std::vector<HardwareButtonDef> hardwareButtons;

  ProjectConfig() : screenWidth(0), screenHeight(0) {}
};





