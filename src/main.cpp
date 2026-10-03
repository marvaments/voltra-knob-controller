#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include "display.h"
#include "touch_cst816.h"
#include "encoder.h"
#include "voltra_ble.h"
#include "board_battery.h"
#include "wifi_provision.h"
#include "wifi_secrets.h" // only kOtaPassword left in here -- WiFi credentials are never compiled in, see wifi_provision.cpp
#include "ota_update.h"
#include "prefs.h"
#include "weight_presets.h"
#include "haptics.h"
#include "version.h"
#include "ui.h"

// Waveshare ESP32-S3-Knob-Touch-LCD-1.8 pins (see display.h for the QSPI
// panel pins). Touch (CST816) and haptics share this I2C bus.
static constexpr int kTouchSda = 11;
static constexpr int kTouchScl = 12;
static constexpr int kTouchInt = 9;
static constexpr int kTouchRst = 10;
static constexpr int kEncoderA = 8;
static constexpr int kEncoderB = 7;

static Arduino_Canvas *gCanvas = nullptr;

// Idle-driven backlight dimming. Any touch, encoder turn, or an actively
// in-progress workout (armed -- which covers waiting/countdown/engaging --
// or loaded) counts as activity and immediately snaps back to full
// brightness; otherwise brightness steps down the longer nothing happens.
// Deliberately a simple step function (no fade animation) to keep this
// easy to reason about -- a fade could be layered on top later without
// touching the activity-tracking logic below.
//
// "Full" brightness and the dim delay used to be the hardcoded constants
// this comment's history still describes (100%, dim at 15s, off at 60s) --
// both are now user-adjustable from Settings > Brightness / Auto-Dim (see
// prefs.h), read fresh on every call so a change takes effect immediately
// without needing a reboot. Auto-dim's "Never" preset is prefs::autoDimSeconds()
// == 0, which this function reads as "skip dimming/blanking entirely, stay
// at full brightness always." The dim level itself (kDimBacklightPercent)
// and the extra idle time before going fully blank after dimming
// (kBlankAfterDimMs) stay fixed rather than user-facing -- mirrors this
// project's original 15s-dim/60s-blank spacing (45s further after dimming).
static constexpr uint8_t kDimBacklightPercent = 12; // still legible, well below any brightness preset
static constexpr uint32_t kBlankAfterDimMs = 45000; // fully off this long after dimming
static uint32_t sLastActivityMs = 0;
static uint8_t sBacklightPercent = 100;

static void updateBacklight(bool inputActivity, const voltra::VoltraState &vst) {
  uint32_t now = millis();
  bool workoutActive = vst.loaded || vst.armed;
  if (inputActivity || workoutActive) sLastActivityMs = now;

  uint8_t fullBrightness = prefs::brightnessPercent();
  uint32_t dimDelayMs = (uint32_t)prefs::autoDimSeconds() * 1000UL;

  uint8_t target = fullBrightness;
  if (dimDelayMs > 0) {
    uint32_t idleMs = now - sLastActivityMs;
    if (idleMs >= dimDelayMs + kBlankAfterDimMs) target = 0;
    else if (idleMs >= dimDelayMs) target = kDimBacklightPercent;
  }

  if (target != sBacklightPercent) {
    sBacklightPercent = target;
    display::setBacklight(target);
  }
}

// ArduinoOTA, so firmware can be updated over the air instead of always
// needing a USB cable. WiFi connectivity itself -- including on-device
// network selection/password entry via the Settings > Wi-Fi screen, NVS
// persistence, and reconnect/backoff -- moved to wifi_provision.*, since no
// unit (customer-owned or your own dev/bench board) has a WiFi SSID baked
// in at compile time any more; every board joins a network by provisioning
// itself via the Settings > Wi-Fi captive portal at least once. This file
// just asks wifi_provision::status() whether it's safe to run ArduinoOTA
// right now. kOtaPassword (below) is the one piece of wifi_secrets.h this
// file still uses directly.
//
// Worth knowing: this ESP32-S3 has one 2.4GHz radio shared between WiFi and
// Bluetooth via its coexistence firmware. That's a very common combination
// and generally works fine, but WiFi/BLE contention is a real, observed
// source of both a dropped/laggy BLE connection while an OTA push is in
// progress, AND of WiFi itself briefly dropping out on its own -- if either
// is disruptive, avoid pushing OTA updates (or running Wi-Fi setup) mid-set.
static bool sOtaStarted = false;

