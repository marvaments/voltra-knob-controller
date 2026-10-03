# Third-Party Components in Voltra Knob Controller

## Protocol layer (the big one)

**`include/voltra_protocol.h` and `src/voltra_ble.cpp`** are a direct, verified
port of [omarshahine/voltra-knob](https://github.com/omarshahine/voltra-knob)
(MIT License, Copyright (c) 2026 Omar Shahine) -- a working implementation of
the same Beyond Power Voltra BLE protocol, built for different hardware (the
Elecrow CrowPanel 2.1"). Every byte-array literal this project uses (the
handshake frames, all 196 weight-table entries, parameter IDs, and the
response-decoding logic in `handleFrame()`) was checked against this
project's own CRC-8/CRC-16 implementation before being trusted -- see
`tools/verify_crc.py` and the full explanation in `README.md`.

That project's own protocol tables trace back to the **`@voltras/node-sdk`**
npm package by Henry Jewkes (MIT License) -- the original decode of Beyond
Power's unpublished local BLE protocol.

Beyond Power has not published a public API for this protocol; none of this
project's code comes from Beyond Power, and this project is not affiliated
with, endorsed by, or supported by Beyond Power.

## Libraries (pulled in by PlatformIO, each ships its own license)

| Component | License | Source |
|---|---|---|
| NimBLE-Arduino | Apache-2.0 | github.com/h2zero/NimBLE-Arduino |
| GFX Library for Arduino (Arduino_GFX) | Per upstream | github.com/moononournation/Arduino_GFX |
| Arduino core for ESP32 (pioarduino fork) | Per upstream | github.com/pioarduino/platform-espressif32 |

PlatformIO downloads each of these into `.pio/libdeps/` at build time, license
file included -- they are not vendored into this repo's own source tree.

## Fonts

`src/fonts/*.h` are generated from DejaVu Sans (a Bitstream Vera derivative)
via Adafruit's `fontconvert` tool -- see the banner comment at the top of each
font file for the exact provenance and license of the source typeface.

## Everything else

Everything not listed above -- the UI (`src/ui.cpp`), rep/set/drop-set
tracking, Wi-Fi provisioning, OTA update client, weight presets, haptics, and
all other application logic -- is original work for this project, licensed
under this repo's own LICENSE (MIT).
