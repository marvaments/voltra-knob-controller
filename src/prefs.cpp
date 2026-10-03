#include "prefs.h"
#include <Arduino.h>
#include <Preferences.h>
#include <nvs_flash.h>

namespace prefs {

static constexpr const char *kNamespace = "voltra_prefs";
static constexpr const char *kKeyUnits = "units";
static constexpr const char *kKeyBrightnessIdx = "bright_idx";
static constexpr const char *kKeyAutoDimIdx = "dim_idx";
static constexpr const char *kKeyStepIdx = "step_idx";
static constexpr const char *kKeyHaptics = "haptics";
static constexpr const char *kKeyPresets = "presets_on";

static Units sUnits = Units::Lb;

static const uint8_t kBrightnessPresets[] = {40, 60, 80, 100};
static constexpr uint8_t kBrightnessCount = sizeof(kBrightnessPresets) / sizeof(kBrightnessPresets[0]);
static uint8_t sBrightnessIdx = kBrightnessCount - 1; // default 100% -- matches pre-existing always-full behavior

// 0 = "never" -- kept last in the list so cycling through feels like it's
// building up to "off" rather than "off" being an arbitrary middle option.
static const uint16_t kAutoDimPresets[] = {10, 15, 30, 60, 0};
static constexpr uint8_t kAutoDimCount = sizeof(kAutoDimPresets) / sizeof(kAutoDimPresets[0]);
static uint8_t sAutoDimIdx = 1; // default 15s -- matches main.cpp's old hardcoded kIdleDimMs exactly

static const uint8_t kStepPresets[] = {1, 2, 5, 10};
static constexpr uint8_t kStepCount = sizeof(kStepPresets) / sizeof(kStepPresets[0]);
static uint8_t sStepIdx = 0; // default 1 lb/detent -- matches existing encoder behavior exactly

static bool sHapticsEnabled = true;
static bool sPresetsEnabled = true;

void begin() {
  Preferences p;
  p.begin(kNamespace, /*readOnly=*/true);
  sUnits = (Units)p.getUChar(kKeyUnits, (uint8_t)Units::Lb);
  sBrightnessIdx = p.getUChar(kKeyBrightnessIdx, kBrightnessCount - 1);
  sAutoDimIdx = p.getUChar(kKeyAutoDimIdx, 1);
  sStepIdx = p.getUChar(kKeyStepIdx, 0);
  sHapticsEnabled = p.getBool(kKeyHaptics, true);
  sPresetsEnabled = p.getBool(kKeyPresets, true);
  p.end();
  // Guard against a stale index from a previous build with fewer presets,
  // same defensive pattern ui.cpp's loadTheme() already uses for sThemeIndex.
  if ((uint8_t)sUnits > 1) sUnits = Units::Lb;
  if (sBrightnessIdx >= kBrightnessCount) sBrightnessIdx = kBrightnessCount - 1;
  if (sAutoDimIdx >= kAutoDimCount) sAutoDimIdx = 1;
  if (sStepIdx >= kStepCount) sStepIdx = 0;
}

Units units() { return sUnits; }
void cycleUnits() {
  sUnits = (sUnits == Units::Lb) ? Units::Kg : Units::Lb;
  Preferences p;
  p.begin(kNamespace, /*readOnly=*/false);
  p.putUChar(kKeyUnits, (uint8_t)sUnits);
  p.end();
  Serial.printf("[prefs] units -> %s\n", unitsLabel(sUnits));
}
const char *unitsLabel(Units u) { return u == Units::Kg ? "KG" : "LB"; }

static void saveBrightnessIdx() {
  Preferences p;
  p.begin(kNamespace, /*readOnly=*/false);
  p.putUChar(kKeyBrightnessIdx, sBrightnessIdx);
  p.end();
  Serial.printf("[prefs] brightness -> %u%%\n", brightnessPercent());
}
uint8_t brightnessPercent() { return kBrightnessPresets[sBrightnessIdx]; }
void cycleBrightness() {
  sBrightnessIdx = (sBrightnessIdx + 1) % kBrightnessCount;
  saveBrightnessIdx();
}
void cycleBrightnessPrev() {
  sBrightnessIdx = (sBrightnessIdx + kBrightnessCount - 1) % kBrightnessCount;
  saveBrightnessIdx();
}

static void saveAutoDimIdx() {
  Preferences p;
  p.begin(kNamespace, /*readOnly=*/false);
  p.putUChar(kKeyAutoDimIdx, sAutoDimIdx);
  p.end();
  Serial.printf("[prefs] auto-dim -> %us\n", (unsigned)autoDimSeconds());
}
uint16_t autoDimSeconds() { return kAutoDimPresets[sAutoDimIdx]; }
void cycleAutoDim() {
  sAutoDimIdx = (sAutoDimIdx + 1) % kAutoDimCount;
  saveAutoDimIdx();
}
void cycleAutoDimPrev() {
  sAutoDimIdx = (sAutoDimIdx + kAutoDimCount - 1) % kAutoDimCount;
  saveAutoDimIdx();
}

static void saveStepIdx() {
  Preferences p;
  p.begin(kNamespace, /*readOnly=*/false);
  p.putUChar(kKeyStepIdx, sStepIdx);
  p.end();
  Serial.printf("[prefs] step size -> %u lb/click\n", stepSizeLb());
}
uint8_t stepSizeLb() { return kStepPresets[sStepIdx]; }
void cycleStepSize() {
  sStepIdx = (sStepIdx + 1) % kStepCount;
  saveStepIdx();
}
void cycleStepSizePrev() {
  sStepIdx = (sStepIdx + kStepCount - 1) % kStepCount;
  saveStepIdx();
}

bool hapticsEnabled() { return sHapticsEnabled; }
void toggleHaptics() {
  sHapticsEnabled = !sHapticsEnabled;
  Preferences p;
  p.begin(kNamespace, /*readOnly=*/false);
  p.putBool(kKeyHaptics, sHapticsEnabled);
  p.end();
  Serial.printf("[prefs] haptics -> %s\n", sHapticsEnabled ? "on" : "off");
}

bool presetsEnabled() { return sPresetsEnabled; }
void togglePresets() {
  sPresetsEnabled = !sPresetsEnabled;
  Preferences p;
  p.begin(kNamespace, /*readOnly=*/false);
  p.putBool(kKeyPresets, sPresetsEnabled);
  p.end();
  Serial.printf("[prefs] weight presets -> %s\n", sPresetsEnabled ? "on" : "off");
}

void factoryReset() {
  Serial.println("[prefs] FACTORY RESET -- erasing all NVS and rebooting");
  Serial.flush();
  delay(50); // give the serial line a moment to actually drain before the flash erase + reboot
  nvs_flash_erase();
  nvs_flash_init();
  ESP.restart();
  while (true) {
    delay(1000); // ESP.restart() doesn't return in practice, but keep the compiler happy about the missing return
  }
}

} // namespace prefs
