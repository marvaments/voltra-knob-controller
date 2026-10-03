// Beyond Power VOLTRA BLE protocol constants.
//
// v2: rebuilt against a real, working, deployed implementation found by the
// user -- https://github.com/omarshahine/voltra-knob (a different physical
// knob, the Elecrow CrowPanel 2.1", but the SAME Voltra BLE protocol). That
// project's frames.gen.h and voltra_link.cpp are the ground truth here.
//
// Independent verification done before trusting any of it: every single
// byte-array literal in that project's generated protocol header (212 of
// them -- all 196 weight-table entries plus every named frame) was checked
// against our own CRC-8/CRC-16 implementation, and every one validates
// exactly. See tools/verify_crc.py. That also confirms our own
// buildParamWriteFrameU16/I16 payload shape is correct, since it reproduces
// their real captured GO/STOP/weight frames byte-for-byte.
//
// v1 (superseded) was built from a different, independent reverse-engineering
// project (dylanmaniatakes/Beyond-Power-HomeAssistant) and had one real bug:
// it had the write and notify characteristic UUIDs backwards. Fixed here.

#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace voltra {

// ---- BLE GATT identifiers -------------------------------------------------
// Only two characteristics are actually needed; the Voltra's GATT service
// exposes two more (55CA... as an alternate notify path, and a write-only
// one) that the real, working reference implementation doesn't use at all.

constexpr const char *kServiceUUID  = "e4dada34-0867-8783-9f70-2ca29216c7e4";
constexpr const char *kWriteCharUUID  = "a010891d-f50f-44f0-901f-9a2421a9e050"; // write commands here
constexpr const char *kNotifyCharUUID = "55ca1e52-7354-25de-6afc-b7df1e8816ac"; // subscribe here

// ---- Frame-level constants --------------------------------------------------

constexpr uint8_t  kSyncByte        = 0x55;
constexpr uint8_t  kSenderId        = 0xAA; // fixed for all post-handshake traffic
constexpr uint8_t  kReceiverId      = 0x10;
constexpr uint16_t kProtocolField   = 0x0020;
constexpr uint8_t  kFrameType       = 0x04; // seen on all read/write frames after connect

constexpr uint8_t kCmdParamRead  = 0x0F;
constexpr uint8_t kCmdParamWrite = 0x11;

// ---- Parameter IDs ----------------------------------------------------------

constexpr uint16_t PARAM_BP_BASE_WEIGHT        = 0x3E86; // uint16 LE, lb, range 5..200
constexpr uint16_t PARAM_BP_CHAINS_WEIGHT      = 0x3E87; // uint16 LE, lb
constexpr uint16_t PARAM_BP_ECCENTRIC_WEIGHT   = 0x3E88; // int16 LE, lb, range -200..200 (clamp to +/- base weight)
constexpr uint16_t PARAM_BP_SET_FITNESS_MODE   = 0x3E89; // uint16 LE, write-only mode command
constexpr uint16_t PARAM_FITNESS_WORKOUT_STATE = 0x4FB0; // uint16 LE, idle(0)/weight-training(1)/... mode select+status

// Chains, Inverse Chains, and Mountain are NOT three separate weights --
// they're one shared amount (PARAM_BP_CHAINS_WEIGHT above) plus a pair of
// mutually-exclusive one-byte boolean style flags that pick which
// resistance profile that amount produces. Ported from
// kirby6365/voltra_remote's voltra_protocol.h (a second, independent
// open-source project targeting this same board/protocol, distinct from
// the omarshahine/voltra-knob project this file's header comment credits
// for everything else) -- like PARAM_BP_CHAINS_WEIGHT itself, these are
// carried over from the Voltra's own parameter table but have NOT been
// independently exercised against real hardware from this project. Neither
// flag is ever pushed unsolicited by the device (see VoltraClient::
// setChainsStyle's comment), so plain Chains is just "neither flag set" --
// there's no separate PARAM_FITNESS_CHAINS id to write.
constexpr uint16_t PARAM_FITNESS_INVERSE_CHAIN = 0x53B0; // uint8, 1 = inverse chains style
constexpr uint16_t PARAM_MOUNTAIN_CURVE        = 0x556F; // uint8, 1 = mountain style

