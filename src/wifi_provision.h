#pragma once
#include <Arduino.h>

// Runtime WiFi provisioning: lets someone connect this board to their own
// WiFi network from the knob itself (Settings > Wi-Fi), instead of the
// network being baked into the firmware at compile time. There is
// deliberately no compile-time fallback (see wifi_provision.cpp) -- every
// board, including your own dev/bench units, has to go through this flow
// at least once. That's the whole point: it guarantees no real WiFi
// credentials can ever end up compiled into a firmware image, so nothing
// you mass-flash for someone else to own can leak your own network.
//
// The provisioning flow itself is a temporary SoftAP + captive portal (the
// same idea most consumer IoT devices use -- smart plugs, bulbs, etc.):
// tapping "ADD WIFI" turns the board into its own open WiFi hotspot for a
// minute or two; a phone joins that hotspot, its OS pops up (or the user
// opens) a simple web page hosted by the board itself, picks a network from
// a scanned list (or types one in) and enters the password there -- far
// less error-prone on a phone keyboard than trying to spell out a WPA2
// password one character at a time with a rotary knob. See
// wifi_provision.cpp for the actual portal implementation.
namespace wifiprov {

enum class State {
  Disconnected, // no saved network, or intentionally not trying right now
  Connecting,   // attempting to join a saved network
  Connected,    // joined and has an IP
  Provisioning, // SoftAP + captive portal active, waiting for a phone
};

struct Status {
  State state = State::Disconnected;
  bool hasSaved = false; // true if a network is saved in NVS
  char ssid[33] = "";    // the saved/target network name
  char ip[16] = "";      // current IP as text, only valid while Connected
  char apName[24] = "";  // the SoftAP name shown to the user while Provisioning
};

// Call once from setup(). Loads a saved network from NVS and starts
// connecting in the background if one is available. There is no
// compiled-in fallback -- a board with nothing saved yet stays
// Disconnected until startProvisioning() is used (see wifi_provision.cpp).
// Non-blocking.
void begin();

// Call every loop() iteration. Non-blocking: services STA connect/backoff,
// or the SoftAP captive-portal web server while Provisioning.
void update();

Status status();

// Enters SoftAP + captive-portal mode so a phone can submit new WiFi
// credentials. Safe to call from any state.
void startProvisioning();

// Leaves provisioning mode and returns to normal STA behavior (reconnecting
// to the saved network if there is one). No-op if not currently provisioning.
void cancelProvisioning();

// Clears the saved network and disconnects. State becomes Disconnected with
// hasSaved=false until provisioned again.
void forgetNetwork();

} // namespace wifiprov
