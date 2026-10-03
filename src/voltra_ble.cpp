// handleFrame() below (response/notification parsing) is ported from
// github.com/omarshahine/voltra-knob (MIT License, Copyright (c) 2026 Omar
// Shahine) -- see THIRD_PARTY.md and voltra_protocol.h's own banner comment
// for the full attribution and verification story.
#include "voltra_ble.h"
#include "voltra_protocol.h"
#include "crc.h"
#include <NimBLEDevice.h>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// Avoid any ambiguity with Arduino's own min()/max() (which on some cores
// are macros, not templates, and don't play well with explicit template
// args or mixed types) -- just write the two comparisons we need by hand.
static inline uint32_t minU32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static inline int maxInt(int a, int b) { return a > b ? a : b; }
static inline int minInt(int a, int b) { return a < b ? a : b; }

namespace voltra {

VoltraClient gVoltra;

// Single reassembly buffer for the one notify characteristic we subscribe
// to. Frames can in principle arrive split across multiple BLE
// notifications; we request a large MTU so this is rare in practice, but we
// still reassemble defensively.
static std::vector<uint8_t> sRxBuf;

static NimBLERemoteService *sService = nullptr;
static NimBLERemoteCharacteristic *sWriteChar = nullptr;

class ClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient *pClient, int reason) override {
    Serial.printf("[voltra] disconnected (reason=%d)\n", reason);
    gVoltra.onDisconnect();
  }
};
static ClientCallbacks sClientCallbacks;

// How long a single scan attempt runs before giving up and backing off, if
// nothing is found. This used to be a hard 4-second BLOCKING wait
// (NimBLEScan::getResults(4000, false)) -- during which loop() never ran,
// so touch/encoder input was completely frozen for 4 out of every several
// seconds the whole time the Voltra was unreachable. Scanning is now
// driven by NimBLEScanCallbacks instead (see ScanCallbacks below), so it
// no longer blocks loop() at all; this can afford to be longer than the
// old hard-coded 4s since a long scan no longer costs UI responsiveness,
// only how quickly we back off when the Voltra really isn't there.
static constexpr uint32_t kScanDurationMs = 8000;

// Set from BLE-host-task context (NimBLEScanCallbacks callbacks run on
// NimBLE's own FreeRTOS task, not the Arduino loop() task) and read back
// from VoltraClient::update() on the next loop() iteration. This mirrors
// how onNotification()/onDisconnect() already touch _state directly from
// that same BLE task elsewhere in this file -- there's no lock around any
// of it, consistent with the rest of this codebase's concurrency model.
static volatile bool sScanFoundFlag = false;
static volatile bool sScanActive = false;
static NimBLEAddress sScanFoundAddress;

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *device) override {
    if (sScanFoundFlag) return; // already latched a match this scan; ignore the rest
    if (device->isAdvertisingService(NimBLEUUID(kServiceUUID))) {
      sScanFoundAddress = device->getAddress();
      sScanFoundFlag = true;
      // Stop the scan immediately rather than waiting out kScanDurationMs
      // -- the Voltra usually advertises every 20-100ms, so this is what
      // makes reconnects fast instead of only non-blocking.
      NimBLEDevice::getScan()->stop();
    }
  }
  void onScanEnd(const NimBLEScanResults &, int) override {
    sScanActive = false;
  }
};
static ScanCallbacks sScanCallbacks;

// ---------------------------------------------------------------------------
// Twin mode discovery -- a separate, independent scan from ScanCallbacks
// above (different NimBLEScanCallbacks instance, different static state):
// that one is the always-running "find THE Voltra to reconnect to" scan
// driven from ConnState::Idle, stopping at the first match; this one only
// ever runs on demand (Settings > Twin's "FIND VOLTRA" button, while
// already Ready/connected to a host) and collects several matches, skipping
// whichever address is already our own host. The two never run at once --
// see startTwinDiscovery()'s comment -- but keeping their state fully
// separate means neither has to know the other exists.

// Converts a NimBLEAddress to the printed "aa:bb:cc:dd:ee:ff" byte order --
// see voltra_protocol.h's kCmdTwinLink comment for why THIS order, not
// NimBLEAddress's own raw internal byte order, is what goes on the wire and
// what TwinCandidate::addr stores.
static void addressToDisplayBytes(const NimBLEAddress &address, uint8_t out[6], char *strOut, size_t strOutLen) {
  std::string s = std::string(address);
  snprintf(strOut, strOutLen, "%s", s.c_str());
  for (int i = 0; i < 6; i++) {
    out[i] = (uint8_t)strtoul(s.substr((size_t)i * 3, 2).c_str(), nullptr, 16);
  }
}

// Reconstructs a connectable NimBLEAddress from the same printed string
// addressToDisplayBytes() produced -- round-trips correctly regardless of
// whatever byte order NimBLEAddress uses internally, since both directions
// go through its own string parsing/formatting rather than raw bytes.
static NimBLEAddress addressFromDisplayString(const char *s) {
  return NimBLEAddress(std::string(s), BLE_ADDR_PUBLIC);
}

static volatile bool sTwinScanActive = false;
static TwinCandidate sTwinCandidates[kMaxTwinCandidates];
static volatile uint8_t sTwinCandidateCount = 0;
static uint8_t sTwinScanExcludeAddr[6]; // our own host's address -- see startTwinDiscovery()

class TwinScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *device) override {
    if (sTwinCandidateCount >= kMaxTwinCandidates) return;
    if (!device->isAdvertisingService(NimBLEUUID(kServiceUUID))) return;
    uint8_t addr[6];
    char str[18];
    addressToDisplayBytes(device->getAddress(), addr, str, sizeof(str));
    if (memcmp(addr, sTwinScanExcludeAddr, 6) == 0) return; // that's our own host, skip
    for (uint8_t i = 0; i < sTwinCandidateCount; i++) {
      if (memcmp(sTwinCandidates[i].addr, addr, 6) == 0) return; // duplicate advertisement, already have it
    }
    memcpy(sTwinCandidates[sTwinCandidateCount].addr, addr, 6);
    snprintf(sTwinCandidates[sTwinCandidateCount].addressStr, sizeof(sTwinCandidates[sTwinCandidateCount].addressStr), "%s", str);
    sTwinCandidateCount = (uint8_t)(sTwinCandidateCount + 1); // avoid the volatile ++ deprecation warning
  }
  void onScanEnd(const NimBLEScanResults &, int) override {
    sTwinScanActive = false;
  }
};
static TwinScanCallbacks sTwinScanCallbacks;

// Reply state for linkFollower()'s brief second connection -- set from the
// follower's own notify callback (BLE-task context, same pattern as
// sScanFoundFlag above) and read back from linkFollower()'s blocking wait
// loop on the very next check.
static volatile bool sFollowerGotReply = false;
static volatile uint8_t sFollowerReplyByte = 0xFF;

