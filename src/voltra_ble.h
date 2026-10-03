#pragma once
#include <Arduino.h>
#include <cstdint>

class NimBLEAddress; // forward-declared so this header doesn't need to pull in NimBLE's own headers

namespace voltra {

enum class ConnState {
  Idle,
  Scanning,
  Connecting,
  Handshaking,
  Ready,
};

// Chains, Inverse Chains, and Mountain are one shared weight amount
// (VoltraState::chainsWeightLb) plus a style selector -- see
// voltra_protocol.h's PARAM_FITNESS_INVERSE_CHAIN/PARAM_MOUNTAIN_CURVE
// comment and VoltraClient::setChainsStyle() for the full explanation.
// Chains is the "neither flag set" style, not a separate wire value.
enum class ChainsStyle {
  Chains,
  Inverse,
  Mountain,
};

// See voltra_protocol.h's kCmdTwinLink comment for the overall approach.
// Only two states: unlike ConnState, there's no "in progress" state exposed
// here -- linking is a single blocking call (VoltraClient::twinWithCandidate())
// rather than something update() advances step by step, so state only ever
// changes at the moment that call returns.
enum class TwinState {
  None,
  Twinned,
};

// A nearby Voltra found while looking for a twin follower (see
// VoltraClient::startTwinDiscovery()). addressStr is display-ready
// ("aa:bb:cc:dd:ee:ff", NimBLEAddress::toString()'s own format); addr holds
// the same 6 bytes in that same printed order, ready to drop straight into
// a kCmdTwinLink payload -- see that constant's comment for why printed
// order, not a BLE stack's raw internal order.
struct TwinCandidate {
  char addressStr[18];
  uint8_t addr[6];
};
constexpr uint8_t kMaxTwinCandidates = 4;

struct VoltraState {
  ConnState connState = ConnState::Idle;
  bool everConnected = false;

  bool haveBaseWeight = false;
  int baseWeightLb = 45;

  bool haveEccentricWeight = false;
  int eccentricWeightLb = 0;

  // "Inverse chains" -- PARAM_BP_CHAINS_WEIGHT (0x3E87) in voltra_protocol.h,
  // right alongside base/eccentric weight in the Voltra's own parameter
  // table, but unlike those two, never independently verified against a
  // real captured frame or reference implementation -- see setChainsWeightLb()
  // in voltra_ble.cpp for what that means in practice.
  bool haveChainsWeight = false;
  int chainsWeightLb = 0;

  // Which of the three mutually-exclusive styles chainsWeightLb currently
  // produces. Like haveChainsWeight/chainsWeightLb above, this is tracked
  // optimistically from what we last wrote (see setChainsStyle()), not
  // confirmed by a device notification -- the device never pushes either
  // style flag back unsolicited.
  ChainsStyle chainsStyle = ChainsStyle::Chains;

  // Twin mode (see voltra_protocol.h's kCmdTwinLink comment).
  // twinFollowerAddrStr is only non-empty while twinState == Twinned.
  // Preserved across a disconnect/reconnect cycle by onDisconnect() -- see
  // its own comment -- unlike almost everything else in this struct, which
  // resets to defaults on every disconnect.
  TwinState twinState = TwinState::None;
  char twinFollowerAddrStr[18] = "";
  // Confirmed by periodically polling the host with kCmdTwinStatus (0xA7)
  // -- unlike twinState above, which flips the instant twinWithCandidate()/
  // untwin() is CALLED, before either command is known to have taken
  // effect, this reflects what the host itself most recently reported.
  // false right after tapping TWIN WITH THIS (or after any reconnect,
  // since a fresh connection hasn't polled yet) until the next status
  // frame confirms it -- see VoltraClient::handleFrame()'s CMD_TWIN_STATUS
  // handling, which also uses this same poll to correct twinState itself
  // if the host ever disagrees (a spontaneous untwin, or a host that was
  // left twinned from before a reboot).
  bool twinConfirmedByHost = false;
  // The twinned peer's own battery %, as last reported by the host's twin
  // status push -- diagnostic only (logged, not yet surfaced in the UI);
  // -1 = unknown.
  int twinHostPeerBatteryPct = -1;

  // Set when the host reports it's already twinned with someone, but this
  // knob didn't initiate it (twinWithCandidate() was never called on this
  // knob -- the pairing came from the other unit's own knob, the
  // voltra-pedal project, or the Beyond+ app). Unlike twinState, this by
  // itself changes nothing about engage()/disengage()'s wire behavior --
  // see VoltraClient::acceptPendingTwin(). Deliberately opt-in: the
  // voltra-pedal project (no screen to ask on) auto-joins on its own, but
  // the knob has a screen, and may be running its own, separate exercise
  // on this Voltra -- it should never silently start sending twin commands
  // just because the Voltra it's connected to happens to report a twin
  // link.
  bool twinPendingAcceptance = false;
  char twinPendingPeerAddrStr[18] = "";

