// Checkpoint 2: ProjectLoader + ColorScreenRenderer render a real (if
// minimal) embedded test project exercising every object type in the M5
// Dial DDF's supportedObjectTypes: box, line, label, MqttDataField,
// level-indicator, MQTTIconField, icon, SoftwareButton. Confirms the full
// pipeline (LittleFS file -> ProjectLoader JSON parsing ->
// ColorScreenRenderer -> canvas -> blit) end to end on real hardware
// before wiring in MQTT/deploy.
#include <M5Dial.h>
#include <LittleFS.h>
#include "ClippedCanvas16.h"
#include "project/ProjectLoader.h"
#include "project/ColorScreenRenderer.h"
#include "test_project.h"
#include "test_icon_bmp.h"

ClippedCanvas16 canvas(240, 240);
ProjectLoader projectLoader;
ColorScreenRenderer* screenRenderer = nullptr;

// Always overwrites (not "if missing") - these are bring-up fixtures that
// change as object types get ported, not real persisted data; an
// "if missing" guard bit us once already (a fixed test_project.h change
// silently had no effect because the stale LittleFS copy from a previous
// flash was never replaced).
void writeTestProject() {
  File f = LittleFS.open("/test_project.json", "w");
  if (!f) {
    Serial.println("[M5Dial] Failed to open /test_project.json for writing");
    return;
  }
  f.print(TEST_PROJECT_JSON);
  f.close();
  Serial.println("[M5Dial] Wrote /test_project.json");
}

void writeTestAssets() {
  LittleFS.mkdir("/assets");
  File f = LittleFS.open("/assets/test-icon.bmp", "w");
  if (!f) {
    Serial.println("[M5Dial] Failed to open /assets/test-icon.bmp for writing");
    return;
  }
  f.write(TEST_ICON_BMP, sizeof(TEST_ICON_BMP));
  f.close();
  Serial.printf("[M5Dial] Wrote /assets/test-icon.bmp (%d bytes)\n", (int)sizeof(TEST_ICON_BMP));
}

void blitCanvasToDisplay() {
  M5Dial.Display.startWrite();
  M5Dial.Display.setSwapBytes(true);
  M5Dial.Display.pushImage(0, 0, 240, 240, canvas.getBuffer());
  M5Dial.Display.endWrite();
}

void setup() {
  auto cfg = M5.config();
  M5Dial.begin(cfg, /*enableEncoder=*/true, /*enableRFID=*/false);
  M5Dial.Display.setBrightness(150);

  Serial.begin(115200);
  Serial.println("[M5Dial] Checkpoint 2 - ProjectLoader + ColorScreenRenderer");

  if (!LittleFS.begin(true)) {
    Serial.println("[M5Dial] LittleFS mount failed");
    return;
  }
  writeTestProject();
  writeTestAssets();

  if (!projectLoader.loadProject("/test_project.json")) {
    Serial.println("[M5Dial] Failed to load test project");
    return;
  }
  Serial.printf("[M5Dial] Loaded project \"%s\" with %d screen(s)\n",
                projectLoader.getProject().name.c_str(), projectLoader.getProject().screens.size());

  screenRenderer = new ColorScreenRenderer(projectLoader, &canvas);
  if (!screenRenderer->renderScreen(0)) {
    Serial.println("[M5Dial] renderScreen(0) failed");
    return;
  }

  blitCanvasToDisplay();
  Serial.println("[M5Dial] Rendered screen 0 to display");
}

void loop() {
  M5Dial.update();
  delay(10);
}