void VoltraClient::begin() {
  NimBLEDevice::init("VoltraKnob");
  NimBLEDevice::setMTU(247);
  NimBLEDevice::getScan()->setScanCallbacks(&sScanCallbacks, false);
  Serial.println("[voltra] BLE init done, running CRC self-test...");
  bool ok = selfTestAgainstCapturedFrames();
  Serial.println(ok ? "[voltra] CRC self-test PASSED (all reference frames)"
                     : "[voltra] CRC self-test FAILED -- protocol frames will be rejected by the device");
}

uint16_t VoltraClient::nextSeq() {
  uint16_t s = _seq;
  _seq = (uint16_t)(_seq + 1);
  return s;
}

void VoltraClient::writeCommand(const uint8_t *data, size_t len) {
  if (!sWriteChar || !sWriteChar->canWrite()) {
    Serial.println("[voltra] writeCommand: not connected, dropping");
    return;
  }
  sWriteChar->writeValue(data, len, sWriteChar->canWrite());
}

void VoltraClient::setConnState(ConnState s) {
  _state.connState = s;
  _state.stateEnteredMs = millis();
}

void VoltraClient::update() {
  switch (_state.connState) {
    case ConnState::Idle: {
      uint32_t now = millis();
      if (now - _lastScanAttemptMs < _reconnectDelayMs) return;
      _lastScanAttemptMs = now;
      if (_twinState == TwinState::Twinned && _haveHostAddr) {
        // The host stops advertising once twinned (see voltra_protocol.h's
        // kCmdTwinLink comment, ported from stevenpoint/voltra_remote) -- a
        // normal scan-and-match-by-service-UUID reconnect (startScan()
        // below) will never find it again, so go straight to a direct
        // connect by the address we already know instead. The reference
        // project's own docs note this doesn't always succeed quickly ("can
        // take a minute or so"), so this can retry for a while the same way
        // a normal scan-based reconnect would, just via connectToAddress()
        // instead of startScan().
        connectToAddress(addressFromDisplayString(_hostAddrStr));
      } else {
        startScan();
      }
      break;
    }
    case ConnState::Scanning: {
      // Non-blocking: ScanCallbacks (BLE-host-task context, above) sets
      // sScanFoundFlag the instant it sees the Voltra's service UUID, or
      // clears sScanActive once the scan ends with nothing found. Neither
      // branch here blocks -- touch/encoder input keeps working the whole
      // time a scan is in progress, unlike the old fixed 4s blocking wait.
      if (sScanFoundFlag) {
        NimBLEAddress address = sScanFoundAddress;
        sScanFoundFlag = false;
        connectToAddress(address);
      } else if (!sScanActive) {
        Serial.println("[voltra] no VOLTRA advertisement seen this scan");
        setConnState(ConnState::Idle);
        _reconnectDelayMs = minU32((uint32_t)(_reconnectDelayMs * 1.5), kConnectRetryMaxDelayMs);
      }
      break;
    }
    case ConnState::Connecting:
    case ConnState::Handshaking:
      // Nothing polled here; notifications drive state forward.
      break;
    case ConnState::Ready: {
      // While an auto-load is armed/counting down/engaging, poll its status
      // registers so the UI follows the device's own timeline (it does not
      // push these unsolicited) -- mirrors the reference implementation's
      // servicePump() poll loop.
      if (_state.fitnessMode == (int)FITNESS_MODE_DIRECT_LOAD) {
        uint32_t now = millis();
        if (now - _lastArmPollMs >= kArmPollMs) {
          _lastArmPollMs = now;
          writeCommand(kReadGuidedLoadStatus, sizeof(kReadGuidedLoadStatus));
        }
      }
      // Twin heartbeat -- see voltra_protocol.h's kTwinHeartbeatPayload
      // comment for why this is sent unconditionally while loaded+twinned
      // despite its purpose not being documented anywhere.
      if (_state.loaded && _twinState == TwinState::Twinned) {
        uint32_t now = millis();
        if (now - _lastTwinHeartbeatMs >= kTwinHeartbeatMs) {
          _lastTwinHeartbeatMs = now;
          auto f = buildCommandFrame(kCmdVendorTrigger, kTwinHeartbeatPayload, sizeof(kTwinHeartbeatPayload), nextSeq());
          writeCommand(f.data(), f.size());
        }
      }
      // Twin status poll -- confirms (or corrects) twinState from the host
      // itself; see voltra_protocol.h's kCmdTwinStatus comment. Runs
      // whether or not we currently think we're twinned (mirrors the
      // voltra-pedal project's own always-on poll), so a spontaneous
      // untwin or a stale twinState left over from before a reboot both
      // get caught by handleFrame()'s CMD_TWIN_STATUS handling, instead of
      // twinState only ever being set optimistically by
      // twinWithCandidate()/untwin() and never re-checked.
      {
        uint32_t now = millis();
        if (now - _lastTwinStatusPollMs >= kTwinStatusPollMs) {
          _lastTwinStatusPollMs = now;
          auto f = buildCommandFrame(kCmdTwinStatus, nullptr, 0, nextSeq());
          writeCommand(f.data(), f.size());
        }
      }
      break;
    }
  }
}

void VoltraClient::startScan() {
  Serial.println("[voltra] scanning for VOLTRA...");
  setConnState(ConnState::Scanning);
  sScanFoundFlag = false;

  NimBLEScan *pScan = NimBLEDevice::getScan();
  // Re-assert our own callbacks -- startTwinDiscovery() below points this
  // same shared NimBLEScan at sTwinScanCallbacks instead while it runs, and
  // this is otherwise only set once, in begin().
  pScan->setScanCallbacks(&sScanCallbacks, false);
  pScan->setActiveScan(true);
  pScan->setInterval(45);
  pScan->setWindow(15);
  sScanActive = true;
  pScan->start(kScanDurationMs, false);
}

void VoltraClient::connectToAddress(const NimBLEAddress &address) {
  Serial.printf("[voltra] found VOLTRA at %s, connecting...\n", std::string(address).c_str());
  setConnState(ConnState::Connecting);

  NimBLEClient *pClient = NimBLEDevice::createClient();
  pClient->setClientCallbacks(&sClientCallbacks, false);
  _client = pClient;

  if (!pClient->connect(address)) {
    Serial.println("[voltra] connect() failed");
    NimBLEDevice::deleteClient(pClient);
    _client = nullptr;
    setConnState(ConnState::Idle);
    return;
  }

  sService = pClient->getService(NimBLEUUID(kServiceUUID));
  if (!sService) {
    Serial.println("[voltra] service not found after connect!");
    pClient->disconnect();
    setConnState(ConnState::Idle);
    return;
  }

  sWriteChar = sService->getCharacteristic(NimBLEUUID(kWriteCharUUID));
  NimBLERemoteCharacteristic *notifyChar = sService->getCharacteristic(NimBLEUUID(kNotifyCharUUID));
  if (!sWriteChar || !notifyChar) {
    Serial.println("[voltra] characteristics missing");
    pClient->disconnect();
    setConnState(ConnState::Idle);
    return;
  }
  if (!notifyChar->subscribe(true, [](NimBLERemoteCharacteristic *, uint8_t *data, size_t length, bool) {
        gVoltra.onNotification(data, length);
      })) {
    Serial.println("[voltra] subscribe failed");
    pClient->disconnect();
    setConnState(ConnState::Idle);
    return;
  }

  sRxBuf.clear();

  Serial.println("[voltra] connected, running handshake...");
  setConnState(ConnState::Handshaking);
  bool ok = runHandshake();
  if (!ok) {
    Serial.println("[voltra] handshake failed/timed out");
    pClient->disconnect();
    setConnState(ConnState::Idle);
    if (!_useFallbackAuth) {
      _useFallbackAuth = true;
      Serial.println("[voltra] will retry with the SDK 'iPhone' identity next attempt");
    }
    return;
  }
  _state.everConnected = true;
  _reconnectDelayMs = kConnectRetryBaseDelayMs;
  addressToDisplayBytes(address, _hostAddr, _hostAddrStr, sizeof(_hostAddrStr));
  _haveHostAddr = true;
}

