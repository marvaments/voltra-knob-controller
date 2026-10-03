#pragma once

namespace board_battery {

// Call once from setup(), after Arduino's ADC subsystem is otherwise ready
// (no ordering requirement in practice -- just needs to run before the
// first update()).
void begin();

// Call periodically (every loop() iteration is fine -- internally
// rate-limited to actually touch the ADC only every few seconds, since
// battery voltage moves slowly and this would otherwise burn cycles
// sampling far more often than the reading could ever change).
void update();

// Last computed percent for the KNOB BOARD's own battery, 0-100. This is
// NOT the Voltra's battery (that comes over BLE as
// voltra::VoltraState::batteryPct) -- this is the little LiPo plugged into
// this board's own PH1.25 connector, read locally via ADC. Returns -1
// before the first sample completes, or if the sampled voltage falls
// outside any plausible LiPo range (most likely meaning no cell is
// plugged in and the pin is floating/reading the charger rail instead).
int percent();

} // namespace board_battery