// Per-board unique hostname/mDNS name, derived from this chip's own MAC
// address (last 3 bytes, as hex) rather than a fixed string. With more than
// one of these boards on the same network, a shared hardcoded hostname like
// "voltra-knob" would collide -- mDNS's own conflict resolution would
// silently rename the losers to "voltra-knob-2", "voltra-knob-3", etc, and
// which physical board ends up with which name would depend on power-on
// order and isn't something you can predict or rely on. A MAC-derived
// suffix is stable per physical board across reboots instead, and still
// starts with "voltra-knob" so searching that prefix in the UniFi
// controller's client list surfaces all of them at once.
static char sOtaHostname[32];
static void computeOtaHostname() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(sOtaHostname, sizeof(sOtaHostname), "voltra-knob-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

static void otaLoop() {
  wifiprov::Status wst = wifiprov::status();
  if (wst.state != wifiprov::State::Connected) {
    sOtaStarted = false; // cleanly ArduinoOTA.begin() again next time we (re)connect
    return;
  }
  if (!sOtaStarted) {
    computeOtaHostname(); // WiFi is already in STA mode with a valid MAC by now -- wifi_provision.cpp guarantees it
    Serial.printf("[main] WiFi connected (%s) as \"%s\" -- starting OTA listener\n", wst.ip, sOtaHostname);
    ArduinoOTA.setHostname(sOtaHostname);
    if (kOtaPassword[0] != '\0') ArduinoOTA.setPassword(kOtaPassword);
    // Default is 1000ms, retried 3x (~3s total) before ArduinoOTA gives up
    // on a stalled transfer as OTA_RECEIVE_ERROR (confirmed by reading
    // ArduinoOTA.cpp's _runUpdate()). That's tight enough that a brief
    // WiFi/BLE radio-coexistence stall can abort an otherwise-fine upload
    // partway through. Stretched to give real hiccups more room to resolve
    // themselves instead of tanking the whole transfer.
    ArduinoOTA.setTimeout(5000);
    ArduinoOTA.onStart([]() { Serial.println("[ota] update starting"); });
    ArduinoOTA.onEnd([]() { Serial.println("[ota] update complete, rebooting"); });
    ArduinoOTA.onError([](ota_error_t error) { Serial.printf("[ota] error [%u]\n", (unsigned)error); });
    ArduinoOTA.begin();
    sOtaStarted = true;
  }
  ArduinoOTA.handle();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n[main] Voltra Knob Controller booting... (firmware v%s)\n", fw::kVersion);

  prefs::begin();
  wpresets::begin();
  gCanvas = display::begin();
  // No boot splash here anymore -- the LiftLayered logo screen (and its
  // fixed 1.5s hold) was removed at Mike's request; the knob now goes
  // straight from power-on into touch/encoder/BLE init below. See
  // bootscreen.cpp/bootlogo.h if this ever needs reviving -- both files are
  // left in the repo, just no longer called from here.
  touch::begin(kTouchSda, kTouchScl, kTouchRst, kTouchInt);
  haptics::begin(); // after touch::begin() -- reuses the I2C bus touch::begin() already initialized
  encoder::begin(kEncoderA, kEncoderB);
  board_battery::begin();

  ui::begin(gCanvas);

  voltra::gVoltra.begin();
  wifiprov::begin();
  otaupdate::begin();

  Serial.println("[main] setup complete");
}

void loop() {
  voltra::gVoltra.update();
  board_battery::update(); // rate-limited internally; cheap to call every loop
  wifiprov::update();
  otaLoop();          // ArduinoOTA -- YOU pushing a build from PlatformIO, LAN-only
  otaupdate::update(); // pull updates -- the board itself checking/downloading, see ota_update.h

  int32_t delta = encoder::readDelta();
  touch::TouchPoint tp = touch::read();

  updateBacklight(delta != 0 || tp.touched, voltra::gVoltra.state());
  ui::update(delta, tp, voltra::gVoltra, board_battery::percent());

  delay(20); // ~50 Hz input poll; plenty responsive for a workout control knob
}