bool VoltraClient::runHandshake() {
  // This exact sequence -- custom identity, then two literal init frames,
  // then select weight-training mode -- is what a real, working
  // implementation uses (github.com/omarshahine/voltra-knob), verified
  // against this project's own CRC before trusting it (tools/verify_crc.py).
  _handshakeEvent = false;

  const uint8_t *authBytes = _useFallbackAuth ? kAuthIphoneFallback : kAuthKnob;
  size_t authLen = _useFallbackAuth ? sizeof(kAuthIphoneFallback) : sizeof(kAuthKnob);
  Serial.printf("[voltra]   -> auth as %s\n", _useFallbackAuth ? "iPhone (fallback)" : "Voltra Knob");
  writeCommand(authBytes, authLen);
  delay(kAuthWaitMs);

  for (size_t i = 0; i < kInitCount; i++) {
    LiteralFrame f = initFrame(i);
    Serial.printf("[voltra]   -> %s (%u bytes)\n", f.label, (unsigned)f.len);
    writeCommand(f.bytes, f.len);
    delay(kInitGapMs);
  }

  Serial.println("[voltra]   -> MODE_WEIGHT_TRAINING");
  writeCommand(kModeWeightTraining, sizeof(kModeWeightTraining));

  uint32_t deadline = millis() + kHandshakeDeadlineMs;
  while (millis() < deadline && !_handshakeEvent) delay(50);
  if (!_handshakeEvent) return false;

  Serial.println("[voltra]   -> TELEMETRY_SUBSCRIBE_ALL");
  writeCommand(kTelemetrySubscribeAll, sizeof(kTelemetrySubscribeAll));
  delay(kInitGapMs);
  Serial.println("[voltra]   -> READ_SETTINGS");
  writeCommand(kReadSettings, sizeof(kReadSettings));

  setConnState(ConnState::Ready);
  Serial.println("[voltra] ready");
  return true;
}

void VoltraClient::onDisconnect() {
  _state = VoltraState{};
  _state.disconnectedSinceMs = millis();
  // _twinState/_twinFollowerAddrStr (private, set by twinWithCandidate()/
  // untwin()) are NOT reset here -- they need to survive this disconnect so
  // the reconnect-by-address branch in update()'s Idle case still knows to
  // use it, and so the UI doesn't flash "not twinned" during a reconnect
  // that's actually still twinned. Re-mirror them into the freshly-reset
  // _state right away so the UI sees the right thing on the very next draw.
  _state.twinState = _twinState;
  snprintf(_state.twinFollowerAddrStr, sizeof(_state.twinFollowerAddrStr), "%s", _twinFollowerAddrStr);
  setConnState(ConnState::Idle);
  sService = nullptr;
  sWriteChar = nullptr;
  if (_client) {
    NimBLEDevice::deleteClient(static_cast<NimBLEClient *>(_client));
    _client = nullptr;
  }
}

void VoltraClient::disconnectAndRescan() {
  if (_client) {
    // onDisconnect() (the ClientCallbacks override above) fires from
    // NimBLE's own disconnect event and does all the real state cleanup
    // (resets _state, tears down sService/sWriteChar, deletes the
    // NimBLEClient) -- same as any other disconnect, just requested by us
    // instead of the peer or an error path.
    static_cast<NimBLEClient *>(_client)->disconnect();
  } else {
    // Not currently holding a client (e.g. already Idle/Scanning) -- there's
    // nothing to disconnect, but still clear any backoff delay so the very
    // next update() tick starts scanning right away instead of waiting out
    // whatever _reconnectDelayMs was already counting down.
    _reconnectDelayMs = kConnectRetryBaseDelayMs;
    _lastScanAttemptMs = 0;
    setConnState(ConnState::Idle);
  }
}

void VoltraClient::startTwinDiscovery() {
  Serial.println("[voltra] twin: scanning for other nearby Voltras...");
  sTwinCandidateCount = 0;
  memcpy(sTwinScanExcludeAddr, _hostAddr, 6);

  NimBLEScan *pScan = NimBLEDevice::getScan();
  pScan->setScanCallbacks(&sTwinScanCallbacks, false);
  pScan->setActiveScan(true);
  pScan->setInterval(45);
  pScan->setWindow(15);
  sTwinScanActive = true;
  pScan->start(kTwinScanDurationMs, false);
}

bool VoltraClient::twinDiscoveryInProgress() const { return sTwinScanActive; }
uint8_t VoltraClient::twinCandidateCount() const { return sTwinCandidateCount; }
const TwinCandidate &VoltraClient::twinCandidate(uint8_t index) const { return sTwinCandidates[index]; }

