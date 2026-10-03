#include "wifi_provision.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <cctype>

namespace wifiprov {
namespace {

constexpr const char *kPrefsNamespace = "voltra_wifi";
constexpr const char *kKeySsid = "ssid";
constexpr const char *kKeyPass = "pass";

// Same fast-then-slow retry shape main.cpp's old OTA WiFi loop used: quick
// retries cover a brief coexistence-type drop resolving itself within
// seconds, then back off hard so a board that's genuinely out of range
// (e.g. taken to the gym) doesn't keep hammering WiFi.begin() forever.
constexpr uint32_t kFastRetryMs = 15000;
constexpr uint8_t kFastRetryAttempts = 4;
constexpr uint32_t kSlowRetryMs = 5 * 60 * 1000;

WebServer sServer(80);
DNSServer sDns;
bool sServerActive = false;
bool sRoutesRegistered = false;

State sState = State::Disconnected;
char sSavedSsid[33] = "";
char sSavedPass[65] = ""; // WPA2 passphrases are at most 63 chars
bool sHasSaved = false;
char sApName[24] = "";

uint32_t sAttemptStartMs = 0;
uint8_t sFailCount = 0;

// ---------------------------------------------------------------------------
// NVS persistence (Preferences, same mechanism ui.cpp uses for the theme
// choice). Namespaced separately ("voltra_wifi") so it can't collide with
// ui.cpp's "voltra_ui" namespace or anything else that shows up later.

void loadSaved() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/true);
  String ssid = prefs.getString(kKeySsid, "");
  String pass = prefs.getString(kKeyPass, "");
  prefs.end();
  if (ssid.length() > 0) {
    strncpy(sSavedSsid, ssid.c_str(), sizeof(sSavedSsid) - 1);
    strncpy(sSavedPass, pass.c_str(), sizeof(sSavedPass) - 1);
    sHasSaved = true;
    return;
  }
  // Nothing provisioned on-device yet. Deliberately no compiled-in
  // fallback here -- every board, dev units included, has to go through
  // startProvisioning() (SoftAP + captive portal, Settings > Wi-Fi > ADD
  // WIFI) at least once. This guarantees no real WiFi credentials can ever
  // end up baked into a firmware image, mass-flashed or otherwise -- there
  // is simply no field in wifi_secrets.h to accidentally leave filled in.
}

void saveCreds(const char *ssid, const char *pass) {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/false);
  prefs.putString(kKeySsid, ssid);
  prefs.putString(kKeyPass, pass);
  prefs.end();
  strncpy(sSavedSsid, ssid, sizeof(sSavedSsid) - 1);
  sSavedSsid[sizeof(sSavedSsid) - 1] = '\0';
  strncpy(sSavedPass, pass, sizeof(sSavedPass) - 1);
  sSavedPass[sizeof(sSavedPass) - 1] = '\0';
  sHasSaved = true;
}

void clearSaved() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/false);
  prefs.remove(kKeySsid);
  prefs.remove(kKeyPass);
  prefs.end();
  sSavedSsid[0] = '\0';
  sSavedPass[0] = '\0';
  sHasSaved = false;
}

// ---------------------------------------------------------------------------
// STA connect/retry, extracted from what used to be main.cpp's otaSetup()/
// otaLoop() -- this file now owns the radio, main.cpp just asks status().

void beginStaConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(sSavedSsid, sSavedPass);
  sAttemptStartMs = millis();
  sFailCount = 0;
  sState = State::Connecting;
  Serial.printf("[wifiprov] connecting to \"%s\"...\n", sSavedSsid);
}

// ---------------------------------------------------------------------------
// Captive portal. Deliberately a plain HTTP page on an open (no-password)
// temporary SoftAP -- the same tradeoff most consumer IoT setup flows make
// (smart plugs/bulbs, Chromecast, etc.): the AP only exists for the minute
// or two setup takes, and a self-signed HTTPS cert would just make phones
// throw scarier warnings than an open network already does, for a page that
// only ever carries credentials over a link nothing else is using yet.

String urlEncode(const String &s) {
  String out;
  out.reserve(s.length() * 3);
  static const char *kHex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      out += '%';
      out += kHex[(c >> 4) & 0xF];
      out += kHex[c & 0xF];
    }
  }
  return out;
}

