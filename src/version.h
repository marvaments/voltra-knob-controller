#pragma once

// Single source of truth for the firmware version string, shown on
// Settings > About and logged once at boot (see main.cpp). Bump this with
// every build that goes out to real hardware -- dev boards flashed over
// USB or, eventually, customer-owned units updated over OTA -- so the About
// screen and the serial log always agree on what's actually running. There
// was no version string anywhere in this project before this file; this is
// intentionally just one constant rather than a build-date/git-hash scheme,
// since bumping it by hand at each real release is simple and this project
// has no CI step to auto-generate one.
namespace fw {
constexpr const char *kVersion = "0.5.6";
}
