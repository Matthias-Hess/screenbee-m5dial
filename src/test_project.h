#pragma once

// Checkpoint 2 embedded test project - exercises the real
// LittleFS -> ProjectLoader -> ColorScreenRenderer pipeline with a real
// (if minimal) project.json, not a hand-called renderer function. Written
// to LittleFS at boot (always overwritten, not "if missing" - see
// main.cpp's writeTestProject() for why) rather than parsed directly from
// this string, so the actual file-reading code path (the one a real
// deployed project will also go through) gets exercised too. Every object
// type in the M5 Dial DDF's supportedObjectTypes is exercised now: box,
// line, label, MqttDataField, level-indicator, MQTTIconField, icon,
// SoftwareButton. Font metrics match public/ddf/m5stack-m5dial.ddf.zip's
// font-helvR12/font-helvR18 entries exactly. No MQTT connection exists
// yet (checkpoint 3) - MqttDataField/level-indicator/MQTTIconField read
// topics[].examples[0] as their value.
//
// icon/MQTTIconField/SoftwareButton all point at the same generated test
// asset (/assets/test-icon.bmp, see main.cpp's writeTestAssets() and
// test_icon_bmp.h) on purpose - the point of this test is proving
// ColorAssetLoader's BMP decode works via all three call sites, not
// producing visually distinct final artwork.
static const char TEST_PROJECT_JSON[] = R"json({
  "name": "Checkpoint 2 Test",
  "screenWidth": 240,
  "screenHeight": 240,
  "exportColorDepth": "24bit",
  "fonts": [
    { "id": "font-helvR12", "internalName": "u8g2_font_helvR12_tf", "size": 18, "ascent": 14, "descent": 4 },
    { "id": "font-helvR18", "internalName": "u8g2_font_helvR18_tf", "size": 27, "ascent": 22, "descent": 5 }
  ],
  "topics": [
    { "id": "topic-level", "topic": "test/level", "type": "numeric", "examples": ["67"] }
  ],
  "screens": [
    {
      "id": "screen-1",
      "name": "Test Screen",
      "backgroundColor": "#ffffff",
      "objects": [
        {
          "type": "box", "id": "obj-box", "x": 20, "y": 8, "width": 200, "height": 38, "zIndex": 1,
          "properties": { "backgroundColor": "#ff6600", "strokeColor": "", "strokeWidth": 0, "cornerRadius": 8 }
        },
        {
          "type": "label", "id": "obj-label-title", "x": 30, "y": 12, "width": 180, "height": 27, "zIndex": 2,
          "properties": { "text": "M5 Dial", "fontId": "font-helvR18", "color": "#ffffff", "textColor": "#ffffff", "backgroundColor": "transparent", "textAlign": "center" }
        },
        {
          "type": "line", "id": "obj-line", "x": 20, "y": 50, "width": 200, "height": 0, "zIndex": 1,
          "properties": { "color": "#000000", "strokeWidth": 1 }
        },
        {
          "type": "label", "id": "obj-label-body", "x": 20, "y": 56, "width": 200, "height": 18, "zIndex": 1,
          "properties": { "text": "ColorScreenRenderer test", "fontId": "font-helvR12", "color": "#000000", "textColor": "#000000", "backgroundColor": "transparent", "textAlign": "left" }
        },
        {
          "type": "box", "id": "obj-box-outline", "x": 20, "y": 78, "width": 200, "height": 30, "zIndex": 1,
          "properties": { "backgroundColor": "transparent", "strokeColor": "#ff6600", "strokeWidth": 2, "cornerRadius": 5 }
        },
        {
          "type": "label", "id": "obj-label-outline", "x": 28, "y": 84, "width": 180, "height": 18, "zIndex": 2,
          "properties": { "text": "Box + border + line", "fontId": "font-helvR12", "color": "#ff6600", "textColor": "#ff6600", "backgroundColor": "transparent", "textAlign": "left" }
        },
        {
          "type": "MqttDataField", "id": "obj-mqtt-field", "x": 20, "y": 112, "width": 200, "height": 18, "zIndex": 1,
          "properties": {
            "topic": "test/level", "prefix": "Level: ", "postfix": " %", "displayAs": "Formatted Number",
            "numberOfDecimals": 0, "fontId": "font-helvR12", "color": "#000000", "textColor": "#000000",
            "backgroundColor": "transparent", "textAlign": "left"
          }
        },
        {
          "type": "level-indicator", "id": "obj-level", "x": 20, "y": 134, "width": 200, "height": 30, "zIndex": 1,
          "properties": {
            "topic": "test/level", "fontId": "font-helvR12", "backgroundColor": "#ffffff",
            "borderColor": "#333333", "fillColor": "#4CAF50", "barDirection": "left-to-right",
            "displayValue": "percentage",
            "calibrationPoints": [ { "value": 0, "barSizePercent": 0 }, { "value": 100, "barSizePercent": 100 } ]
          }
        },
        {
          "type": "icon", "id": "obj-icon", "x": 20, "y": 174, "width": 32, "height": 32, "zIndex": 1,
          "path": "/assets/test-icon.bmp",
          "properties": {}
        },
        {
          "type": "MQTTIconField", "id": "obj-mqtt-icon", "x": 60, "y": 174, "width": 32, "height": 32, "zIndex": 1,
          "properties": {
            "topic": "test/level",
            "valueIconPairs": [
              { "id": "pair-1", "comparisonOperator": ">=", "value": 50, "path": "/assets/test-icon.bmp" }
            ]
          }
        },
        {
          "type": "SoftwareButton", "id": "obj-sw-button", "x": 100, "y": 174, "width": 32, "height": 32, "zIndex": 1,
          "pathNormal": "/assets/test-icon.bmp", "pathActive": "/assets/test-icon.bmp",
          "properties": {}
        }
      ]
    }
  ],
  "hardwareButtons": []
})json";
