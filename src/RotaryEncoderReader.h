#pragma once
#include <Arduino.h>

// Direct interrupt-driven quadrature decode for the M5 Dial's rotary
// encoder (GPIO 41/40) - NOT M5Dial.Encoder (the M5Dial library's own
// ENCODER class, .pio/libdeps/m5dial/M5Dial/src/utility/Encoder.h), even
// though M5Dial.begin(..., enableEncoder=true) already sets that up.
//
// Found 2026-08-11: that library's attachInterrupt() call only actually
// attaches for GPIO pins it has a hardcoded CORE_INTxx_PIN macro for - a
// table inherited from AVR boards' small fixed set of hardware interrupt
// vectors. The highest one it defines is CORE_INT39_PIN; GPIO 40/41 aren't
// in the table at all, so its own attach_interrupt() silently returns 0
// for both pins (interrupts_in_use stays 0) and ENCODER::read() falls back
// to running the quadrature state machine only at the moment read() itself
// is called - i.e. real interrupts never fire, this becomes pure polling
// at whatever cadence loop() happens to call read(). A fast physical click
// can cycle through all 4 quadrature states within a couple of
// milliseconds; polling once per ~10ms loop() iteration (plus whatever
// else that iteration does) is too coarse to reliably see every
// transition, so some clicks were silently dropped or under/over-counted -
// reported live as "sometimes needs 2-3 physical clicks to advance one
// tablet" on the screen-switch navigator.
//
// ESP32's real attachInterrupt() has no such pin-table restriction (that's
// purely a vestige of the vendored library's own AVR-era macro list), so
// this reimplements the identical quadrature decode - same state/position
// lookup as ENCODER::update(), a well-tested Gray-code table, not a new
// algorithm - as two direct GPIO interrupts instead.
namespace RotaryEncoderReader {

void begin();
int32_t read();

}