bool VoltraClient::linkFollower(const uint8_t (&followerAddr)[6], const char *followerAddrStr) {
  // A brief, throwaway second connection -- a separate NimBLEClient that
  // never touches _client/sService/sWriteChar (the primary host
  // connection's own globals) at any point, so the host connection is
  // completely undisturbed by this running alongside it. See
  // voltra_protocol.h's kCmdTwinLink comment for where this sequence comes
  // from (stevenpoint/voltra_remote's watch206 branch -- "open a short
  // second connection to the follower and tell it to join this host").
  Serial.printf("[voltra] twin: connecting to follower %s...\n", followerAddrStr);
  NimBLEClient *pClient = NimBLEDevice::createClient();
  NimBLEAddress address = addressFromDisplayString(followerAddrStr);
  if (!pClient->connect(address)) {
    Serial.println("[voltra] twin: follower connect() failed");
    NimBLEDevice::deleteClient(pClient);
    return false;
  }

  NimBLERemoteService *svc = pClient->getService(NimBLEUUID(kServiceUUID));
  NimBLERemoteCharacteristic *writeChar = svc ? svc->getCharacteristic(NimBLEUUID(kWriteCharUUID)) : nullptr;
  NimBLERemoteCharacteristic *notifyChar = svc ? svc->getCharacteristic(NimBLEUUID(kNotifyCharUUID)) : nullptr;
  if (!writeChar || !notifyChar) {
    Serial.println("[voltra] twin: follower missing service/characteristic");
    pClient->disconnect();
    NimBLEDevice::deleteClient(pClient);
    return false;
  }

  sFollowerGotReply = false;
  sFollowerReplyByte = 0xFF;
  if (!notifyChar->subscribe(true, [](NimBLERemoteCharacteristic *, uint8_t *data, size_t length, bool) {
        // CMD_TWIN_LINK reply: cmd byte at [10], reply code at [11] (0x00 = accepted).
        if (length >= 14 && data[10] == kCmdTwinLink) {
          sFollowerReplyByte = data[11];
          sFollowerGotReply = true;
        }
      })) {
    Serial.println("[voltra] twin: follower subscribe failed");
    pClient->disconnect();
    NimBLEDevice::deleteClient(pClient);
    return false;
  }

  // Full auth + two-init sequence, same as runHandshake() uses for the
  // primary connection and the same sequence the voltra-pedal project's
  // own (confirmed working on real hardware) twin-join uses. A previous
  // version of this function sent auth alone and went straight to the
  // join command -- confirmed on real hardware that the follower never
  // actually acts on a join sent that way (every attempt reported "alone"
  // afterward, never "twinned"), almost certainly because the reference
  // protocol treats auth+init as one unit before it accepts anything else,
  // same as this project's own primary connection already requires.
  Serial.println("[voltra] twin:   -> auth");
  writeChar->writeValue(kAuthKnob, sizeof(kAuthKnob), true);
  delay(kAuthWaitMs);
  for (size_t i = 0; i < kInitCount; i++) {
    LiteralFrame f = initFrame(i);
    Serial.printf("[voltra] twin:   -> %s\n", f.label);
    writeChar->writeValue(f.bytes, f.len, true);
    delay(kInitGapMs);
  }
  delay(kTwinFollowerSettleMs);

  uint8_t payload[7];
  payload[0] = kTwinLinkSubcmd;
  memcpy(&payload[1], _hostAddr, 6);
  auto frame = buildCommandFrame(kCmdTwinLink, payload, sizeof(payload), nextSeq());
  Serial.printf("[voltra] twin:   -> link command (host=%s)\n", _hostAddrStr);
  writeChar->writeValue(frame.data(), frame.size(), true);

  // Wait for the follower's own reply (0x00 = accepted), or for it to hang
  // up on its own to go connect to the host -- both count as success, same
  // as the voltra-pedal project's own implementation of this exact step.
  // Only a timeout with the connection still open and nothing back counts
  // as failure -- unlike the previous version of this function, which
  // never checked either way and always reported success.
  uint32_t deadline = millis() + kTwinJoinReplyTimeoutMs;
  while (millis() < deadline && !sFollowerGotReply && pClient->isConnected()) delay(20);
  bool ok = (sFollowerGotReply && sFollowerReplyByte == 0x00) || (!sFollowerGotReply && !pClient->isConnected());
  if (sFollowerGotReply) {
    Serial.printf("[voltra] twin: follower replied 0x%02x (%s)\n", sFollowerReplyByte, ok ? "accepted" : "rejected");
  } else if (!pClient->isConnected()) {
    Serial.println("[voltra] twin: follower hung up on its own -- treating as accepted");
  } else {
    Serial.println("[voltra] twin: follower never replied and is still connected -- treating as failed");
  }

  if (pClient->isConnected()) pClient->disconnect();
  NimBLEDevice::deleteClient(pClient);
  return ok;
}

bool VoltraClient::twinWithCandidate(uint8_t index) {
  if (index >= sTwinCandidateCount) {
    Serial.println("[voltra] twin: candidate index out of range");
    return false;
  }
  if (!_haveHostAddr) {
    Serial.println("[voltra] twin: don't know our own host address yet, can't twin");
    return false;
  }
  const TwinCandidate &c = sTwinCandidates[index];
  bool ok = linkFollower(c.addr, c.addressStr);
  if (ok) {
    _twinState = TwinState::Twinned;
    memcpy(_twinFollowerAddr, c.addr, 6);
    snprintf(_twinFollowerAddrStr, sizeof(_twinFollowerAddrStr), "%s", c.addressStr);
    _state.twinState = TwinState::Twinned;
    snprintf(_state.twinFollowerAddrStr, sizeof(_state.twinFollowerAddrStr), "%s", c.addressStr);
    _state.twinConfirmedByHost = false; // not yet -- see the kCmdTwinStatus poll below
    _state.twinPendingAcceptance = false; // this is us initiating our own twin, not accepting someone else's
    _twinJoinSentMs = millis();         // see kTwinJoinSettleMs -- don't let an early "alone" poll undo this
    _lastTwinStatusPollMs = 0;          // poll right away instead of waiting up to kTwinStatusPollMs
    Serial.printf("[voltra] twin: link command sent to %s, waiting for the host to confirm\n", c.addressStr);
  }
  return ok;
}

void VoltraClient::untwin() {
  if (_twinState != TwinState::Twinned) return;
  // Require an active connection, same as every other command in this file
  // -- unlike those, though, this one also mutates local twin state below,
  // and that state is what update()'s Idle branch relies on to reconnect by
  // address instead of scanning (the host stops advertising once twinned).
  // Clearing it here without the host ever receiving the unlink command
  // would leave the host still genuinely twinned (still not advertising)
  // while we've locally forgotten that fact -- exactly the stuck-reconnect
  // scenario that branch exists to avoid. Silently doing nothing while
  // disconnected (matching every other command's not-connected handling)
  // is safer: the user can retry once reconnected.
  if (!isReady()) {
    Serial.println("[voltra] untwin: not connected, ignoring");
    return;
  }
  uint8_t payload[7];
  payload[0] = kTwinUnlinkSubcmd;
  memcpy(&payload[1], _twinFollowerAddr, 6);
  auto frame = buildCommandFrame(kCmdTwinLink, payload, sizeof(payload), nextSeq());
  Serial.println("[voltra] twin: sending un-twin command to host");
  writeCommand(frame.data(), frame.size());
  _twinState = TwinState::None;
  memset(_twinFollowerAddr, 0, sizeof(_twinFollowerAddr));
  _twinFollowerAddrStr[0] = '\0';
  _state.twinState = TwinState::None;
  _state.twinFollowerAddrStr[0] = '\0';
  _state.twinConfirmedByHost = false;
  _state.twinPendingAcceptance = false;
  _state.twinPendingPeerAddrStr[0] = '\0';
  _lastTwinStatusPollMs = 0; // poll right away to confirm the host actually went back to alone
}