// Guided (auto-)load status registers -- read-only, polled via
// kReadGuidedLoadStatus while fitnessMode == FITNESS_MODE_DIRECT_LOAD.
// Register IDs and meanings ported from omarshahine/voltra-knob's
// app/voltra_link.cpp (the only place this side of the Voltra's own
// firmware documents what 0x538D/0x53C7/0x53C8/0x53C9 mean).
constexpr uint16_t PARAM_BP_GUIDED_SAFETY    = 0x538D; // uint8, 0 = ok
constexpr uint16_t PARAM_BP_GUIDED_STATE     = 0x53C7; // uint8, see kGuidedState* below
constexpr uint16_t PARAM_BP_GUIDED_COUNTDOWN = 0x53C8; // uint16 LE, ms remaining, valid only while state==COUNTDOWN
constexpr uint16_t PARAM_BP_GUIDED_CTRL      = 0x53C9; // uint8, not surfaced in the UI, kept for completeness

// Values written to PARAM_BP_SET_FITNESS_MODE / read back from it.
constexpr uint16_t FITNESS_MODE_STRENGTH_READY  = 0x0004; // "unload" / disengage
constexpr uint16_t FITNESS_MODE_STRENGTH_LOADED = 0x0005; // "load" / engage (plain strength mode)
constexpr uint16_t FITNESS_MODE_AUTOLOAD_ARMED  = 0x0026; // device is armed, waiting for the first pull
constexpr uint16_t FITNESS_MODE_DIRECT_LOAD     = 0x0027; // reported right after auto-load arms; counts as armed until PARAM_BP_GUIDED_STATE reaches LOADED

// Values of PARAM_BP_GUIDED_STATE (register 0x53C7) while fitnessMode ==
// FITNESS_MODE_DIRECT_LOAD. Ported from the reference implementation's
// GUIDED_WAITING/COUNTDOWN/ENGAGING/LOADED constants -- Beyond Power hasn't
// published names for these, these are the reference project's own labels.
constexpr int kGuidedStateWaiting   = 11; // armed, waiting for the first pull
constexpr int kGuidedStateCountdown = 12; // pull detected, safety countdown running (see guidedCountdownMs)
constexpr int kGuidedStateEngaging  = 13; // countdown finished, motor engaging
constexpr int kGuidedStateLoaded    = 14; // fully engaged -- equivalent to "loaded" for a plain load

// Values written to PARAM_FITNESS_WORKOUT_STATE.
constexpr uint16_t WORKOUT_MODE_IDLE            = 0x0000;
constexpr uint16_t WORKOUT_MODE_WEIGHT_TRAINING = 0x0001;

// ---- Twin mode --------------------------------------------------------------
// Pairs two VOLTRA units so weight/load/unload from one remote drives both
// (Beyond Power's own feature -- see the in-app/on-device "Twin" setup
// flow). Ported from stevenpoint/voltra_remote's watch206 branch
// (docs/PROTOCOL.md, "Twin mode" section) -- a third independent
// reverse-engineering project targeting this same protocol, alongside
// omarshahine/voltra-knob (credited at the top of this file) and
// kirby6365/voltra_remote (credited on the Chains style flags below). That
// project's own twin-mode implementation is hardware-tested on real
// VOLTRAs; the byte layouts below are carried over from its documentation,
// not independently re-verified against a captured frame from THIS
// project's own tooling -- in particular, the peer-address byte order in
// kCmdTwinLink's payload is taken from that project's own "as printed"
// colon-notation convention (e.g. "80:b5:4e:07:02:a6"), which voltra_ble.cpp
// reproduces by parsing NimBLEAddress::toString() rather than trusting any
// particular BLE stack's raw internal byte order -- see linkFollower()'s
// comment there if twinning doesn't work on the first real-hardware try.
constexpr uint8_t kCmdTwinLink = 0xA8; // payload: [subcmd, 6-byte peer address, in printed order]
constexpr uint8_t kTwinLinkSubcmd   = 0x01; // sent to the FOLLOWER, payload continues with the HOST's address: "join this host"
constexpr uint8_t kTwinUnlinkSubcmd = 0x02; // sent to the HOST, payload continues with the FOLLOWER's address: "drop this follower"

