#pragma once

// Checkpoint 2 embedded test project - exercises the real
// LittleFS -> ProjectLoader -> ColorScreenRenderer pipeline with a real
// (if minimal) project.json, not a hand-called renderer function. Written
// to LittleFS at boot if not already present (see main.cpp) rather than
// parsed directly from this string, so the actual file-reading code path
// (the one a real deployed project will also go through) gets exercised
// too. Object types match a subset of what's already ported
// (ColorScreenRenderer.h): box, line, label. Font metrics match
// public/ddf/m5stack-m5dial.ddf.zip's font-helvR12/font-helvR18 entries
// exactly, so this stays representative of a real DDF-derived project.
static const char TEST_PROJECT_JSON[] = R"json({
  "name": "Checkpoint 2 Test",
  "screenWidth": 240,
  "screenHeight": 240,
  "exportColorDepth": "24bit",
  "fonts": [
    { "id": "font-helvR12", "internalName": "u8g2_font_helvR12_tf", "size": 18, "ascent": 14, "descent": 4 },
    { "id": "font-helvR18", "internalName": "u8g2_font_helvR18_tf", "size": 27, "ascent": 22, "descent": 5 }
  ],
  "topics": [],
  "screens": [
    {
      "id": "screen-1",
      "name": "Test Screen",
      "backgroundColor": "#ffffff",
      "objects": [
        {
          "type": "box", "id": "obj-box", "x": 20, "y": 20, "width": 200, "height": 60, "zIndex": 1,
          "properties": { "backgroundColor": "#ff6600", "strokeColor": "", "strokeWidth": 0, "cornerRadius": 8 }
        },
        {
          "type": "label", "id": "obj-label-title", "x": 30, "y": 30, "width": 180, "height": 27, "zIndex": 2,
          "properties": { "text": "M5 Dial", "fontId": "font-helvR18", "color": "#ffffff", "textColor": "#ffffff", "backgroundColor": "transparent", "textAlign": "center" }
        },
        {
          "type": "line", "id": "obj-line", "x": 20, "y": 100, "width": 200, "height": 0, "zIndex": 1,
          "properties": { "color": "#000000", "strokeWidth": 1 }
        },
        {
          "type": "label", "id": "obj-label-body", "x": 20, "y": 120, "width": 200, "height": 18, "zIndex": 1,
          "properties": { "text": "ProjectLoader -> ColorScreenRenderer", "fontId": "font-helvR12", "color": "#000000", "textColor": "#000000", "backgroundColor": "transparent", "textAlign": "left" }
        },
        {
          "type": "box", "id": "obj-box-outline", "x": 20, "y": 150, "width": 200, "height": 50, "zIndex": 1,
          "properties": { "backgroundColor": "transparent", "strokeColor": "#ff6600", "strokeWidth": 3, "cornerRadius": 6 }
        },
        {
          "type": "label", "id": "obj-label-outline", "x": 30, "y": 158, "width": 180, "height": 18, "zIndex": 2,
          "properties": { "text": "Box + border + line", "fontId": "font-helvR12", "color": "#ff6600", "textColor": "#ff6600", "backgroundColor": "transparent", "textAlign": "left" }
        }
      ]
    }
  ],
  "hardwareButtons": []
})json";