// See VoltraState::twinPendingAcceptance's comment: promotes a twin the
// host reported but this knob didn't initiate into one it actively
// participates in -- from this call on, engage()/disengage() use the twin
// fitness-mode values and a dropped connection reconnects by address (see
// update()'s Idle case) instead of scanning.
void VoltraClient::acceptPendingTwin() {
  if (!_state.twinPendingAcceptance) return;
  _twinState = TwinState::Twinned;
  memcpy(_twinFollowerAddr, _twinPendingPeerAddr, 6);
  snprintf(_twinFollowerAddrStr, sizeof(_twinFollowerAddrStr), "%s", _state.twinPendingPeerAddrStr);
  _state.twinState = TwinState::Twinned;
  snprintf(_state.twinFollowerAddrStr, sizeof(_state.twinFollowerAddrStr), "%s", _state.twinPendingPeerAddrStr);
  _state.twinConfirmedByHost = true; // the host itself is what told us about this pairing
  _state.twinPendingAcceptance = false;
  Serial.printf("[voltra] twin: accepted -- now twinned with %s\n", _state.twinFollowerAddrStr);
}

void VoltraClient::onNotification(const uint8_t *data, size_t len) {
  _state.lastNotifyMs = millis();
  sRxBuf.insert(sRxBuf.end(), data, data + len);

  // Resync to the next 0x55 sync byte if we've drifted.
  while (!sRxBuf.empty() && sRxBuf[0] != kSyncByte) sRxBuf.erase(sRxBuf.begin());

  while (sRxBuf.size() >= 4) {
    size_t declaredLen = sRxBuf[1];
    if (declaredLen < 4 || declaredLen > 160) {
      sRxBuf.erase(sRxBuf.begin());
      while (!sRxBuf.empty() && sRxBuf[0] != kSyncByte) sRxBuf.erase(sRxBuf.begin());
      continue;
    }
    if (sRxBuf.size() < declaredLen) break; // wait for more data
    handleFrame(sRxBuf.data(), declaredLen);
    sRxBuf.erase(sRxBuf.begin(), sRxBuf.begin() + declaredLen);
  }
}

namespace {
inline int u16le(const uint8_t *d, size_t off) { return d[off] | (d[off + 1] << 8); }

// "Vendor command" frames (rep/set counters) all share this shape: a fixed
// header at [10..12] identifying which one it is, rather than a param id.
inline bool isVendorFrame(const uint8_t *d, size_t n, uint8_t a, uint8_t b) {
  return n >= 13 && d[10] == 0xaa && d[11] == a && d[12] == b;
}

// Register width table for the cmd-0x0F bulk read response, and the two
// "settings changed" push-notification shapes -- ported from the reference
// decoder (src/protocol/voltra_protocol.cpp in omarshahine/voltra-knob),
// trimmed to the registers this firmware actually tracks (weight/eccentric/
// fitness mode/workout state, plus the four guided-load status registers).
inline size_t cmd0fWidth(uint8_t hi, uint8_t lo) {
  if (lo == 0x3e && (hi == 0x86 || hi == 0x87 || hi == 0x88 || hi == 0x89)) return 2;
  if (lo == 0x4f && hi == 0xb0) return 1;
  if (lo == 0x53 && hi == 0xc8) return 2;                              // guided-load countdown ms
  if (lo == 0x53 && (hi == 0x8d || hi == 0xc7 || hi == 0xc9)) return 1; // guided-load safety/state/ctrl
  return 0; // unknown register width -- stop parsing this frame, rather than guess
}
inline bool isU16Param(uint8_t hi, uint8_t lo) {
  if (lo == 0x3e && (hi == 0x86 || hi == 0x87 || hi == 0x88 || hi == 0x89)) return true;
  if (lo == 0x53 && hi == 0xc8) return true; // guided-load countdown ms
  return false;
}
} // namespace

