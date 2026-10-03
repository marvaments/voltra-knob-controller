#pragma once
#include <Arduino_GFX_Library.h>

namespace display {

// Waveshare ESP32-S3-Knob-Touch-LCD-1.8 panel pins (from Waveshare's own
// demo code / pin map for this board):
//   QSPI: CLK=13 CS=14 D0=15 D1=16 D2=17 D3=18   RST=21   Backlight=47
constexpr int kPinClk = 13;
constexpr int kPinCs = 14;
constexpr int kPinD0 = 15;
constexpr int kPinD1 = 16;
constexpr int kPinD2 = 17;
constexpr int kPinD3 = 18;
constexpr int kPinRst = 21;
constexpr int kPinBacklight = 47;

constexpr int kWidth = 360;
constexpr int kHeight = 360;

// Returns a ready-to-draw GFX canvas (backed by a PSRAM framebuffer so the
// UI can be redrawn each frame and flushed atomically -- no tearing/flicker
// from partial writes to the physical panel).
Arduino_Canvas *begin();

// Push the canvas contents to the physical display.
void flush(Arduino_Canvas *canvas);

// Sets backlight brightness as a percent (0 = off, 100 = full). Backed by
// PWM (LEDC), not a plain digital on/off, so it can be dimmed for
// idle/power-saving rather than only fully on or fully off. begin() starts
// the backlight at 100.
void setBacklight(uint8_t percent);

} // namespace display
