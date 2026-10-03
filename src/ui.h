#pragma once
#include <Arduino_GFX_Library.h>
#include "voltra_ble.h"
#include "touch_cst816.h"

namespace ui {

void begin(Arduino_Canvas *canvas);

// Call every loop iteration with the latest encoder delta (detents since
// last call, may be 0), the latest touch reading, the current Voltra
// state, and the knob board's OWN battery percent (0-100, or -1 if not
// yet known -- see board_battery.h; this is separate from the Voltra's
// own battery, which lives inside voltraClient's state). Handles input,
// mutates the Voltra connection as needed, and redraws the screen only
// when something actually changed.
void update(int32_t encoderDelta, const touch::TouchPoint &tp, voltra::VoltraClient &voltraClient,
            int boardBatteryPercent);

} // namespace ui
