#include "bootscreen.h"
#include "bootlogo.h"
#include "display.h"
#include <Arduino_GFX_Library.h>
#include <Arduino.h>

namespace bootscreen {

void show(Arduino_Canvas *canvas, uint32_t holdMs) {
  // Fill with the logo art's own cream background rather than this file's
  // current UI theme (kBg/etc in ui.cpp) -- this is a literal brand splash,
  // not a themed screen, so it should look the same regardless of which
  // color theme the user has picked in Settings.
  canvas->fillScreen(bootlogo::kBgColor565);
  int16_t x = (display::kWidth - bootlogo::kWidth) / 2;
  int16_t y = (display::kHeight - bootlogo::kHeight) / 2;
  // This library's Arduino_Canvas override only implements the non-const
  // uint16_t* overload of draw16bitRGBBitmap (despite Arduino_GFX's base
  // class also declaring a `const uint16_t bitmap[]` one) -- const_cast is
  // safe here since it only ever reads from this array.
  canvas->draw16bitRGBBitmap(x, y, const_cast<uint16_t *>(bootlogo::kBitmap), bootlogo::kWidth, bootlogo::kHeight);
  canvas->flush();
  delay(holdMs);
}

} // namespace bootscreen
