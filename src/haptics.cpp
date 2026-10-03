#include "haptics.h"
#include <Arduino.h>
#include <Wire.h>

namespace haptics {

static constexpr uint8_t kAddr = 0x5A;
static constexpr uint8_t kRegMode = 0x01;
static constexpr uint8_t kRegLibrary = 0x03;
static constexpr uint8_t kRegWaveSeq1 = 0x04;
static constexpr uint8_t kRegWaveSeq2 = 0x05;
static constexpr uint8_t kRegGo = 0x0C;
static constexpr uint8_t kModeInternalTrigger = 0x00;
static constexpr uint8_t kLibraryErmA = 1;
// "Sharp Click - 100%" -- library 1, effect #4 in the full 123-effect list
// Waveshare's own 03_DRV2605_Test.ino enumerates. Picked over the louder/
// longer "Strong Click"/"Strong Buzz" effects for something short and crisp
// enough to fire on every tap without feeling heavy-handed.
static constexpr uint8_t kEffectSharpClick100 = 4;

static bool sPresent = false;

static bool writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

void begin() {
  // Wire is already initialized by touch::begin() on this same shared I2C
  // bus (see touch_cst816.cpp, and main.cpp's setup() ordering) -- do NOT
  // call Wire.begin() again here.
  Wire.beginTransmission(kAddr);
  sPresent = (Wire.endTransmission() == 0);
  if (!sPresent) {
    Serial.println("[haptics] DRV2605 not found on I2C bus -- haptic feedback disabled");
    return;
  }
  writeReg(kRegMode, kModeInternalTrigger);
  writeReg(kRegLibrary, kLibraryErmA);
  Serial.println("[haptics] DRV2605 ready");
}

void pulse() {
  if (!sPresent) return;
  writeReg(kRegWaveSeq1, kEffectSharpClick100);
  writeReg(kRegWaveSeq2, 0); // terminator slot -- ends the sequence after one effect
  writeReg(kRegGo, 1);       // starts playback; self-clears when done
}

bool present() { return sPresent; }

} // namespace haptics
