#pragma once
#include <cstdint>

// Two generic "weight memory" slots for the workout screen's tap=recall/
// hold=store gesture (see ui.cpp's kPresetZone0/kPresetZone1 handling). A
// slot just remembers a single lb number -- it's not tied to Base,
// Eccentric, or Chains specifically. Storing captures whichever value is
// currently focused (sMode) into a slot; recalling applies a slot's stored
// number back to whichever value is currently focused at recall time, which
// may be a different one than when it was stored -- that's intentional
// (generic value memory, not a per-mode preset), not a bug.
//
// Deliberately its own NVS namespace ("voltra_wpre"), separate from
// prefs.cpp's "voltra_prefs" -- see that file's own namespace comment for
// why each concern gets its own namespace rather than sharing one.
namespace wpresets {

// Call once from setup(), before anything else here is read.
void begin();

// slot is 0 or 1 (two slots total -- matches kPresetZone0/kPresetZone1 in
// ui.cpp). Any other value is treated as "no value" / a no-op, defensively,
// though nothing in this firmware ever passes one.
bool hasValue(uint8_t slot);
int valueLb(uint8_t slot); // meaningful only when hasValue(slot) is true

void store(uint8_t slot, int lb);

} // namespace wpresets