  int fitnessMode = -1; // raw PARAM_BP_SET_FITNESS_MODE value, see voltra_protocol.h
  bool loaded = false;  // fully engaged (plain load, or auto-load that has reached kGuidedStateLoaded)
  bool armed = false;   // auto-load armed and waiting (fitnessMode == AUTOLOAD_ARMED, or DIRECT_LOAD not yet loaded)

  // Auto-load ("guided load") status -- only meaningful while armed or while
  // fitnessMode == FITNESS_MODE_DIRECT_LOAD. See voltra_protocol.h for the
  // kGuidedState* values.
  int guidedState = -1;      // register 0x53C7
  int guidedCountdownMs = -1; // register 0x53C8, valid only while guidedState == kGuidedStateCountdown

  int reps = 0; // rep count in the current set; reset to 0 whenever a fresh load starts

  int batteryPct = -1; // 0..100, -1 = unknown. Pushed unsolicited by the device; no read command for it.

  uint32_t lastNotifyMs = 0;
  uint32_t lastStateChangeMs = 0;

  // millis() timestamp of the most recent connState transition -- lets the
  // UI show "how long has this been going on" (e.g. "SCANNING 12s") while
  // reconnecting, so a slow reconnect reads as "still working" rather than
  // looking frozen.
  uint32_t stateEnteredMs = 0;
  // millis() timestamp of the last time we left ConnState::Ready (or 0 if
  // we've never been Ready since boot). Unlike stateEnteredMs, this
  // doesn't reset on every Idle<->Scanning cycle during a long reconnect
  // attempt, so it can answer "how long has the Voltra actually been
  // unreachable" rather than just "how long in the current sub-state".
  uint32_t disconnectedSinceMs = 0;
};

class VoltraClient {
public:
  // Call once from setup().
  void begin();

  // Call every loop() iteration. Non-blocking except for the brief
  // (few-second) active scan it kicks off periodically while disconnected.
  void update();

  const VoltraState &state() const { return _state; }
  bool isReady() const { return _state.connState == ConnState::Ready; }

  // Fire-and-forget commands; safe to call even if not connected (they are
  // ignored with a log line in that case). All values are already in the
  // final "pounds" units the Voltra itself expects -- no unit conversion.
  void setBaseWeightLb(int lb);
  void setEccentricWeightLb(int lb);
  void setChainsWeightLb(int lb);
  void setChainsStyle(ChainsStyle style); // picks which of Chains/Inverse/Mountain the chains amount above produces
  void engage();      // load now (standard strength mode): SETUP then GO, matching the reference sequence
  void disengage();   // stop/release load, or cancel an armed/counting-down auto-load
  void armAutoLoad(); // arm auto-load: device engages on the first pull after its own countdown

  // Forces a disconnect (if currently connected/connecting) and an
  // immediate rescan, instead of waiting out whatever backoff delay
  // update() would otherwise be sitting in. For Settings > Bluetooth's
  // "RECONNECT" button -- there was no existing way to nudge a stuck/slow
  // reconnect from the UI before this; disconnects previously only
  // happened via internal error paths or the peer disconnecting first.
  void disconnectAndRescan();

  // Twin mode -- see voltra_protocol.h's kCmdTwinLink comment for the
  // overall approach and its caveats. All meant to be driven from the
  // Settings > Twin screen (ui.cpp), only while isReady().

  // Starts a short (kTwinScanDurationMs), non-blocking scan for other
  // nearby Voltras to twin with -- same non-blocking pattern as the
  // reconnect scan startScan() below runs, just collecting up to
  // kMaxTwinCandidates matches instead of stopping at the first one, and
  // skipping whichever address we're already connected to. Poll
  // twinDiscoveryInProgress()/twinCandidateCount()/twinCandidate() from the
  // UI same as ConnState is polled elsewhere -- there's no callback here.
  void startTwinDiscovery();
  bool twinDiscoveryInProgress() const;
  uint8_t twinCandidateCount() const;
  const TwinCandidate &twinCandidate(uint8_t index) const;

  // Opens a brief separate connection to the given candidate and sends it
  // the twin-link command (see linkFollower() in voltra_ble.cpp for the
  // exact sequence). BLOCKING for a few seconds -- the same order of
  // magnitude the primary connection's own auth handshake already blocks
  // update() for elsewhere in this file -- so only call this from a UI
  // action the user just tapped, never from update() itself. Returns
  // whether the follower accepted the connection and the command was
  // written; there is no confirmed-twinned reply parsed back (see
  // linkFollower()'s comment), so a `true` return means "sent", not
  // "confirmed twinned" -- verify on the Voltras' own screens if unsure.
  bool twinWithCandidate(uint8_t index);