// Raw vendor-trigger command -- same cmd byte kGuidedLoadTrigger below
// already uses for arming auto-load, just a different payload here. Sent to
// the host every kTwinHeartbeatMs while loaded AND twinned. The reference
// project's own docs call its purpose "unknown" but send it unconditionally
// in that state, so this does too rather than risk the pair silently
// drifting out of sync without it.
constexpr uint8_t kCmdVendorTrigger = 0xAA;
constexpr uint8_t kTwinHeartbeatPayload[2] = {0x13, 0x01};
constexpr uint32_t kTwinHeartbeatMs = 500;

// Fitness-mode values that replace FITNESS_MODE_STRENGTH_LOADED/READY above
// while twinned -- writing the single-unit values to a twinned host
// loads/unloads the host alone, not the pair.
constexpr uint16_t FITNESS_MODE_TWIN_LOADED = 0x0101;
constexpr uint16_t FITNESS_MODE_TWIN_READY  = 0x0100;

constexpr uint32_t kTwinScanDurationMs = 5000; // how long startTwinDiscovery()'s scan runs

// Confirms twin status from the host itself, rather than only ever trusting
// our own memory of having sent a join/unlink command -- see
// VoltraClient::twinWithCandidate()/untwin()'s "sent, not confirmed"
// caveat above. Same source as kCmdTwinLink above (stevenpoint/voltra_remote's
// watch206 branch, docs/PROTOCOL.md "Twin mode"), and independently
// cross-checked: the companion voltra-pedal project (a separate ESP32
// board/firmware also targeting this protocol) ported this exact command
// on its own and reports it confirmed working against two real VOLTRA I
// units, including surviving a real spontaneous untwin and an automatic,
// unattended rejoin about a minute later.
constexpr uint8_t kCmdTwinStatus = 0xA7; // empty-payload request; host answers once and also pushes this unsolicited on any twin-state change
constexpr int kTwinStatusAlone   = 0x00;
constexpr int kTwinStatusJoining = 0x11; // a follower has been told to join but the host hasn't confirmed the pair yet
constexpr int kTwinStatusTwinned = 0x12;
constexpr uint32_t kTwinStatusPollMs = 10000; // how often to re-read it while Ready (matches the voltra-pedal project's own interval)

// Grace period after sending a join before an "alone" status report is
// trusted enough to clear it back out. The real pairing happens over a
// SEPARATE BLE connection directly between the two Voltra units -- the
// follower has to disconnect from us, then connect to and handshake with
// the actual host -- which takes real time (comparable to how long our
// own connect+handshake elsewhere in this file takes). Polling status
// immediately after sending the join will usually still see "alone"
// simply because that hasn't happened yet; without this grace period,
// that premature read was clearing the very state twinWithCandidate() had
// just optimistically set, moments before the real pairing actually
// completed -- confirmed on real hardware (TWIN WITH THIS looked like it
// failed instantly, every time). Long enough to comfortably cover at
// least one non-immediate kTwinStatusPollMs poll after the first one.
constexpr uint32_t kTwinJoinSettleMs = 15000;

// How long to wait for the follower's own reply to the join command before
// giving up on it -- matches the voltra-pedal project's own
// TWIN_JOIN_REPLY_TIMEOUT_MS, which is confirmed working against real
// hardware. A 0x00 reply means accepted; the follower hanging up on its
// own also counts as accepted (it disconnects to go join the host) -- only
// a timeout with the connection still open and nothing back counts as
// failure.
constexpr uint32_t kTwinJoinReplyTimeoutMs = 4000;
// Extra settle time after the init frames, before sending the join command
// itself -- matches the pedal project's own TWIN_JOIN_SETTLE_MS.
constexpr uint32_t kTwinFollowerSettleMs = 800;

