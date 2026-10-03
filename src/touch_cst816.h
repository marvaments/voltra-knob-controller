#pragma once
#include <cstdint>

namespace touch {

struct TouchPoint {
  bool touched = false;
  int16_t x = 0;
  int16_t y = 0;
};

// Minimal CST816 polling driver over I2C (addr 0x15). No gesture support,
// single-point touch only -- plenty for tap-to-select buttons on this UI.
void begin(int sda, int scl, int rstPin, int intPin);
TouchPoint read();

} // namespace touch
