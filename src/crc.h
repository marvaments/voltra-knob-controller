// CRC-8 and CRC-16 exactly as used by the VOLTRA BLE frame format.
//
// Parameters (poly/init as reported by the reverse-engineered HA
// integration; refin/refout/xorout independently brute-forced and
// verified against all 8 literal bootstrap packets -- see
// tools/verify_crc.py):
//   CRC-8:  poly=0x31, init=0xEE, refin=true, refout=true, xorout=0x00
//   CRC-16: poly=0x1021, init=0x496C, refin=true, refout=true, xorout=0x0000
#pragma once
#include <cstdint>
#include <cstddef>

namespace voltra {

uint8_t crc8(const uint8_t *data, size_t len);
uint16_t crc16(const uint8_t *data, size_t len);

// Runs crc8()/crc16() against the literal bootstrap packets baked into
// voltra_protocol.h and returns true only if every one matches. Call this
// once at boot (see main.cpp) so a silent protocol/library regression
// shows up immediately in the serial log instead of as a mysterious BLE
// failure later.
bool selfTestAgainstCapturedFrames();

} // namespace voltra
