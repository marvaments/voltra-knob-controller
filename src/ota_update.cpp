#include "ota_update.h"
#include "wifi_provision.h"
#include "wifi_secrets.h" // kOtaUpdateBaseUrl
#include "version.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <NetworkClientSecure.h>

namespace otaupdate {

namespace {

Status sStatus;

// Set by checkForUpdate()/beginInstall() to defer their actual blocking
// network work to the next update() call -- see update()'s header comment
// for why. Only one can ever be pending at a time (both setters refuse to
// run while state is already Checking or Downloading).
enum class Pending { None, Check, Install };
Pending sPending = Pending::None;

// Splits "MAJOR.MINOR.PATCH" into three ints, treating any missing or
// unparsable segment as 0 -- good enough for this project's own
// hand-bumped version.h scheme (see that file's comment on why it's just
// one constant, not a build-date/git-hash scheme), not a general semver
// parser (no pre-release/build-metadata suffixes to worry about here).
void parseVersion(const char *v, int out[3]) {
  out[0] = out[1] = out[2] = 0;
  int part = 0, value = 0;
  bool any = false;
  for (const char *p = v;; p++) {
    if (*p >= '0' && *p <= '9') {
      value = value * 10 + (*p - '0');
      any = true;
    } else {
      if (any && part < 3) out[part] = value;
      part++;
      value = 0;
      any = false;
      if (*p == '\0' || part >= 3) break;
    }
  }
}

bool isNewerVersion(const char *candidate, const char *current) {
  int c[3], u[3];
  parseVersion(candidate, c);
  parseVersion(current, u);
  for (int i = 0; i < 3; i++) {
    if (c[i] != u[i]) return c[i] > u[i];
  }
  return false;
}

void setError(const char *msg) {
  sStatus.state = State::Error;
  snprintf(sStatus.message, sizeof(sStatus.message), "%s", msg);
}

// The actual latest.txt fetch + version compare, run from update() one
// loop tick after checkForUpdate() flips state to Checking.
void runCheck() {
  String url = String(kOtaUpdateBaseUrl) + "/latest.txt";
  // Real TLS validation against the OTA host, not setInsecure() -- this
  // fetch is what tells the device a new version exists at all, and once
  // this ships to units on other people's networks a spoofed/tampered
  // response here is exactly the kind of thing HTTPS is for. IDF's builtin
  // certificate bundle (the same broad public-CA trust set a browser or
  // phone would use) means this isn't pinned to one specific host's cert,
  // so it keeps working across a future host/cert change with no firmware
  // update needed.
  NetworkClientSecure secureClient;
  secureClient.useBuiltinCACertBundle();
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  if (!http.begin(secureClient, url)) {
    setError("Can't connect");
    return;
  }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    // Full HTTP status goes to Serial for debugging; the on-screen message
    // stays short and fixed -- see ota_update.h's comment on
    // Status::message (this file only ever writes short, pre-measured
    // strings there, since the UI shows it centered in the big bold font
    // with real round-display clearance limits -- see ui.cpp's
    // drawFirmwareUpdateScreen()).
    Serial.printf("[ota_update] GET %s -> HTTP %d\n", url.c_str(), code);
    http.end();
    setError("Server error");
    return;
  }
  String body = http.getString();
  http.end();
  body.trim();

  if (body.length() == 0 || body.length() >= sizeof(sStatus.availableVersion)) {
    Serial.printf("[ota_update] bad latest.txt body: %s\n", body.c_str());
    setError("Bad response");
    return;
  }

  if (isNewerVersion(body.c_str(), fw::kVersion)) {
    snprintf(sStatus.availableVersion, sizeof(sStatus.availableVersion), "%s", body.c_str());
    sStatus.state = State::UpdateAvailable;
  } else {
    // Nothing newer -- back to Idle with no message. The UI shows the
    // currently-running version for Idle regardless of whether that's
    // because nothing's been checked yet or a check just confirmed it's
    // current; either way "here's what's running" is the honest thing to
    // show, without this module having to track which of those it was.
    sStatus.state = State::Idle;
    sStatus.message[0] = '\0';
  }
}

// The actual download+flash, run from update() one loop tick after
// beginInstall() flips state to Downloading.
void runInstall() {
  String version = sStatus.availableVersion;
  String firmwareUrl = String(kOtaUpdateBaseUrl) + "/firmware-" + version + ".bin";
  String sha256Url = firmwareUrl + ".sha256";

  // Same reasoning as runCheck() above: real cert validation via IDF's
  // builtin CA bundle, not a plain unauthenticated NetworkClient. Every
  // fetch HTTPUpdate makes (the firmware .bin AND its .sha256 sidecar) goes
  // through this one client, so both are protected -- the SHA256 check
  // alone only proves the download matches what the server sent, not that
  // the server wasn't spoofed.
  NetworkClientSecure client;
  client.useBuiltinCACertBundle();
  httpUpdate.rebootOnUpdate(true);
  // Fetches <firmwareUrl>.sha256 (plain `sha256sum firmware.bin` output --
  // HTTPUpdate's sidecar parser scans for a bare 64-hex-char token, so no
  // special formatting is needed) and refuses to flash unless the
  // downloaded bytes hash to match. This is the actual integrity check for
  // every install -- a corrupted download or a tampered file gets rejected
  // before a single byte reaches the inactive OTA partition, not after.
  httpUpdate.setSHA256sumUrl(sha256Url);
  httpUpdate.onProgress([](int cur, int total) {
    if (total > 0) sStatus.progressPercent = (uint8_t)((cur * 100) / total);
  });

  t_httpUpdate_return ret = httpUpdate.update(client, firmwareUrl);
  // Reaching here at all means rebootOnUpdate(true) did NOT reboot us --
  // i.e. this only runs on failure. A successful install reboots from
  // inside httpUpdate.update() and never returns.
  //
  // httpUpdate.getLastErrorString() is logged in full to Serial (it can be
  // an arbitrary-length library string -- e.g. a checksum-mismatch or
  // partition-size message), but the on-screen message stays one short
  // fixed string, same reasoning as runCheck()'s errors above.
  Serial.printf("[ota_update] install failed (result=%d): %s\n", (int)ret, httpUpdate.getLastErrorString().c_str());
  setError(ret == HTTP_UPDATE_NO_UPDATES ? "No update found" : "Update failed");
}

} // namespace

void begin() {
  sStatus = Status{};
  sPending = Pending::None;
}

Status status() {
  return sStatus;
}

void checkForUpdate() {
  if (sStatus.state == State::Checking || sStatus.state == State::Downloading) return;

  if (kOtaUpdateBaseUrl[0] == '\0') {
    setError("Not configured");
    return;
  }
  if (wifiprov::status().state != wifiprov::State::Connected) {
    setError("No Wi-Fi");
    return;
  }

  sStatus.state = State::Checking;
  sStatus.message[0] = '\0';
  sPending = Pending::Check; // runCheck() happens on the next update() tick
}

void beginInstall(bool workoutActive) {
  if (sStatus.state != State::UpdateAvailable) return;
  if (workoutActive) {
    setError("Finish set first");
    return;
  }

  sStatus.state = State::Downloading;
  sStatus.progressPercent = 0;
  sPending = Pending::Install; // runInstall() happens on the next update() tick
}

void update() {
  if (sPending == Pending::None) return;
  Pending action = sPending;
  sPending = Pending::None;
  if (action == Pending::Check) {
    runCheck();
  } else if (action == Pending::Install) {
    runInstall();
  }
}

} // namespace otaupdate