  // Sends the un-twin command to the host we're currently connected to.
  // Fire-and-forget, same optimistic-state-update pattern setChainsStyle()
  // above already uses -- no confirmed reply parsed for this either.
  void untwin();

  // Promotes a pending twin (see VoltraState::twinPendingAcceptance) to
  // actually twinned -- from this point on engage()/disengage() use the
  // twin fitness-mode values and update()'s Idle case will reconnect by
  // address if the connection drops. Does nothing if there's no pending
  // twin. For Settings > Twin's "ENTER TWIN MODE" button.
  void acceptPendingTwin();

private:
  VoltraState _state;
  uint32_t _lastScanAttemptMs = 0;
  uint32_t _reconnectDelayMs = 2000; // mirrors kConnectRetryBaseDelayMs
  uint32_t _lastArmPollMs = 0;       // last time we polled guided-load status while armed/engaging
  uint16_t _seq = 100;               // runtime sequence counter for frames we build ourselves
  bool _useFallbackAuth = false;     // switch to the SDK "iPhone" identity if AUTH_KNOB is ever rejected
  bool _handshakeEvent = false;      // set once a Settings/ModeConfirmed notification arrives post-handshake

  // Twin mode -- persists across a disconnect/reconnect (unlike _state,
  // which onDisconnect() resets wholesale; see its own comment for how
  // these get mirrored back into _state.twinState/twinFollowerAddrStr right
  // after that reset). _hostAddr is this connection's own address, recorded
  // once the handshake succeeds (see connectToAddress()) so a reconnect
  // while twinned -- the host stops advertising once twinned, see
  // voltra_protocol.h's kCmdTwinLink comment -- can connect by address
  // directly instead of scanning, which would never find it again.
  TwinState _twinState = TwinState::None;
  uint8_t _twinFollowerAddr[6] = {0};
  char _twinFollowerAddrStr[18] = "";
  // _hostAddr is in kCmdTwinLink's printed byte order (for the link
  // payload); _hostAddrStr is the same address as a string, kept
  // separately so reconnecting (NimBLEAddress's string constructor, same
  // round-trip addressFromDisplayString() elsewhere in voltra_ble.cpp uses)
  // never has to assume _hostAddr's printed order matches whatever raw
  // internal byte order NimBLEAddress's own (bytes, type) constructor
  // expects -- those are not necessarily the same thing.
  uint8_t _hostAddr[6] = {0};
  char _hostAddrStr[18] = "";
  bool _haveHostAddr = false;
  uint32_t _lastTwinHeartbeatMs = 0;
  uint32_t _lastTwinStatusPollMs = 0; // last time we sent a kCmdTwinStatus read
  // millis() timestamp of the last successful twinWithCandidate() -- see
  // voltra_protocol.h's kTwinJoinSettleMs comment; guards applyTwinStatus()
  // against clearing a just-sent join before the real pairing has had time
  // to complete. Not touched by acceptPendingTwin(), which reflects a
  // pairing the host already confirmed, not one we're still waiting on.
  uint32_t _twinJoinSentMs = 0;
  // Raw address bytes for whatever twin acceptPendingTwin() would promote
  // right now (printed order, same convention as _twinFollowerAddr) --
  // kept alongside VoltraState::twinPendingPeerAddrStr (the display string)
  // so acceptPendingTwin() doesn't have to re-derive them from it.
  uint8_t _twinPendingPeerAddr[6] = {0};

  void *_client = nullptr; // NimBLEClient*, opaque here to keep this header light

  // Starts (or is mid-way through) a non-blocking BLE scan -- see
  // voltra_ble.cpp's ScanCallbacks for how "found the Voltra" gets back to
  // update() without blocking loop() for the scan's whole duration.
  void startScan();
  // Connects to a device address handed back by the scan callback once
  // it's matched the Voltra's advertised service UUID.
  void connectToAddress(const NimBLEAddress &address);
  bool runHandshake();
  uint16_t nextSeq();
  // The actual brief-second-connection sequence behind twinWithCandidate()
  // above: connect to the follower, auth, send kCmdTwinLink, disconnect.
  // Entirely self-contained -- does not touch _client/the primary
  // connection's globals in voltra_ble.cpp at all, so the host connection
  // this runs alongside is completely undisturbed by it.
  bool linkFollower(const uint8_t (&followerAddr)[6], const char *followerAddrStr);
  // Sets connState and stamps stateEnteredMs in the same place, so every
  // transition updates both together -- see VoltraState::stateEnteredMs.
  void setConnState(ConnState s);

public:
  // Public only so the NimBLE notify-callback lambda (registered in
  // voltra_ble.cpp) can reach it; not meant to be called from UI code.
  void onNotification(const uint8_t *data, size_t len);
  void onDisconnect();

private:
  void handleFrame(const uint8_t *frame, size_t len);
  void writeCommand(const uint8_t *data, size_t len);
};

extern VoltraClient gVoltra;

} // namespace voltra
