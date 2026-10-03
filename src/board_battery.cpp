#include "board_battery.h"
#include <Arduino.h>

namespace board_battery {

// This board (Waveshare ESP32-S3-Knob-Touch-LCD-1.8) has a real "MX1.25
// Lithium battery socket" and an onboard "battery charging management
// module" per Waveshare's own wiki -- it's not just USB powered. There's
// no fuel-gauge chip on it though (unlike some other Waveshare products
// that share the same demo repo and DO have one, e.g. BQ27220 -- that's a
// different board's example, not this one), so the only way to read
// battery level is a raw ADC voltage sense.
//
// Pin/circuit identified from Waveshare's own board-specific demo
// (ESP32-S3-Knob-Touch-LCD-1.8-Demo/Arduino/examples/01_ADC_Test/
// adc_bsp.cpp), which reads ADC_UNIT_1 / ADC_CHANNEL_0 and multiplies the
// raw ADC voltage by 2 -- the standard pattern for a resistor divider that
// halves the battery voltage before it reaches the ADC pin, so a full
// ~4.2V LiPo reads back as ~2.1V, safely inside the ADC's usable range.
// ADC1_CHANNEL_0 is GPIO1 on the ESP32-S3, confirmed against the installed
// ESP-IDF's own soc/adc_channel.h (ADC1_CHANNEL_0_GPIO_NUM == 1) rather
// than assumed. Waveshare's wiki doesn't publish the exact divider ratio
// or a schematic, so the "*2" is taken on faith from their own working
// demo code for this exact board -- if readings come out roughly half (or
// double) what a multimeter shows across the cell, that ratio is the
// first thing to re-check.
static constexpr int kPinBatteryAdc = 1;

// Battery voltage sense is inherently noisy sample-to-sample on this ADC;
// average a handful of back-to-back reads rather than trusting one.
static constexpr int kSampleCount = 16;

// Battery level changes slowly (minutes/hours), so there's no reason to
// hit the ADC on every loop() iteration -- re-sample only this often.
static constexpr uint32_t kSampleIntervalMs = 4000;

// Anything outside this range isn't a real single-cell LiPo reading --
// most likely no cell is plugged into the PH1.25 connector at all (in
// which case this pin floats or picks up whatever the charge-management
// module's output happens to be), so treat it as "no reading" rather than
// report a nonsense percentage.
static constexpr float kPlausibleMinV = 2.8f;
static constexpr float kPlausibleMaxV = 4.35f;

static int sPercent = -1;
static uint32_t sLastSampleMs = 0;
static bool sBegun = false;
static bool sSampledOnce = false;

// Single-cell LiPo open-circuit-voltage-to-percent curve, from the
// discharge-curve figures commonly published for standard 3.7V-nominal/
// 4.2V-full LiPo cells -- not this specific pack's own datasheet, since
// there's no chemistry ID or fuel gauge on this board to read one from.
// Treat this as a reasonable estimate, most accurate at rest and least
// accurate while the Voltra itself is under heavy load and sagging the
// cell voltage momentarily (this is the KNOB's own small battery, though,
// so it isn't under any real load from the workout itself either way).
struct VoltPoint {
  float volts;
  int percent;
};
static constexpr VoltPoint kCurve[] = {
    {3.30f, 0},  {3.45f, 2},  {3.55f, 5},  {3.62f, 10}, {3.68f, 15}, {3.71f, 20},
    {3.74f, 30}, {3.77f, 40}, {3.79f, 50}, {3.82f, 60}, {3.87f, 70}, {3.92f, 80},
    {3.98f, 90}, {4.06f, 95}, {4.20f, 100},
};
static constexpr size_t kCurveLen = sizeof(kCurve) / sizeof(kCurve[0]);

static int voltsToPercent(float v) {
  if (v <= kCurve[0].volts) return 0;
  if (v >= kCurve[kCurveLen - 1].volts) return 100;
  for (size_t i = 1; i < kCurveLen; i++) {
    if (v <= kCurve[i].volts) {
      const VoltPoint &lo = kCurve[i - 1];
      const VoltPoint &hi = kCurve[i];
      float t = (v - lo.volts) / (hi.volts - lo.volts);
      return lo.percent + (int)(t * (hi.percent - lo.percent) + 0.5f);
    }
  }
  return 100; // unreachable given the clamp above, kept for safety
}

void begin() {
  // ADC_11db (the Arduino core's name for what ESP-IDF now calls
  // ADC_ATTEN_DB_12) is what Waveshare's own demo uses for this same pin,
  // and gives the full ~0-3.3V input range this divided-down reading needs.
  analogSetPinAttenuation(kPinBatteryAdc, ADC_11db);
  sBegun = true;
  sLastSampleMs = 0; // force a sample on the first update() rather than waiting a full interval
}

void update() {
  if (!sBegun) return;
  uint32_t now = millis();
  if (sSampledOnce && now - sLastSampleMs < kSampleIntervalMs) return;
  sLastSampleMs = now;
  sSampledOnce = true;

  uint32_t sumMv = 0;
  for (int i = 0; i < kSampleCount; i++) {
    sumMv += analogReadMilliVolts(kPinBatteryAdc);
    delayMicroseconds(200);
  }
  float batteryVolts = (sumMv / (float)kSampleCount) / 1000.0f * 2.0f; // undo the onboard divider

  if (batteryVolts < kPlausibleMinV || batteryVolts > kPlausibleMaxV) {
    sPercent = -1;
    return;
  }
  sPercent = voltsToPercent(batteryVolts);
}

int percent() { return sPercent; }

} // namespace board_battery
