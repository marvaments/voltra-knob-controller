#include "voltra_protocol.h"
#include "crc.h"
#include <vector>
#include <cstring>

namespace voltra {

static std::vector<uint8_t> buildFrame(uint8_t cmd, const uint8_t *payload, size_t payloadLen, uint16_t seq) {
  size_t headerLen = 11; // sync,len,type,crc8,sender,receiver,seq(2),protocol(2),cmd
  size_t totalLen = headerLen + payloadLen + 2; // +CRC16

  std::vector<uint8_t> f(totalLen);
  f[0] = kSyncByte;
  f[1] = (uint8_t)totalLen;
  f[2] = kFrameType;
  // f[3] filled below once bytes 0-2 are set
  f[4] = kSenderId;
  f[5] = kReceiverId;
  f[6] = (uint8_t)(seq & 0xFF);
  f[7] = (uint8_t)((seq >> 8) & 0xFF);
  f[8] = (uint8_t)(kProtocolField & 0xFF);
  f[9] = (uint8_t)((kProtocolField >> 8) & 0xFF);
  f[10] = cmd;
  if (payloadLen > 0) {
    memcpy(&f[11], payload, payloadLen);
  }

  f[3] = crc8(f.data(), 3);

  uint16_t c16 = crc16(f.data(), totalLen - 2);
  f[totalLen - 2] = (uint8_t)(c16 & 0xFF);
  f[totalLen - 1] = (uint8_t)((c16 >> 8) & 0xFF);

  return f;
}

std::vector<uint8_t> buildParamWriteFrameU16(uint16_t paramId, uint16_t value, uint16_t seq) {
  // Payload shape [0x01, 0x00] + paramId(LE) + value(LE), cmd 0x11. This is
  // independently confirmed correct: it reproduces the real project's 196
  // captured weight-write frames byte-for-byte (mod sequence number) --
  // see tools/verify_crc.py.
  uint8_t payload[6];
  payload[0] = 0x01;
  payload[1] = 0x00;
  payload[2] = (uint8_t)(paramId & 0xFF);
  payload[3] = (uint8_t)((paramId >> 8) & 0xFF);
  payload[4] = (uint8_t)(value & 0xFF);
  payload[5] = (uint8_t)((value >> 8) & 0xFF);
  return buildFrame(kCmdParamWrite, payload, sizeof(payload), seq);
}

std::vector<uint8_t> buildParamWriteFrameI16(uint16_t paramId, int16_t value, uint16_t seq) {
  return buildParamWriteFrameU16(paramId, (uint16_t)value, seq);
}

std::vector<uint8_t> buildParamWriteFrameU8(uint16_t paramId, uint8_t value, uint16_t seq) {
  // Same shape as buildParamWriteFrameU16 above but with a single value
  // byte (5-byte payload instead of 6) -- used for the one-byte
  // style-selector flags (PARAM_FITNESS_INVERSE_CHAIN, PARAM_MOUNTAIN_CURVE)
  // rather than a 16-bit weight. Mirrors kirby6365/voltra_remote's
  // build_param_write_u8, the same reference project voltra_protocol.h
  // credits for those two parameter IDs.
  uint8_t payload[5];
  payload[0] = 0x01;
  payload[1] = 0x00;
  payload[2] = (uint8_t)(paramId & 0xFF);
  payload[3] = (uint8_t)((paramId >> 8) & 0xFF);
  payload[4] = value;
  return buildFrame(kCmdParamWrite, payload, sizeof(payload), seq);
}

std::vector<uint8_t> buildCommandFrame(uint8_t cmd, const uint8_t *payload, size_t payloadLen, uint16_t seq) {
  return buildFrame(cmd, payload, payloadLen, seq);
}

} // namespace voltra
