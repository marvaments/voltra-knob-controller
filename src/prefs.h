#pragma once
#include <cstdint>

// Persisted, consumer-adjustable device settings: display units, screen
// brightness, auto-dim delay, encoder step size, and haptic feedback
// on/off. All live in one NVS namespace ("voltra_prefs") separate from
// voltra_ui's theme (ui.cpp) and voltra_wifi's saved network
// (wifi_provision.cpp) -- keeping each concern in its own namespace avoids
// any cross-module key-name collisions as more settings get added later.
//
// Every setting here is a small fixed preset list, cycled forward/backward
// from the Settings screens (the same idiom ui.cpp's theme picker already
// uses) rather than a free-form value typed on-device -- there's no
// practical way to type an arbitrary number with just a knob and a
// touchscreen, and a handful of sensible presets covers real usage better
// than a wide-open range would anyway.
namespace prefs {

// Call once from setup(), before anything else here is read.
void begin();

// -- Display units (cosmetic only -- see ui.cpp's kg-conversion comment;
// all real BLE protocol values and encoder step math stay in lb) --
enum class Units : uint8_t { Lb, Kg };
Units units();
void cycleUnits();
const char *unitsLabel(Units u); // "LB" / "KG"

// -- Screen brightness while active (percent of full backlight) --
uint8_t brightnessPercent();
void cycleBrightness();     // next preset
void cycleBrightnessPrev(); // previous preset

// -- Auto-dim delay: seconds of no activity before the screen dims.
// 0 means "never dim". Replaces main.cpp's old hardcoded kIdleDimMs. --
uint16_t autoDimSeconds();
void cycleAutoDim();     // next preset
void cycleAutoDimPrev(); // previous preset

// -- Encoder step size: lb added/removed per detent, multiplying
// encoder.cpp's raw +/-1-per-detent delta (see ui.cpp's update()). --
uint8_t stepSizeLb();
void cycleStepSize();     // next preset
void cycleStepSizePrev(); // previous preset

// -- Haptic tap-confirmation buzz on/off (see haptics.h) --
bool hapticsEnabled();
void toggleHaptics();

// -- Weight-preset pills (P1/P2) on the main workout screen, on/off. Some
// people want the quick-recall shortcut, some find two more boxes on an
// already-busy idle screen distracting -- default on, matching the
// behavior that existed before this toggle did. --
bool presetsEnabled();
void togglePresets();

// Wipes every persisted setting across every NVS namespace this firmware
// uses (theme, saved wifi credentials, and these prefs) via a full
// flash-level erase, then reboots. Deliberately flash-level rather than
// each module clearing its own keys one at a time -- one source of truth
// for "what does factory reset mean," instead of every future settings
// module having to remember to also wire itself into a reset routine.
// Never returns.
void factoryReset();

} // namespace prefs