// Applies one decoded (hi, lo, value) register triple to _state. Shared by
// both the push-notification (cmd 0x10) and bulk-read (cmd 0x0f) frame
// shapes below, so the two can't drift out of sync with each other.
void VoltraClient::handleFrame(const uint8_t *frame, size_t len) {
  uint8_t gotCrc8 = crc8(frame, 3);
  uint16_t gotCrc16 = crc16(frame, len - 2);
  uint16_t expectedCrc16 = (uint16_t)frame[len - 2] | ((uint16_t)frame[len - 1] << 8);
  bool valid = (gotCrc8 == frame[3]) && (gotCrc16 == expectedCrc16);

  char hex[3 * 48 + 1] = {0};
  size_t n = len < 48 ? len : 48;
  for (size_t i = 0; i < n; i++) sprintf(hex + i * 3, "%02X ", frame[i]);
  Serial.printf("[voltra] rx %u bytes, crc=%s: %s\n", (unsigned)len, valid ? "ok" : "BAD", hex);
  if (!valid) return;

  _state.lastStateChangeMs = millis();

  // fm == the raw PARAM_BP_SET_FITNESS_MODE (0x3E89) value just received.
  // Ported from the reference implementation's recomputeLoadState(): while
  // fitnessMode == DIRECT_LOAD (auto-load), "loaded" vs. "armed" is decided
  // by the separate guidedState register (0x53C7), not by fitnessMode alone.
  auto applyFitnessMode = [this](int fm) {
    _state.fitnessMode = fm;
    bool wasLoaded = _state.loaded;
    if (fm == (int)FITNESS_MODE_STRENGTH_LOADED || fm == (int)FITNESS_MODE_TWIN_LOADED) {
      // FITNESS_MODE_TWIN_LOADED is what a twinned host reports back for
      // the same "loaded" state -- see voltra_protocol.h's kCmdTwinLink
      // comment. Without this branch, a reconnect while twinned+loaded
      // (READ_SETTINGS in runHandshake(), or any later notification) would
      // report an fm value none of these branches matched, leaving
      // _state.loaded stuck at onDisconnect()'s reset default of false even
      // though the host is actually still loaded.
      _state.loaded = true;
      _state.armed = false;
    } else if (fm == (int)FITNESS_MODE_DIRECT_LOAD) {
      bool engaged = _state.guidedState >= kGuidedStateLoaded;
      _state.loaded = engaged;
      _state.armed = !engaged;
      if (_state.guidedState != kGuidedStateCountdown) _state.guidedCountdownMs = -1;
    } else if (fm == (int)FITNESS_MODE_AUTOLOAD_ARMED) {
      _state.loaded = false;
      _state.armed = true;
    } else if (fm == (int)FITNESS_MODE_STRENGTH_READY || fm == (int)FITNESS_MODE_TWIN_READY || fm == 0) {
      _state.loaded = false;
      _state.armed = false;
      _state.guidedCountdownMs = -1;
      _state.guidedState = -1;
    }
    if (_state.loaded && !wasLoaded) _state.reps = 0;
  };

  // Applies the 0x53C7 guided-load state register, with the same
  // "stale backstep" guard the reference implementation uses: a status poll
  // that lands right after a per-rep frame already proved engagement (14)
  // must not be allowed to walk state back to countdown/engaging (12/13) --
  // only a fresh arm (11) may lower it.
  auto applyGuidedState = [this, applyFitnessMode](int gs) {
    if (gs == _state.guidedState) return;
    bool staleBackstep = _state.guidedState == kGuidedStateLoaded &&
                          (gs == kGuidedStateCountdown || gs == kGuidedStateEngaging);
    if (staleBackstep) return;
    _state.guidedState = gs;
    if (_state.fitnessMode == (int)FITNESS_MODE_DIRECT_LOAD) applyFitnessMode(_state.fitnessMode);
  };

  auto applyParam = [this, &applyFitnessMode, &applyGuidedState](uint8_t hi, uint8_t lo, int value) {
    if (hi == 0x86 && lo == 0x3e) { _state.baseWeightLb = (int16_t)value; _state.haveBaseWeight = true; }
    else if (hi == 0x87 && lo == 0x3e) { _state.chainsWeightLb = (int16_t)value; _state.haveChainsWeight = true; }
    else if (hi == 0x88 && lo == 0x3e) { _state.eccentricWeightLb = (int16_t)value; _state.haveEccentricWeight = true; }
    else if (hi == 0x89 && lo == 0x3e) applyFitnessMode(value);
    else if (hi == 0xc7 && lo == 0x53) applyGuidedState(value);
    else if (hi == 0xc8 && lo == 0x53) { if (_state.guidedState == kGuidedStateCountdown) _state.guidedCountdownMs = value; }
    // 0x538d (guidedSafety) and 0x53c9 (guidedCtrl) are read but not surfaced in the UI.
  };

  // Parses a CMD_TWIN_STATUS (0xA7) response/push and reconciles our own
  // twinState against it -- see voltra_protocol.h's kCmdTwinStatus comment.
  // Payload layout ported as-is from stevenpoint/voltra_remote's
  // watch206 branch via the companion voltra-pedal project's
  // src/protocol/twin_frames.cpp parseTwinStatus(), which reports this
  // confirmed working against real hardware; it's intricate and
  // undocumented upstream beyond that, so see that file's own comment if
  // this ever needs re-deriving from a captured frame. `p` is the payload
  // (11-byte header skipped); pn is its length (frame length minus the
  // 11-byte header and 2-byte CRC16). A leading 0x00 byte means this is a
  // reply to our own empty-payload read rather than an unsolicited push;
  // payload byte 0 is otherwise the HOST'S OWN battery %, which a push
  // with a battery reading of exactly 0 could be confused with -- pn==23
  // (the only shape a push carries at full length) disambiguates that one
  // edge case.
  auto applyTwinStatus = [this](const uint8_t *frame, size_t len) {
    const uint8_t *p = frame + 11;
    const size_t pn = len - 13;
    const size_t k = (pn >= 2 && p[0] == 0x00 && (p[1] != 0x01 || pn == 23)) ? 1 : 0;
    if (pn < k + 3 || p[k + 1] != 0x01) return;
    int hostStatus = p[k + 2];

    uint8_t peer[6] = {0};
    char peerAddrStr[18] = "";
    bool havePeer = false;
    if (pn >= k + 15) {
      memcpy(peer, p + k + 9, 6);
      for (uint8_t b : peer) havePeer |= (b != 0);
      if (havePeer) {
        snprintf(peerAddrStr, sizeof(peerAddrStr), "%02x:%02x:%02x:%02x:%02x:%02x",
                 peer[0], peer[1], peer[2], peer[3], peer[4], peer[5]);
      }
    }
    if (pn >= k + 16 && p[k + 15] > 0 && p[k + 15] <= 100) _state.twinHostPeerBatteryPct = p[k + 15];
    else if (hostStatus == kTwinStatusAlone) _state.twinHostPeerBatteryPct = -1;

    // Reconcile against what the host just reported, rather than only
    // ever trusting our own memory of having sent a join/unlink command --
    // but unlike the "alone" direction below, NEVER silently switch this
    // knob into twin behavior on our own account. If the host says
    // twinned and we already agree (we initiated it, or a previous poll
    // was already accepted), just keep confirming/refreshing it. If the
    // host says twinned and we DON'T already agree -- a fresh boot after
    // an earlier acceptance, a pairing formed by the other unit's own
    // knob, the voltra-pedal project, or the Beyond+ app -- surface it as
    // a choice (VoltraState::twinPendingAcceptance) instead of adopting it
    // automatically. This is the knob's own opt-in design: it may be
    // running its own, separate exercise on this Voltra, unlike the
    // voltra-pedal project (no screen to ask on), which auto-joins.
    if (hostStatus == kTwinStatusTwinned && havePeer) {
      if (_twinState == TwinState::Twinned) {
        if (strcmp(_twinFollowerAddrStr, peerAddrStr) != 0) {
          Serial.printf("[voltra] twin: host confirms twinned with %s\n", peerAddrStr);
        }
        memcpy(_twinFollowerAddr, peer, 6);
        snprintf(_twinFollowerAddrStr, sizeof(_twinFollowerAddrStr), "%s", peerAddrStr);
        _state.twinState = TwinState::Twinned;
        snprintf(_state.twinFollowerAddrStr, sizeof(_state.twinFollowerAddrStr), "%s", peerAddrStr);
        _state.twinConfirmedByHost = true;
        _state.twinPendingAcceptance = false;
      } else {
        if (!_state.twinPendingAcceptance || strcmp(_state.twinPendingPeerAddrStr, peerAddrStr) != 0) {
          Serial.printf("[voltra] twin: host reports twinned with %s (not initiated on this knob -- offering as an option)\n", peerAddrStr);
        }
        memcpy(_twinPendingPeerAddr, peer, 6);
        _state.twinPendingAcceptance = true;
        snprintf(_state.twinPendingPeerAddrStr, sizeof(_state.twinPendingPeerAddrStr), "%s", peerAddrStr);
      }
    } else if (hostStatus == kTwinStatusAlone) {
      bool stillSettling = _twinState == TwinState::Twinned && (millis() - _twinJoinSentMs) < kTwinJoinSettleMs;
      if (stillSettling) {
        Serial.println("[voltra] twin: host reports alone shortly after our own join -- still settling, not clearing yet");
      } else {
        if (_twinState == TwinState::Twinned) {
          Serial.println("[voltra] twin: host reports alone -- clearing stale local twin state");
        }
        _twinState = TwinState::None;
        memset(_twinFollowerAddr, 0, sizeof(_twinFollowerAddr));
        _twinFollowerAddrStr[0] = '\0';
        _state.twinState = TwinState::None;
        _state.twinFollowerAddrStr[0] = '\0';
        _state.twinConfirmedByHost = false;
      }
      _state.twinPendingAcceptance = false;
      _state.twinPendingPeerAddrStr[0] = '\0';
    }
    // kTwinStatusJoining (0x11): transient (a follower is mid-join, either
    // ours or one we don't know about) -- leave everything as-is either way.
  };

  // Telemetry frame (phase: idle/concentric/hold/eccentric) -- not surfaced
  // in the UI yet, but must be recognized and skipped so it isn't
  // misparsed by one of the checks below.
  if (len >= 30 && frame[0] == kSyncByte && frame[1] == 0x3a && frame[2] == 0x04 && frame[3] == 0x70) {
    return;
  }

  // Twin status response/push (cmd 0xA7) -- see voltra_protocol.h's
  // kCmdTwinStatus comment. Has its own distinct header shape (cmd byte
  // directly at [10], no vendor-0xaa wrapper) that doesn't overlap any of
  // the checks below, so this can be checked unconditionally here.
  if (len >= 13 && frame[10] == kCmdTwinStatus) {
    applyTwinStatus(frame, len);
    return;
  }

  // Per-rep frame: fires once per completed rep during a set.
  if (isVendorFrame(frame, len, 0x82, 0x3b) && len >= 74) {
    _state.reps = frame[17];
    // A per-rep frame is unambiguous proof the device is actively mid-set,
    // regardless of what our own armed/loaded bookkeeping currently shows.
    // Originally this only fired while _state.armed was already true (the
    // pull-after-arming race, ported verbatim from the reference
    // implementation's onNotify() PerRep handling) -- but that left
    // _state.loaded stuck false, with no path to ever become true, whenever
    // a set started a way this knob's own fitnessMode/guidedState tracking
    // never saw or never recognized (started directly on the Voltra's own
    // console, from the Beyond+ app, or from another remote while this
    // knob happened to be mid-reconnect) -- even though _state.reps just
    // above was already tracking the real count correctly the whole time.
    // Trusting the per-rep frame itself removes that gap: whatever we
    // thought the state was, a real rep just happened, so we're loaded.
    // guidedState is set to match (rather than reset) since nothing above
    // this point already touched it for this frame type.
    if (!_state.loaded) {
      _state.loaded = true;
      _state.armed = false;
      _state.guidedState = kGuidedStateLoaded;
    }
    return;
  }
  // Set-summary frames: two different shapes seen for the same "set ended,
  // here's the final rep count" event.
  if (isVendorFrame(frame, len, 0x85, 0x5f) && len >= 110) { _state.reps = u16le(frame, 26); return; }
  if (isVendorFrame(frame, len, 0x86, 0x7d) && len >= 140) { _state.reps = u16le(frame, 16); return; }
  // A state-dump frame that happens to be the same length (52 bytes) as one
  // of the two battery-push shapes below. Must be excluded explicitly --
  // ported from the reference decoder, which found this by trial: without
  // this check, one in every so many "battery" readings was actually a
  // slice of this unrelated dump and came out as garbage.
  if (isVendorFrame(frame, len, 0x80, 0x25) && len >= 52) return;

  // Push notification: a setting changed. Shape: ... [10]=0x10 [11]=count, then count*(hi,lo,value) pairs.
  if (len >= 16 && frame[10] == 0x10) {
    const int count = frame[11];
    size_t off = 13;
    for (int i = 0; i < count && i < 16; i++) {
      if (off + 2 > len) break;
      uint8_t hi = frame[off], lo = frame[off + 1];
      off += 2;
      size_t width = isU16Param(hi, lo) ? 2 : 1;
      if (off + width > len) break;
      int value = width == 2 ? u16le(frame, off) : frame[off];
      off += width;
      applyParam(hi, lo, value);
    }
    if (_state.connState == ConnState::Handshaking) _handshakeEvent = true;
    return;
  }

  // Bulk cmd-0x0F read response: ... [10]=0x0f, count at [12..13] LE, then pairs from [14].
  if (len >= 14 && frame[0] == kSyncByte && (frame[2] == 0x08 || frame[2] == 0x09) && frame[10] == 0x0f) {
    const int count = u16le(frame, 12);
    size_t off = 14;
    for (int i = 0; i < count && i < 32; i++) {
      if (off + 2 > len) break;
      uint8_t hi = frame[off], lo = frame[off + 1];
      off += 2;
      size_t width = cmd0fWidth(hi, lo);
      if (width == 0 || off + width > len) break;
      int value = width == 2 ? u16le(frame, off) : frame[off];
      off += width;
      applyParam(hi, lo, value);
    }
    if (_state.connState == ConnState::Handshaking) _handshakeEvent = true;
    return;
  }

  // Mode-confirmed frame: [0]=0x55 [1]=0x12, value at [15]; register id at [13],[14].
  if (frame[0] == kSyncByte && frame[1] == 0x12 && len >= 18) {
    if (frame[13] == 0x89 && frame[14] == 0x3e) applyFitnessMode(frame[15]);
    if (_state.connState == ConnState::Handshaking) _handshakeEvent = true;
    return;
  }

  // Battery push notifications -- two different shapes, sent unsolicited by
  // the device (no read command triggers these). Checked last, and after
  // the 0x80/0x25 exclusion above, since one of them aliases that frame's
  // length.
  if (frame[0] == kSyncByte && frame[1] == 0x23 && len >= 35) {
    int pct = frame[11];
    if (pct >= 0 && pct <= 100) _state.batteryPct = pct;
    return;
  }
  if (frame[0] == kSyncByte && frame[1] == 0x34 && len >= 52) {
    int pct = frame[12];
    if (pct >= 0 && pct <= 100) _state.batteryPct = pct;
    return;
  }
}

