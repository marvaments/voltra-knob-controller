#pragma once
#include <cstdint>

// Pull-based firmware updates: unlike main.cpp's ArduinoOTA/espota path
// (a developer on the same LAN PUSHING a build from PlatformIO to a board
// they can reach directly), this is what a customer-owned unit uses on its
// own: it reaches out to a fixed URL -- the internet in production, or a
// laptop running a plain "python3 -m http.server" on the same WiFi while
// testing, see tools/serve_ota_test.py -- checks whether a newer
// firmware.bin than the one it's currently running is available, and if so
// downloads and flashes it itself. No PlatformIO, no LAN access to a dev
// machine, no physical access required at all.
//
// Server contract (deliberately just static files -- works unchanged
// whether it's a python http.server, GitHub Releases, or an S3/R2 bucket,
// no server-side logic needed):
//   <kOtaUpdateBaseUrl>/latest.txt              -- one line, the newest
//                                                  available version string
//   <kOtaUpdateBaseUrl>/firmware-<version>.bin  -- that version's compiled
//                                                  binary (.pio/build/.../firmware.bin)
//   <kOtaUpdateBaseUrl>/firmware-<version>.bin.sha256
//                                                -- `sha256sum` output for
//                                                   that file, verified
//                                                   automatically by
//                                                   HTTPUpdate before it
//                                                   ever writes a byte to
//                                                   flash (see .cpp)
//
// Kept entirely user-initiated for now (Settings > Firmware Update >
// "CHECK NOW", then a second tap to actually install) rather than
// automatic background polling -- main.cpp's WiFi/BLE coexistence comment
// already flags why a surprise multi-second download mid-workout would be
// a bad idea, and a single confirmed tap is trivially easy to test
// deterministically compared to a timer-driven background check.
namespace otaupdate {

enum class State {
  Idle,            // nothing checked yet, or the last check found nothing new
  Checking,        // fetching latest.txt right now
  UpdateAvailable, // a newer version was found (see Status::availableVersion), waiting for confirm
  Downloading,     // download+flash in progress (Status::progressPercent valid)
  Error,           // last check or update attempt failed (see Status::message)
};

struct Status {
  State state = State::Idle;
  char availableVersion[20] = ""; // valid once state == UpdateAvailable/Downloading
  uint8_t progressPercent = 0;    // valid while Downloading
  // Short, fixed-vocabulary error text (e.g. "No Wi-Fi", "Server error") --
  // valid only while state == Error. Deliberately always one of a small set
  // of pre-measured strings (see ota_update.cpp), never a raw library error
  // string: the UI shows this centered in the big bold font with real
  // round-display clearance limits (see ui.cpp's drawFirmwareUpdateScreen),
  // and an arbitrary-length string from e.g. HTTPUpdate's own error text
  // could run off the edge. Full diagnostic detail always goes to Serial
  // instead, alongside this short version.
  char message[24] = "";
};

// Call once from setup().
void begin();

// Call every loop() iteration. This is where the actual blocking network
// work happens (see checkForUpdate()/beginInstall() below) -- both of
// those return immediately after just flipping state, so the screen gets
// one real frame showing "Checking..."/"Installing..." before this
// function performs the multi-second blocking call on the NEXT loop()
// tick. Without that one-tick deferral, the state change and the blocking
// call would happen back-to-back inside the same touch-dispatch call that
// requested it, and the UI would just freeze with no feedback until it was
// already done.
void update();

Status status();

// Requests a check for a newer version. Returns immediately (see update()'s
// comment on why) after validating there's something to check at all
// (kOtaUpdateBaseUrl configured, Wi-Fi connected) and flipping state to
// Checking -- the actual latest.txt fetch and version comparison against
// fw::kVersion happens on the next update() call. No-op if a check or
// install is already in progress.
void checkForUpdate();

// Confirms and requests installing the version checkForUpdate() found
// (state must be UpdateAvailable). Returns immediately after flipping state
// to Downloading -- the actual download+flash happens on the next update()
// call, blocking it for the whole transfer (same tradeoff the existing
// ArduinoOTA push path already makes), and reboots into the new firmware on
// success without returning. workoutActive gates the install itself (not
// the check above) against a live set -- pass
// voltraClient.state().armed || .loaded.
void beginInstall(bool workoutActive);

} // namespace otaupdate
