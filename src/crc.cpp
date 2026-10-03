#include "crc.h"
#include "voltra_protocol.h"
#include <Arduino.h>

namespace voltra {

static uint8_t reflect8(uint8_t v) {
  v = (v & 0xF0) >> 4 | (v & 0x0F) << 4;
  v = (v & 0xCC) >> 2 | (v & 0x33) << 2;
  v = (v & 0xAA) >> 1 | (v & 0x55) << 1;
  return v;
}

static uint16_t reflect16(uint16_t v) {
  v = (v & 0xFF00) >> 8 | (v & 0x00FF) << 8;
  v = (v & 0xF0F0) >> 4 | (v & 0x0F0F) << 4;
  v = (v & 0xCCCC) >> 2 | (v & 0x3333) << 2;
  v = (v & 0xAAAA) >> 1 | (v & 0x5555) << 1;
  return v;
}

uint8_t crc8(const uint8_t *data, size_t len) {
  uint8_t crc = 0xEE; // init
  for (size_t i = 0; i < len; i++) {
    uint8_t b = reflect8(data[i]); // refin
    crc ^= b;
    for (int bit = 0; bit < 8; bit++) {
      if (crc & 0x80) {
        crc = (uint8_t)((crc << 1) ^ 0x31);
      } else {
        crc = (uint8_t)(crc << 1);
      }
    }
  }
  return reflect8(crc); // refout, xorout = 0
}

uint16_t crc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0x496C; // init
  for (size_t i = 0; i < len; i++) {
    uint8_t b = reflect8(data[i]); // refin
    crc ^= ((uint16_t)b) << 8;
    for (int bit = 0; bit < 8; bit++) {
      if (crc & 0x8000) {
        crc = (uint16_t)((crc << 1) ^ 0x1021);
      } else {
        crc = (uint16_t)(crc << 1);
      }
    }
  }
  return reflect16(crc); // refout, xorout = 0
}

bool selfTestAgainstCapturedFrames() {
  // Every one of these (plus 196 weight-table entries checked offline, see
  // tools/verify_crc.py) validates cleanly against this CRC implementation --
  // unlike v1, there's no exempted frame here.
  const LiteralFrame frames[] = {
      {"AUTH_KNOB", kAuthKnob, sizeof(kAuthKnob)},
      {"AUTH_IPHONE (fallback)", kAuthIphoneFallback, sizeof(kAuthIphoneFallback)},
      {"INIT0", kInit0, sizeof(kInit0)},
      {"INIT1", kInit1, sizeof(kInit1)},
      {"MODE_WEIGHT_TRAINING", kModeWeightTraining, sizeof(kModeWeightTraining)},
      {"MODE_IDLE", kModeIdle, sizeof(kModeIdle)},
      {"TELEMETRY_SUBSCRIBE_ALL", kTelemetrySubscribeAll, sizeof(kTelemetrySubscribeAll)},
      {"READ_SETTINGS", kReadSettings, sizeof(kReadSettings)},
  };
  bool allOk = true;
  for (const auto &p : frames) {
    uint8_t expectedCrc8 = p.bytes[3];
    uint8_t gotCrc8 = crc8(p.bytes, 3);

    uint16_t expectedCrc16 = (uint16_t)p.bytes[p.len - 2] | ((uint16_t)p.bytes[p.len - 1] << 8);
    uint16_t gotCrc16 = crc16(p.bytes, p.len - 2);

    bool crc8Ok = (gotCrc8 == expectedCrc8);
    bool crc16Ok = (gotCrc16 == expectedCrc16);
    if (!(crc8Ok && crc16Ok)) allOk = false;

    Serial.printf("[crc-selftest] %-24s crc8 %s (exp %02X got %02X)  crc16 %s (exp %04X got %04X)\n",
                  p.label,
                  crc8Ok ? "OK" : "FAIL", expectedCrc8, gotCrc8,
                  crc16Ok ? "OK" : "FAIL", expectedCrc16, gotCrc16);
  }
  return allOk;
}

} // namespace voltra