// UI-facing limits (from the reference implementation's WeightDial).
constexpr int kMinTargetLb    = 5;
constexpr int kMaxTargetLb    = 200;
constexpr int kMinEccentricLb = -200;
constexpr int kMaxEccentricLb = 200;
constexpr int kLoadedMaxDeltaLb = 25; // while loaded, one dial change may move at most this far from confirmed weight

// ---- Literal captured frames (byte-exact, from a real working connection) --
// These are used verbatim, not reconstructed, for the handshake -- so the
// handshake does not depend on our own CRC code being right (though it now
// is, independently confirmed -- see the file header comment).

struct LiteralFrame {
  const char *label;
  const uint8_t *bytes;
  size_t len;
};

// clang-format off
// Custom "Voltra Knob" identity frame, sent first to authenticate the connection.
static const uint8_t kAuthKnob[] = {0x55,0x29,0x04,0xc9,0x01,0x10,0x00,0x00,0x20,0x00,0x4f,0x56,0x6f,0x6c,0x74,0x72,0x61,0x20,0x4b,0x6e,0x6f,0x62,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x84,0xab,0x1a,0x5f,0x29,0x20,0x01,0xf3,0x1b};
// Fallback identity (the Voltra SDK's own "iPhone" string) -- try this if AUTH_KNOB is ever rejected.
static const uint8_t kAuthIphoneFallback[] = {0x55,0x29,0x04,0xc9,0x01,0x10,0x00,0x00,0x20,0x00,0x4f,0x69,0x50,0x68,0x6f,0x6e,0x65,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x84,0xab,0x1a,0x5f,0x29,0x20,0x01,0x72,0xd8};
static const uint8_t kInit0[] = {0x55,0x0f,0x08,0x01,0xaa,0xd2,0x00,0x00,0x20,0x00,0xff,0x00,0xaa,0x04,0x19};
static const uint8_t kInit1[] = {0x55,0x1f,0x04,0x4e,0xaa,0x10,0x00,0x00,0x20,0x00,0x27,0x81,0x10,0x65,0xf8,0xc6,0xb9,0x17,0x90,0x47,0xb7,0x97,0xd3,0x35,0x1c,0x04,0xbf,0xfb,0x92,0x5c,0xd6};
// Selects weight-training mode; the device's reply to this is what confirms the handshake worked.
static const uint8_t kModeWeightTraining[] = {0x55,0x13,0x04,0x03,0xaa,0x10,0x00,0x20,0x20,0x00,0x11,0x01,0x00,0xb0,0x4f,0x01,0x00,0xf3,0x53};
static const uint8_t kModeIdle[] = {0x55,0x13,0x04,0x03,0xaa,0x10,0x00,0x20,0x20,0x00,0x11,0x01,0x00,0xb0,0x4f,0x00,0x00,0x2b,0x4a};
// Subscribes to all telemetry (reps/phase/etc). Last two payload shorts are a bitmask; all-FF = everything.
static const uint8_t kTelemetrySubscribeAll[] = {0x55,0x15,0x04,0xa9,0xaa,0x10,0x01,0x20,0x20,0x00,0x11,0x01,0x00,0x83,0x51,0xff,0xff,0xff,0xff,0x35,0xeb};
// Bulk cmd-0x0F read of base weight (3e86), fitness mode (3e89), workout mode (4fb0).
static const uint8_t kReadSettings[] = {0x55,0x15,0x04,0xa9,0xaa,0x10,0x00,0x20,0x20,0x00,0x0f,0x03,0x00,0x86,0x3e,0x89,0x3e,0xb0,0x4f,0xc2,0x2f};
// Sent right before the plain-load GO write, mirroring the reference project's exact
// Load sequence byte-for-byte -- a cmd-0x0F read of two setup registers (0x506a,
// 0x3e82) whose purpose isn't otherwise documented, but which the one known-working
// implementation always sends first. Cheap to include verbatim; skipping it is an
// untested deviation from what's actually been run against real hardware.
static const uint8_t kSetupFrame[] = {0x55,0x13,0x04,0x03,0xaa,0x10,0x15,0x00,0x20,0x00,0x0f,0x02,0x00,0x6a,0x50,0x82,0x3e,0x8f,0x2f};
// Arms auto-load: device runs its own safety check + countdown, then engages on the
// first pull. A raw vendor command (cmd byte 0xaa, not a param read/write), so it's
// used as a literal captured frame rather than built from voltra::buildFrame().
static const uint8_t kGuidedLoadTrigger[] = {0x55,0x0e,0x04,0x66,0xaa,0x10,0x00,0x20,0x20,0x00,0xaa,0x12,0x52,0x31};
// Bulk cmd-0x0F read of the guided-load status registers (538d,53c7,53c8,53c9) --
// poll this periodically while fitnessMode == FITNESS_MODE_DIRECT_LOAD so the UI
// follows the device's own arm/countdown/engage timeline.
static const uint8_t kReadGuidedLoadStatus[] = {0x55,0x17,0x04,0x38,0xaa,0x10,0x00,0x20,0x20,0x00,0x0f,0x04,0x00,0x8d,0x53,0xc7,0x53,0xc8,0x53,0xc9,0x53,0xd5,0xf9};
// clang-format on