void VoltraClient::setBaseWeightLb(int lb) {
  if (lb < kMinTargetLb) lb = kMinTargetLb;
  if (lb > kMaxTargetLb) lb = kMaxTargetLb;
  if (_state.loaded) {
    int lo = _state.baseWeightLb - kLoadedMaxDeltaLb, hi = _state.baseWeightLb + kLoadedMaxDeltaLb;
    lb = maxInt(lo, minInt(hi, lb));
  }
  if (!isReady()) {
    Serial.println("[voltra] setBaseWeightLb: not connected, ignoring");
    return;
  }
  auto f = buildParamWriteFrameU16(PARAM_BP_BASE_WEIGHT, (uint16_t)lb, nextSeq());
  writeCommand(f.data(), f.size());
  _state.baseWeightLb = lb;
  _state.haveBaseWeight = true;
  Serial.printf("[voltra] -> set base weight %d lb\n", lb);
}

void VoltraClient::setEccentricWeightLb(int lb) {
  int minE = maxInt(kMinEccentricLb, -_state.baseWeightLb);
  int maxE = minInt(kMaxEccentricLb, _state.baseWeightLb);
  if (lb < minE) lb = minE;
  if (lb > maxE) lb = maxE;
  if (!isReady()) {
    Serial.println("[voltra] setEccentricWeightLb: not connected, ignoring");
    return;
  }
  auto f = buildParamWriteFrameI16(PARAM_BP_ECCENTRIC_WEIGHT, (int16_t)lb, nextSeq());
  writeCommand(f.data(), f.size());
  _state.eccentricWeightLb = lb;
  _state.haveEccentricWeight = true;
  Serial.printf("[voltra] -> set eccentric weight %d lb\n", lb);
}

