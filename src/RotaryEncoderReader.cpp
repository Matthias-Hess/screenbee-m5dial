#include "RotaryEncoderReader.h"
#include <driver/gpio.h>

namespace {

const gpio_num_t PIN_A = GPIO_NUM_41;
const gpio_num_t PIN_B = GPIO_NUM_40;

volatile uint8_t state = 0;
volatile int32_t position = 0;

// A real spinlock (ESP-IDF portMUX), not the AVR-style noInterrupts()/
// interrupts() pair this started with - ESP32-S3 is dual-core, and a GPIO
// interrupt can be serviced on either core independent of which core
// happens to be running read() at that moment. noInterrupts() only masks
// interrupts on the CURRENT core, so it was not actually a safe critical
// section against an ISR that might fire on the other core - portMUX is
// the correct cross-core primitive for exactly this ISR/task shared-state
// pattern (see portENTER_CRITICAL_ISR's own documentation).
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// Identical Gray-code state/position table to M5Dial's own
// ENCODER::update() (see RotaryEncoderReader.h's own comment for why this
// firmware doesn't just use that class directly) - ported, not
// reinvented, since it's already a well-tested quadrature decode.
// gpio_get_level() instead of digitalRead() - a direct, IRAM-safe ESP-IDF
// register read with no Arduino-layer wrapper overhead in between, same
// spirit as the original library's own DIRECT_PIN_READ (it just couldn't
// use that macro's cached port/bitmask approach here since GPIO 40/41
// aren't in its interrupt-pin table at all - see this file's header
// comment).
void IRAM_ATTR isr() {
  uint8_t p1 = gpio_get_level(PIN_A) ? 1 : 0;
  uint8_t p2 = gpio_get_level(PIN_B) ? 1 : 0;
  uint8_t s = state & 3;
  if (p1) s |= 4;
  if (p2) s |= 8;
  state = s >> 2;

  int8_t delta = 0;
  switch (s) {
    case 1: case 7: case 8: case 14:
      delta = 1;
      break;
    case 2: case 4: case 11: case 13:
      delta = -1;
      break;
    case 3: case 12:
      delta = 2;
      break;
    case 6: case 9:
      delta = -2;
      break;
    default:
      return;
  }

  portENTER_CRITICAL_ISR(&mux);
  position += delta;
  portEXIT_CRITICAL_ISR(&mux);
}

}  // namespace

namespace RotaryEncoderReader {

void begin() {
  pinMode(PIN_A, INPUT_PULLUP);
  pinMode(PIN_B, INPUT_PULLUP);
  // Let the pull-ups settle through whatever passive R-C filtering exists
  // on this board before reading the initial state - same allowance
  // ENCODER::begin() makes.
  delayMicroseconds(2000);
  uint8_t s = 0;
  if (gpio_get_level(PIN_A)) s |= 1;
  if (gpio_get_level(PIN_B)) s |= 2;
  state = s;

  attachInterrupt(digitalPinToInterrupt(PIN_A), isr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_B), isr, CHANGE);
}

int32_t read() {
  portENTER_CRITICAL(&mux);
  int32_t ret = position;
  portEXIT_CRITICAL(&mux);
  return ret;
}

}  // namespace RotaryEncoderReader
