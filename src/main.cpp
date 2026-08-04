// Checkpoint 1: M5GFX/M5Dial bring-up - confirms board selection, display
// driver, encoder and touch all work on real hardware before any of
// ScreenBee's own rendering/MQTT/deploy logic gets ported in. Shows the
// encoder count and push-button press count live, and draws a dot wherever
// the touchscreen is currently pressed - a single glance at the real
// device confirms all three input paths (encoder, button, touch) and the
// color display are wired correctly.
#include <M5Dial.h>

int32_t lastEncoderValue = 0;
int pressCount = 0;

void drawStatus() {
  M5Dial.Display.fillRect(0, 0, M5Dial.Display.width(), 90, TFT_BLACK);
  M5Dial.Display.setTextDatum(top_center);
  M5Dial.Display.setTextColor(TFT_ORANGE, TFT_BLACK);
  M5Dial.Display.setTextFont(&fonts::FreeSansBold12pt7b);
  M5Dial.Display.drawString("ScreenBee M5 Dial", M5Dial.Display.width() / 2, 20);

  M5Dial.Display.setTextFont(&fonts::FreeMono9pt7b);
  M5Dial.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  char line[48];
  snprintf(line, sizeof(line), "Encoder: %ld", static_cast<long>(lastEncoderValue));
  M5Dial.Display.drawString(line, M5Dial.Display.width() / 2, 55);
  snprintf(line, sizeof(line), "Presses: %d", pressCount);
  M5Dial.Display.drawString(line, M5Dial.Display.width() / 2, 75);
}

void setup() {
  auto cfg = M5.config();
  M5Dial.begin(cfg, /*enableEncoder=*/true, /*enableRFID=*/false);

  M5Dial.Display.setBrightness(150);
  M5Dial.Display.fillScreen(TFT_BLACK);
  drawStatus();

  Serial.begin(115200);
  Serial.println("[M5Dial] Checkpoint 1 - display/encoder/touch bring-up");
}

void loop() {
  M5Dial.update();

  const int32_t encoderValue = M5Dial.Encoder.read();
  bool needsRedraw = false;

  if (encoderValue != lastEncoderValue) {
    Serial.printf("[M5Dial] Encoder: %ld -> %ld\n", static_cast<long>(lastEncoderValue), static_cast<long>(encoderValue));
    lastEncoderValue = encoderValue;
    needsRedraw = true;
  }

  if (M5Dial.BtnA.wasPressed()) {
    pressCount++;
    Serial.printf("[M5Dial] Push button pressed (count=%d)\n", pressCount);
    needsRedraw = true;
  }

  if (needsRedraw) {
    drawStatus();
  }

  auto touchDetail = M5Dial.Touch.getDetail();
  if (touchDetail.isPressed()) {
    M5Dial.Display.fillCircle(touchDetail.x, touchDetail.y, 4, TFT_SKYBLUE);
  }

  delay(10);
}