constexpr uint32_t kArmPollMs = 500;      // how often to re-send kReadGuidedLoadStatus while armed/engaging
constexpr uint32_t kLoadSetupGapMs = 300; // gap between kSetupFrame and the GO write

constexpr size_t kInitCount = 2;
inline LiteralFrame initFrame(size_t i) {
  return i == 0 ? LiteralFrame{"INIT0", kInit0, sizeof(kInit0)} : LiteralFrame{"INIT1", kInit1, sizeof(kInit1)};
}

// ---- Timing (from the reference implementation) -----------------------------

constexpr uint32_t kAuthWaitMs = 3000;
constexpr uint32_t kInitGapMs = 20;
constexpr uint32_t kWriteGapMs = 500; // minimum spacing between ordinary writes once connected
constexpr uint32_t kHandshakeDeadlineMs = 5000;
constexpr uint32_t kConnectRetryBaseDelayMs = 2000;
constexpr uint32_t kConnectRetryMaxDelayMs = 30000;

// ---- Frame builders (implemented in voltra_protocol.cpp) -------------------
// Used for values not covered by a literal frame above (eccentric weight has
// no precomputed table in the reference project; base weight does, via a
// 196-entry lookup table, but our builder reproduces it byte-for-byte -- see
// tools/verify_crc.py -- so we use one builder for both instead of vendoring
// their table).

std::vector<uint8_t> buildParamWriteFrameU16(uint16_t paramId, uint16_t value, uint16_t seq);
std::vector<uint8_t> buildParamWriteFrameI16(uint16_t paramId, int16_t value, uint16_t seq);
// One-byte variant (5-byte payload instead of 6) for the style-selector
// flags above -- there's no u16-width parameter that needs this shape yet,
// but the wire format is otherwise identical to buildParamWriteFrameU16.
std::vector<uint8_t> buildParamWriteFrameU8(uint16_t paramId, uint8_t value, uint16_t seq);
// Generic builder for a raw command frame outside the param-read/write
// shape above -- used for twin mode's kCmdTwinLink/kCmdVendorTrigger, whose
// payloads aren't a [flag, paramId, value] triple. Exposes the same
// internal buildFrame() every other builder in this file already funnels
// through, just without a param-write-specific payload shape imposed on it.
std::vector<uint8_t> buildCommandFrame(uint8_t cmd, const uint8_t *payload, size_t payloadLen, uint16_t seq);

} // namespace voltra