String htmlEscape(const String &s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    switch (s[i]) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += s[i];
    }
  }
  return out;
}

const char *kHtmlHead =
  "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>Voltra Knob Setup</title><style>"
  "body{font-family:-apple-system,Helvetica,Arial,sans-serif;background:#111;color:#eee;"
  "margin:0;padding:24px 16px;}h1{font-size:20px;margin:0 0 4px}"
  "p{color:#aaa;font-size:14px;margin:0 0 20px}"
  "a.net{display:block;padding:14px;margin-bottom:8px;background:#222;border-radius:10px;"
  "color:#eee;text-decoration:none;font-size:16px}"
  "a.net span{float:right;color:#888;font-size:13px}"
  "form{margin-top:8px}input{width:100%;box-sizing:border-box;padding:12px;font-size:16px;"
  "margin-bottom:12px;border-radius:8px;border:1px solid #444;background:#1a1a1a;color:#eee}"
  "button{width:100%;padding:14px;font-size:16px;font-weight:600;border:none;border-radius:8px;"
  "background:#4da3ff;color:#04121f}"
  "</style></head><body>";
const char *kHtmlFoot = "</body></html>";

String scanListHtml() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED || n < 0) {
    WiFi.scanNetworks(/*async=*/true);
    return "<p>Scanning for networks&hellip; pull to refresh in a few seconds.</p>";
  }
  String html;
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue; // skip hidden networks in the list -- still reachable via "enter it manually"
    html += "<a class=net href='/pick?ssid=" + urlEncode(ssid) + "'>" + htmlEscape(ssid) +
            "<span>" + String(WiFi.RSSI(i)) + " dBm</span></a>";
  }
  if (html.length() == 0) html = "<p>No networks found nearby. Reload this page to scan again.</p>";
  return html;
}

void handleRoot() {
  String html = String(kHtmlHead);
  html += "<h1>Set up your Voltra Knob</h1><p>Pick your WiFi network below.</p>";
  html += scanListHtml();
  html += "<p style='margin-top:20px;font-size:13px;color:#777'>Don&rsquo;t see your network? "
          "<a href='/pick?ssid=' style='color:#4da3ff'>Enter it manually</a></p>";
  html += kHtmlFoot;
  sServer.send(200, "text/html", html);
}

void handlePick() {
  String ssid = sServer.arg("ssid");
  String html = String(kHtmlHead);
  html += "<form method='POST' action='/save'>";
  if (ssid.length() > 0) {
    html += "<h1>" + htmlEscape(ssid) + "</h1><p>Enter the WiFi password, then tap Connect.</p>";
    html += "<input type='hidden' name='ssid' value='" + htmlEscape(ssid) + "'>";
    html += "<input type='password' name='pass' placeholder='WiFi password' autofocus>";
  } else {
    html += "<h1>Enter network manually</h1><p>For a hidden network, or one that didn't show up.</p>";
    html += "<input type='text' name='ssid' placeholder='Network name (SSID)' autofocus>";
    html += "<input type='password' name='pass' placeholder='WiFi password'>";
  }
  html += "<button type='submit'>Connect</button></form>";
  html += "<p style='margin-top:20px'><a href='/' style='color:#4da3ff'>&larr; back to network list</a></p>";
  html += kHtmlFoot;
  sServer.send(200, "text/html", html);
}

void stopPortal() {
  sServer.stop();
  sDns.stop();
  WiFi.softAPdisconnect(/*wifioff=*/true);
  sServerActive = false;
}

void handleSave() {
  String ssid = sServer.arg("ssid");
  String pass = sServer.arg("pass");
  if (ssid.length() == 0) {
    sServer.send(400, "text/plain", "A network name is required.");
    return;
  }
  saveCreds(ssid.c_str(), pass.c_str());
  String html = String(kHtmlHead);
  html += "<h1>Saved</h1><p>The knob is now trying to connect to \"" + htmlEscape(ssid) +
          "\". You can close this page and reconnect your phone to your normal WiFi -- "
          "check the knob's Settings > Wi-Fi screen to confirm it connected.</p>";
  html += kHtmlFoot;
  sServer.send(200, "text/html", html);
  delay(50); // let the response actually go out over the AP link before we tear it down
  stopPortal();
  beginStaConnect();
}

