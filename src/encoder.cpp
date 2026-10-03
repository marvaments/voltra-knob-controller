#include "encoder.h"
#include <Arduino.h>
#include "esp_timer.h"

namespace encoder {

namespace {

// This is a byte-for-byte port of the decode algorithm in Waveshare's own
// official demo firmware for this exact board
// (ESP32-S3-Knob-Touch-LCD-1.8-Demo/Arduino/examples/04_Encoder_Test/
// bidi_switch_knob.c, their "iot_knob" driver). It is NOT a textbook
// quadrature decoder (which is what this file used to do, via an
// interrupt-driven A/B state-transition table) -- it polls both pins on a
// timer and tracks each channel *independently*: a debounced rising edge
// on the A pin adds +1 to the running count, a debounced rising edge on B
// subtracts 1. That from-scratch quadrature approach never registered
// turns reliably on this board's actual encoder hardware; this is the
// exact scheme Waveshare verified works on it, so it's ported as-is
// rather than re-derived, including its specific timing: a 3ms poll
// (TICKS_INTERVAL) with a 2-tick debounce (DEBOUNCE_TICKS) -- both kept
// as named constants below in case this needs tuning for a noisier unit.
constexpr uint32_t kPollIntervalUs = 3000;
constexpr uint8_t kDebounceTicks = 2;

int sPinA = -1, sPinB = -1;
esp_timer_handle_t sTimer = nullptr;

struct ChannelState {
  uint8_t prevLevel = 1;
  uint8_t debounceCnt = 0;
};
ChannelState sChA, sChB;
volatile int32_t sCount = 0;

void processChannel(uint8_t level, ChannelState &ch, int8_t sign) {
  if (level == 0) {
    if (level != ch.prevLevel) ch.debounceCnt = 0;
    else ch.debounceCnt++;
  } else {
    if (level != ch.prevLevel && (uint8_t)(++ch.debounceCnt) >= kDebounceTicks) {
      ch.debounceCnt = 0;
      sCount += sign;
    } else {
      ch.debounceCnt = 0;
    }
  }
  ch.prevLevel = level;
}

void onTimer(void * /*arg*/) {
  uint8_t a = digitalRead(sPinA);
  uint8_t b = digitalRead(sPinB);
  // A-channel edges count up, B-channel edges count down -- same sign
  // convention as Waveshare's own driver (their KNOB_RIGHT/KNOB_LEFT
  // events map to A/B respectively). If turning the knob one way
  // decreases the on-screen value when you expect it to increase, flip
  // these two signs rather than the pin wiring.
  processChannel(a, sChA, +1);
  processChannel(b, sChB, -1);
}

} // namespace

void begin(int pinA, int pinB) {
  sPinA = pinA;
  sPinB = pinB;
  pinMode(sPinA, INPUT_PULLUP);
  pinMode(sPinB, INPUT_PULLUP);
  sChA.prevLevel = digitalRead(sPinA);
  sChB.prevLevel = digitalRead(sPinB);

  esp_timer_create_args_t args = {};
  args.callback = &onTimer;
  args.name = "encoder_poll";
  esp_timer_create(&args, &sTimer);
  esp_timer_start_periodic(sTimer, kPollIntervalUs);
}

int32_t readDelta() {
  static int32_t sLast = 0;
  int32_t cur = sCount; // single aligned 32-bit read; good enough without a
                         // lock given the ~3ms timer task vs. ~20ms main
                         // loop cadence -- worst case a step is seen one
                         // poll late, never lost or double-counted.
  int32_t delta = cur - sLast;
  sLast = cur;
  return delta;
}

} // namespace encoder