// "Inverse chains" -- PARAM_BP_CHAINS_WEIGHT. Unlike setBaseWeightLb() (whose
// 196-entry frame table is verified byte-for-byte against a real captured
// connection -- see voltra_protocol.h's header comment and
// tools/verify_crc.py) and setEccentricWeightLb() (same builder, same
// verified CRC path), this parameter ID was carried over from the Voltra's
// own parameter table but has NOT been independently exercised against real
// hardware -- no reference implementation or captured frame confirms what
// value range the machine actually accepts, or that writing it produces the
// resistance-profile behavior its name implies, rather than being rejected,
// clamped server-side to something else, or silently ignored.
//
// The clamp below (0..base weight) is this project's own conservative
// guess, not a documented limit -- it can't go negative (the param is
// declared uint16, unsigned, in voltra_protocol.h) and is capped at the
// current base weight on the assumption that an added-resistance profile
// shouldn't be able to exceed the weight it's modifying. Test cautiously on
// real hardware with small values first, and watch what the machine
// actually does -- don't assume this behaves like base/eccentric weight
// just because the wiring looks the same.
void VoltraClient::setChainsWeightLb(int lb) {
  int maxC = maxInt(0, minInt(kMaxTargetLb, _state.baseWeightLb));
  if (lb < 0) lb = 0;
  if (lb > maxC) lb = maxC;
  if (!isReady()) {
    Serial.println("[voltra] setChainsWeightLb: not connected, ignoring");
    return;
  }
  auto f = buildParamWriteFrameU16(PARAM_BP_CHAINS_WEIGHT, (uint16_t)lb, nextSeq());
  writeCommand(f.data(), f.size());
  _state.chainsWeightLb = lb;
  _state.haveChainsWeight = true;
  Serial.printf("[voltra] -> set chains weight %d lb (UNVERIFIED param -- watch real behavior)\n", lb);
}

// Chains/Inverse/Mountain are one shared amount (chainsWeightLb above) plus
// a pair of mutually-exclusive one-byte style flags that pick which
// resistance profile that amount produces -- see voltra_protocol.h's
// PARAM_FITNESS_INVERSE_CHAIN/PARAM_MOUNTAIN_CURVE comment. Same UNVERIFIED
// caveat as setChainsWeightLb() above: this project has not independently
// confirmed these two registers against real hardware, only carried them
// over from a second reference project (kirby6365/voltra_remote) that also
// targets this board/protocol. Both flags are written every call (never
// just the one that changed) so a stale flag from a previous style can
// never survive a switch -- e.g. going Mountain -> Chains must actively
// clear the mountain flag, not just leave the inverse one at 0. Two writes
// back-to-back, so a kWriteGapMs gap is inserted between them like engage()
// does for its own two-write SETUP+GO sequence above.
void VoltraClient::setChainsStyle(ChainsStyle style) {
  if (!isReady()) {
    Serial.println("[voltra] setChainsStyle: not connected, ignoring");
    return;
  }
  uint8_t inverseFlag = (style == ChainsStyle::Inverse) ? 1 : 0;
  uint8_t mountainFlag = (style == ChainsStyle::Mountain) ? 1 : 0;
  auto f1 = buildParamWriteFrameU8(PARAM_FITNESS_INVERSE_CHAIN, inverseFlag, nextSeq());
  writeCommand(f1.data(), f1.size());
  delay(kWriteGapMs);
  auto f2 = buildParamWriteFrameU8(PARAM_MOUNTAIN_CURVE, mountainFlag, nextSeq());
  writeCommand(f2.data(), f2.size());
  _state.chainsStyle = style;
  Serial.printf("[voltra] -> set chains style %d (0=chains,1=inverse,2=mountain) (UNVERIFIED param -- watch real behavior)\n", (int)style);
}

void VoltraClient::engage() {
  if (!isReady()) {
    Serial.println("[voltra] engage: not connected, ignoring");
    return;
  }
  // SETUP then a short gap then the load write -- this exact two-step
  // sequence (not just the load write alone) is what the reference
  // implementation always does before a plain load; matching it byte-for-
  // byte here rather than skipping the SETUP step, since this firmware
  // hasn't had its own load path exercised against real hardware yet.
  writeCommand(kSetupFrame, sizeof(kSetupFrame));
  delay(kLoadSetupGapMs);
  // Twinned: FITNESS_MODE_TWIN_LOADED instead of the single-unit value --
  // writing the single-unit value to a twinned host loads the host alone,
  // not the pair (see voltra_protocol.h's kCmdTwinLink comment).
  bool twinned = _twinState == TwinState::Twinned;
  uint16_t mode = twinned ? FITNESS_MODE_TWIN_LOADED : FITNESS_MODE_STRENGTH_LOADED;
  auto f = buildParamWriteFrameU16(PARAM_BP_SET_FITNESS_MODE, mode, nextSeq());
  writeCommand(f.data(), f.size());
  _state.loaded = true;
  _state.armed = false;
  _state.reps = 0;
  Serial.printf("[voltra] -> engage (SETUP + %s)\n", twinned ? "TWIN_LOADED" : "STRENGTH_LOADED");
}

void VoltraClient::disengage() {
  if (!isReady()) {
    Serial.println("[voltra] disengage: not connected, ignoring");
    return;
  }
  bool twinned = _twinState == TwinState::Twinned;
  uint16_t mode = twinned ? FITNESS_MODE_TWIN_READY : FITNESS_MODE_STRENGTH_READY;
  auto f = buildParamWriteFrameU16(PARAM_BP_SET_FITNESS_MODE, mode, nextSeq());
  writeCommand(f.data(), f.size());
  _state.loaded = false;
  _state.armed = false;
  _state.guidedState = -1;
  _state.guidedCountdownMs = -1;
  Serial.printf("[voltra] -> disengage (%s)\n", twinned ? "TWIN_READY" : "STRENGTH_READY");
}

void VoltraClient::armAutoLoad() {
  if (!isReady()) {
    Serial.println("[voltra] armAutoLoad: not connected, ignoring");
    return;
  }
  if (_state.loaded || _state.armed) {
    Serial.println("[voltra] armAutoLoad: already loaded/armed, ignoring");
    return;
  }
  // GUIDED_LOAD_TRIGGER is a raw vendor command (cmd byte 0xaa), sent as a
  // literal captured frame -- there's no generic builder for this one, see
  // voltra_protocol.h.
  //
  // NOT adjusted for twin mode -- the reference project's twin-mode notes
  // mention auto-load's "finished" status reading differently while twinned
  // (a different raw value on the guided-state register) but don't fully
  // pin down which register or how it maps onto this project's own
  // kGuidedState* constants, so this is left as-is rather than guessed at.
  // If auto-load while twinned arms/counts down fine but never reports
  // loaded (_state.loaded stays false, guidedState stuck below
  // kGuidedStateLoaded), that's the likely cause -- check a real captured
  // frame before changing kGuidedStateLoaded's value.
  writeCommand(kGuidedLoadTrigger, sizeof(kGuidedLoadTrigger));
  _state.armed = true;
  _state.loaded = false;
  _state.guidedState = kGuidedStateWaiting;
  _state.guidedCountdownMs = -1;
  _state.reps = 0;
  _lastArmPollMs = 0; // poll guided-load status on the very next update()
  Serial.println("[voltra] -> arm auto-load (GUIDED_LOAD_TRIGGER)");
}

} // namespace voltra