void handleNotFound() {
  // Any URL that isn't one of the routes above is treated as a captive-
  // portal probe (iOS/Android/Windows each ping a handful of well-known
  // URLs right after joining a new network specifically to detect this) --
  // redirecting them all to our own root page is the standard trick that
  // makes the phone auto-pop the setup page instead of the user having to
  // go find it themselves.
  sServer.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  sServer.send(302, "text/plain", "");
}

void registerRoutesOnce() {
  if (sRoutesRegistered) return;
  sServer.on("/", HTTP_GET, handleRoot);
  sServer.on("/pick", HTTP_GET, handlePick);
  sServer.on("/save", HTTP_POST, handleSave);
  sServer.onNotFound(handleNotFound);
  sRoutesRegistered = true;
}

} // namespace

void begin() {
  loadSaved();
  if (sHasSaved) {
    beginStaConnect();
  } else {
    WiFi.mode(WIFI_STA); // park in STA (not AP) with radio otherwise idle
    sState = State::Disconnected;
  }
}

void update() {
  switch (sState) {
    case State::Provisioning:
      if (sServerActive) sServer.handleClient();
      // sDns runs on AsyncUDP internally -- no per-loop pump needed.
      return;

    case State::Connecting:
      if (WiFi.status() == WL_CONNECTED) {
        sState = State::Connected;
        sFailCount = 0;
        Serial.printf("[wifiprov] connected, ip=%s\n", WiFi.localIP().toString().c_str());
        return;
      }
      {
        uint32_t interval = (sFailCount < kFastRetryAttempts) ? kFastRetryMs : kSlowRetryMs;
        if (millis() - sAttemptStartMs > interval) {
          sFailCount++;
          WiFi.disconnect();
          WiFi.begin(sSavedSsid, sSavedPass);
          sAttemptStartMs = millis();
        }
      }
      return;

    case State::Connected:
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[wifiprov] WiFi dropped -- retrying in the background");
        sAttemptStartMs = millis();
        sFailCount = 0; // was fine a moment ago -- retry fast again, don't inherit an old backoff
        sState = State::Connecting;
      }
      return;

    case State::Disconnected:
    default:
      return;
  }
}

Status status() {
  Status s;
  s.state = sState;
  s.hasSaved = sHasSaved;
  strncpy(s.ssid, sSavedSsid, sizeof(s.ssid) - 1);
  strncpy(s.apName, sApName, sizeof(s.apName) - 1);
  if (sState == State::Connected) {
    strncpy(s.ip, WiFi.localIP().toString().c_str(), sizeof(s.ip) - 1);
  }
  return s;
}

void startProvisioning() {
  if (sState == State::Provisioning) return;

  // AP_STA (not just AP) so a network scan can still run concurrently with
  // the AP being up -- WiFi.scanNetworks() needs the STA interface active.
  WiFi.mode(WIFI_AP_STA);
  uint8_t mac[6];
  WiFi.macAddress(mac); // needs STA mode set first for a valid MAC, same as main.cpp's OTA hostname
  snprintf(sApName, sizeof(sApName), "VoltraKnob-%02X%02X%02X", mac[3], mac[4], mac[5]);

  WiFi.softAP(sApName); // open network on purpose -- see the captive-portal comment above
  sDns.start();         // defaults: port 53, catch-all domain, resolves to the AP's own IP
  registerRoutesOnce();
  sServer.begin();
  sServerActive = true;
  WiFi.scanNetworks(/*async=*/true); // kick off a scan immediately so the list is ready fast

  sState = State::Provisioning;
  Serial.printf("[wifiprov] provisioning started, AP=\"%s\" ip=%s\n", sApName,
                WiFi.softAPIP().toString().c_str());
}

void cancelProvisioning() {
  if (sState != State::Provisioning) return;
  stopPortal();
  if (sHasSaved) {
    beginStaConnect();
  } else {
    WiFi.mode(WIFI_STA);
    sState = State::Disconnected;
  }
}

void forgetNetwork() {
  if (sState == State::Provisioning) stopPortal();
  WiFi.disconnect(/*wifioff=*/true);
  clearSaved();
  sState = State::Disconnected;
}

} // namespace wifiprov
