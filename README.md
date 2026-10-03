# Voltra Knob Controller

Firmware for the **Waveshare ESP32-S3-Knob-Touch-LCD-1.8** that connects
directly to a **Beyond Power VOLTRA** over Bluetooth LE and lets you adjust
base weight and eccentric weight with the knob, and start/stop the load
with a tap, right from the device's own screen -- no phone needed.

No cloud, no bridge device: the ESP32-S3's own BLE radio talks straight to
the Voltra's BLE GATT service.

**If you bought the Elecrow CrowPanel 2.1" instead (or as well):** don't use
this project for it. Use
[omarshahine/voltra-knob](https://github.com/omarshahine/voltra-knob)
directly and unmodified -- it's a mature, real, working implementation built
specifically for that board, with more features than this one (auto-load
arming, rep counting, battery, safety-limited weight changes while loaded,
reconnect-with-pinned-address). See "Building for the Elecrow CrowPanel
2.1"" below for the exact commands. This project exists because the
Waveshare board needs different display/touch/encoder drivers -- the BLE
protocol underneath is identical, and in fact this project's v2 was
corrected and verified using that project's real, captured protocol data.

## What's implemented

- Connects to the Voltra over BLE: scans for its GATT service, authenticates
  with a custom "Voltra Knob" identity, enters weight-training mode,
  subscribes to telemetry, reads current settings.
- Round-screen UI: connection status, a big number for the active value,
  tap the label to switch between **BASE WEIGHT** and **ECCENTRIC**, turn
  the knob to adjust by 1 lb per detent.
- A load button with three actions, since this board's knob has no physical
  button (see "Auto-load arming" below for why that matters):
  - **Tap** while idle: load now (plain strength mode).
  - **Tap** while loaded or armed: stop / cancel, instantly.
  - **Hold** (~0.6s) while idle: arm auto-load -- the device runs its own
    safety check and countdown, then engages on your first pull. The screen
    shows "ARMED - pull to start", then a countdown, then "ENGAGING...".
  - While loaded, a weight change is capped to +/-25 lb from the confirmed
    weight (same safety limit as the reference implementation).
- **Rep counter**: shows live during a set (`REPS: N`), from the same
  per-rep BLE frames the reference implementation uses; resets to 0 each
  time a fresh load starts.
- **Battery percentage**: shown top-right whenever the Voltra has pushed a
  reading (it's unsolicited -- there's no read command for it, so it shows
  `--` until the first one arrives after connecting).
- Serial log (115200 baud) of every BLE frame sent/received, in hex, for
  debugging.

Not implemented: resistance-band/damper/isokinetic modes (this firmware only
does plain strength mode + auto-load, same as the reference implementation).

One real limitation vs. the Elecrow reference project: connecting/
handshaking here blocks the main loop for a few seconds (the reference
project runs BLE on its own FreeRTOS task so the UI never freezes). The
screen will visibly pause during (re)connect. Fine for now; moving the BLE
work to its own task on core 0 (`xTaskCreatePinnedToCore`, exactly like the
reference project does) is the natural next improvement.

## How this was built (and what to trust how much)

Beyond Power hasn't published a public API for the Voltra's local BLE
protocol. This firmware's protocol layer (`include/voltra_protocol.h`,
`src/voltra_ble.cpp`) is now a direct, verified port of
[omarshahine/voltra-knob](https://github.com/omarshahine/voltra-knob) --
a real, working project (built for different hardware, the Elecrow
CrowPanel 2.1", but the same Voltra) whose own README states its protocol
tables trace back to the `@voltras/node-sdk` npm package. Before trusting
any of it, every one of that project's 212 captured/generated frames (all
196 weight values 5-200 lb, plus every named handshake/mode/telemetry
frame) was independently checked against this firmware's own CRC-8/CRC-16
implementation, and all 212 validated exactly -- see `tools/verify_crc.py`,
which re-derives the CRC parameters straight from the literal frames now
embedded in `voltra_protocol.h`, and the firmware re-runs the same check on
every boot (watch the serial log for `[crc-selftest]` -- every line should
say `OK`/`OK`, no exceptions this time).

Concretely, that means:

1. **The BLE handshake is byte-exact and confirmed.** `AUTH_KNOB`, `INIT0`,
   `INIT1`, and `MODE_WEIGHT_TRAINING` are sent as literal bytes from the
   working reference project, not reconstructed.
2. **Parameter IDs, units, and the write payload shape are now confirmed,
   not guessed.** Base weight (`0x3E86`), eccentric weight (`0x3E88`), and
   engage/disengage (`0x3E89`, write `0x0005`/`0x0004`) all match the
   reference project's real, deployed code, and the generic write-frame
   builder in `voltra_protocol.cpp` reproduces its 196 real captured
   weight-set frames byte-for-byte (mod the sequence number, which
   naturally differs per-session).
3. **Response parsing** (`handleFrame()` in `voltra_ble.cpp`) is ported from
   that project's real notification decoder, not a heuristic scan anymore --
   including the auto-load status registers, rep-count frames, and battery
   push notifications added for those three features.
4. **Auto-load arming, rep counting, and battery** all use the reference
   project's own literal frames/registers, not new guesses:
   `GUIDED_LOAD_TRIGGER` and the guided-load status read are sent
   byte-for-byte as that project captured them; the rep-count and battery
   frame shapes (including the one "looks like battery but isn't" frame
   that has to be excluded first) are ported from its decoder verbatim.

An earlier version of this file was built from a different, independent
reverse-engineering project and had gotten the write and notify
characteristic UUIDs backwards -- a real bug, now fixed by cross-checking
against a second, working implementation.

### What "verified" means here, precisely

This firmware itself has now been **compiled clean** -- a full build,
against the real pioarduino toolchain, real NimBLE-Arduino and Arduino_GFX
libraries, zero warnings from any of this project's own source files -- not
just reviewed by eye. (Earlier revisions of this README said compiling
wasn't possible in the environment this was built in; that sandbox has
since gotten PlatformIO and a route to the toolchain, and it does compile.)
What that confirms: the code is well-formed C++, every BLE frame/struct
layout matches across files, and it links against the real libraries with
the real board config. What it does **not** confirm: that any of this
actually works against a live Voltra. Nobody has pulled the cable with this
firmware running yet -- that first real-hardware test, especially of the
new hold-to-arm gesture and the auto-load countdown/engage sequence, is the
next real milestone, and it's the same kind of "verified the parts we can,
flagged the part we can't" as everything else in this README.

### If something still doesn't work on real hardware

Open the serial monitor (115200 baud) and watch the `[voltra] rx ...` hex
dumps. Given the level of verification above, a full communication failure
is unlikely; a more likely snag is the Voltra refusing the connection
because the Beyond+ app is still connected to it (the trainer accepts one
controller at a time -- fully close the app on your phone first), or the
first-time "Voltra Knob wants to connect" prompt on the trainer's own
screen needing to be accepted once.

## Building for the Elecrow CrowPanel 2.1"

If that's the board in hand, skip everything else in this file and follow
[omarshahine/voltra-knob](https://github.com/omarshahine/voltra-knob)'s own
README directly -- it's `arduino-cli`-based, not PlatformIO, with libraries
vendored for a reproducible build:

```bash
git clone https://github.com/omarshahine/voltra-knob
cd voltra-knob
brew install arduino-cli uv            # or your platform's equivalents
arduino-cli config init
arduino-cli config set board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install "NimBLE-Arduino"

tools/build.sh   # compile
tools/flash.sh   # upload over USB (first /dev/cu.usbmodem*; set PORT= to override)
uv run --with pyserial tools/serial_tail.py --seconds 10   # watch the serial log
```

Before powering on: fully close the Beyond+ app on your phone (the trainer
accepts one controller at a time), then accept "Voltra Knob wants to
connect" on the trainer's screen the first time. Everything else -- turn to
set weight, tap to load, hold to arm auto-load, tap to release -- is
described in that project's README.

## Hardware reference (Waveshare ESP32-S3-Knob-Touch-LCD-1.8)

| Function | Pins |
|---|---|
| Display (ST77916, QSPI) | CLK=13 CS=14 D0=15 D1=16 D2=17 D3=18 RST=21, Backlight=47 |
| Touch (CST816, I2C, addr 0x15) | SDA=11 SCL=12 INT=9 RST=10 |
| Rotary knob (quadrature) | A=8 B=7 (no push-button -- tap/hold the screen instead) |
| Haptics (DRV2605, unused in v1) | shares the touch I2C bus, addr 0x5A |

This board actually has a second onboard MCU (ESP32-U4WDH) that handles
audio output and a second knob -- irrelevant here, this firmware only uses
the main ESP32-S3.

If your knob turns the wrong way, or one click moves the value by 2 or 4
instead of 1, adjust `kEdgesPerDetent` in `src/encoder.cpp`. If taps land
on the wrong spot on screen, see `mapTouch()` in `src/ui.cpp`. If the
hold-to-arm gesture fires too eagerly (or not eagerly enough) on the load
button, adjust `kHoldMs` near the top of `src/ui.cpp` (600 ms by default).

## Building and flashing

This is a **PlatformIO** project (recommended over the plain Arduino IDE
here -- it pins exact library versions so this keeps building the same way
months from now).

1. Install VS Code, then the **PlatformIO IDE** extension from its
   marketplace (search "PlatformIO IDE"). It bundles everything else
   (toolchains, `pio` CLI) automatically on first use -- no separate
   installer needed.
2. Open this folder (`voltra-knob-controller/`) in VS Code -- PlatformIO
   will detect `platformio.ini` and show a PlatformIO icon in the sidebar.
3. Plug the board in over USB-C (the port that talks to the ESP32-S3, not
   the secondary chip -- the board silkscreen usually marks this, and it's
   normally the port closer to the display ribbon).
4. Click the checkmark (Build) in the bottom status bar first, to make sure
   everything compiles for you. This has now been compiled clean end-to-end
   (real pioarduino toolchain, real NimBLE-Arduino + Arduino_GFX, zero
   warnings) before being sent to you, so this should just work -- but if
   your installed library versions differ enough to error anyway, paste me
   the exact error and I'll fix it.
5. Click the right-arrow (Upload) to flash.
6. Open the Serial Monitor (plug icon, or `pio device monitor`) at 115200
   baud to watch connection/debug logs.

If PlatformIO can't find a new-enough Arduino-ESP32 core for the octal
PSRAM setting (`board_build.arduino.memory_type = qio_opi`), switch the
`platform =` line in `platformio.ini` to the community `pioarduino` fork
(commented right above it) -- that's what Waveshare's own demo code for
this board builds against.

## Project layout

```
platformio.ini              board/toolchain/library configuration
include/voltra_protocol.h   BLE UUIDs, frame format, parameter IDs, captured bootstrap bytes
src/crc.{h,cpp}              verified CRC-8/CRC-16 + boot self-test
src/voltra_protocol.cpp      frame builders (param read/write)
src/voltra_ble.{h,cpp}       BLE connection state machine, NimBLE glue
src/display.{h,cpp}          ST77916 QSPI panel + PSRAM canvas setup
src/touch_cst816.{h,cpp}     CST816 touch polling
src/encoder.{h,cpp}          quadrature knob decoding
src/ui.{h,cpp}                screen layout, touch zones, drawing
src/main.cpp                  wiring it all together
tools/verify_crc.py           standalone re-derivation of the CRC parameters
```

## License

Open source under the [MIT License](LICENSE) -- free to use, modify,
redistribute, and build on, including building your own firmware for this
hardware. The BLE protocol layer was ported from
[omarshahine/voltra-knob](https://github.com/omarshahine/voltra-knob) (also
MIT); see [THIRD_PARTY.md](THIRD_PARTY.md) for full third-party attribution.
This project is not affiliated with or endorsed by Beyond Power.

## Safety note

This talks directly to a piece of exercise equipment that applies real
force. Test the START/STOP button and weight changes with the cable
unloaded (nothing attached, standing clear) before trusting it during an
actual set, the same way you'd test any new remote for a cable machine.
