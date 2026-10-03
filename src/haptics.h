#pragma once

// Minimal hand-rolled driver for the DRV2605 haptic motor driver chip on
// this board's I2C bus (the same bus as the CST816 touch controller -- see
// touch_cst816.cpp -- SDA=11/SCL=12; the DRV2605's fixed address 0x5A
// doesn't collide with the touch controller's own 0x15). Hand-rolled
// against the DRV2605's public register map rather than pulling in
// Waveshare's own example's third-party SensorLib class, matching this
// project's existing style of not adding external library dependencies for
// a narrow, well-documented protocol.
//
// Only the one feature this project actually needs is implemented: a
// single short "tap confirmation" buzz (library-1 effect #4, "Sharp Click -
// 100%", from the enumerated effect list in Waveshare's own
// 03_DRV2605_Test.ino) fired once per meaningful touchscreen tap. See
// ui.cpp's update() for where pulse() is called and how it's gated on
// prefs::hapticsEnabled().
namespace haptics {

// Call once from setup(), AFTER touch::begin() -- touch::begin() is what
// actually calls Wire.begin() on this shared I2C bus (see
// touch_cst816.cpp); this must not re-initialize it. Safe to call even if
// no DRV2605 is populated on a given board: pulse() then just does nothing.
void begin();

// Fires one short confirmation buzz. No-op if no DRV2605 was found at
// begin(). Does NOT itself check prefs::hapticsEnabled() -- callers decide
// whether a buzz is wanted right now; this is purely the mechanism.
void pulse();

// True if a DRV2605 responded on the I2C bus at begin().
bool present();

} // namespace haptics
