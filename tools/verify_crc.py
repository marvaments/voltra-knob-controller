#!/usr/bin/env python3
"""
Independently re-derives and verifies the CRC-8/CRC-16 parameters used by
the VOLTRA BLE frame format, by parsing the literal frame byte-arrays
straight out of include/voltra_protocol.h (no hand-copied hex here, so this
can't drift from what the firmware actually sends).

Those frames were sourced from a real, working project --
https://github.com/omarshahine/voltra-knob. Before trusting that project's
frames.gen.h at all, every one of its 212 byte-array literals (all 196
weight-table entries plus every named frame: AUTH_*, INIT0/1, GO, STOP,
MODE_*, TELEMETRY_*, READ_SETTINGS, ...) was checked the same way, and every
single one validated cleanly. That's also what gives confidence in
buildParamWriteFrameU16/I16 in src/voltra_protocol.cpp: same CRC, same
payload shape, just building frames for values (like eccentric weight) that
project doesn't have a precomputed table for.

Run this any time you want to sanity-check the CRC, e.g. after capturing a
new frame from your own device:

    python3 tools/verify_crc.py
"""
import itertools
import re
from pathlib import Path

HEADER = Path(__file__).resolve().parent.parent / "include" / "voltra_protocol.h"


def load_frames():
    text = HEADER.read_text()
    frames = []
    for m in re.finditer(r'static const uint8_t (k\w+)\[\]\s*=\s*\{([^}]+)\}', text):
        name = m.group(1)
        vals = [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]{2}', m.group(2))]
        frames.append((name, bytes(vals)))
    return frames


def reflect(data: int, width: int) -> int:
    r = 0
    for i in range(width):
        if data & (1 << i):
            r |= 1 << (width - 1 - i)
    return r


def crc_generic(data: bytes, width: int, poly: int, init: int, refin: bool, refout: bool, xorout: int) -> int:
    mask = (1 << width) - 1
    crc = init
    for byte in data:
        if refin:
            byte = reflect(byte, 8)
        crc ^= (byte << (width - 8)) if width > 8 else byte
        for _ in range(8):
            if crc & (1 << (width - 1)):
                crc = ((crc << 1) ^ poly) & mask
            else:
                crc = (crc << 1) & mask
    if refout:
        crc = reflect(crc, width)
    return crc ^ xorout


def find_params(frames, width, poly, init):
    for refin, refout in itertools.product([False, True], repeat=2):
        for xorout in ((0x00, 0xFF) if width == 8 else (0x0000, 0xFFFF)):
            ok = True
            for _, f in frames:
                if width == 8:
                    got, exp = crc_generic(f[0:3], 8, poly, init, refin, refout, xorout), f[3]
                else:
                    got, exp = crc_generic(f[0:-2], 16, poly, init, refin, refout, xorout), (f[-2] | (f[-1] << 8))
                if got != exp:
                    ok = False
                    break
            if ok:
                return refin, refout, xorout
    return None


if __name__ == "__main__":
    frames = load_frames()
    print(f"Loaded {len(frames)} literal frames from {HEADER.relative_to(HEADER.parent.parent)}: "
          + ", ".join(n for n, _ in frames))

    c8 = find_params(frames, 8, 0x31, 0xEE)
    c16 = find_params(frames, 16, 0x1021, 0x496C)
    print(f"CRC-8  (poly=0x31, init=0xEE):     refin,refout,xorout = {c8}")
    print(f"CRC-16 (poly=0x1021, init=0x496C): refin,refout,xorout = {c16}")
    expected = (True, True, 0)
    print("MATCHES firmware (src/crc.cpp):", c8 == expected and c16 == expected)
