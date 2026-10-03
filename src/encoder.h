#pragma once
#include <cstdint>

namespace encoder {

// Rotary encoder on the board's primary knob (GPIO8=A, GPIO7=B per
// Waveshare's own demo pin map). No push-button on this knob -- taps on the
// touchscreen are used for selection instead (see ui.cpp). Decoded with a
// timer-polled debounce algorithm ported from Waveshare's own demo
// firmware (see encoder.cpp) -- this isn't a generic quadrature encoder
// driver, it's specifically tuned for this board's actual hardware.
void begin(int pinA, int pinB);

// Returns signed detent delta since the last call (usually -1, 0, or +1;
// can be larger if you called this less often than the knob was turned).
// If turning the knob one way changes the on-screen value in the opposite
// direction from what you'd expect, flip the two signs in
// encoder.cpp's onTimer() rather than swapping the A/B wiring.
int32_t readDelta();

} // namespace encoder
