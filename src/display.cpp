#include "display.h"
#include "sh8601_init.h"
#include <Arduino.h>

namespace display {

static Arduino_DataBus *sBus = nullptr;
static Arduino_GFX *sPanel = nullptr;
static Arduino_Canvas *sCanvas = nullptr;

// PWM backlight so brightness can be dimmed (idle/power-saving) instead of
// only switched fully on/off. 5kHz is well above any visible-flicker
// range; 8-bit duty is plenty of resolution for a handful of brightness
// steps -- no need for anything finer here.
static constexpr uint32_t kBacklightPwmFreqHz = 5000;
static constexpr uint8_t kBacklightPwmResBits = 8;

void setBacklight(uint8_t percent) {
  if (percent > 100) percent = 100;
  uint32_t duty = (uint32_t)percent * 255 / 100;
  ledcWrite(kPinBacklight, duty);
}

Arduino_Canvas *begin() {
  sBus = new Arduino_ESP32QSPI(kPinCs, kPinClk, kPinD0, kPinD1, kPinD2, kPinD3);
  // Waveshare's own official demo drives this exact panel through the
  // ESP-IDF `esp_lcd_sh8601` component, not through Arduino_GFX's
  // Arduino_ST77916 driver -- confirmed by reading their demo source
  // (ESP32-S3-Knob-Touch-LCD-1.8-Demo/Arduino/examples/08_LVGL_Test).
  // Both of Arduino_GFX's built-in ST77916 init tables were tried on real
  // hardware and produced identical scrambled/garbled output; the QSPI
  // command framing itself checks out identical between the two drivers
  // (both send an 0x02/0x32 opcode + register in a 24-bit field over all
  // 4 lines), so the problem was specifically the init register values.
  // kSh8601InitOperations (sh8601_init.h) is a byte-for-byte port of
  // Waveshare's real `lcd_init_cmds` table plus the MADCTL/COLMOD writes
  // their driver issues automatically before it -- see that file's
  // comment for the full reasoning.
  sPanel = new Arduino_ST77916(
      sBus, kPinRst, 0 /* rotation */, true /* IPS */, kWidth, kHeight,
      0, 0, 0, 0,
      kSh8601InitOperations, sizeof(kSh8601InitOperations));
  sCanvas = new Arduino_Canvas(kWidth, kHeight, sPanel, 0, 0, 0);

  sCanvas->begin();
  // Waveshare's table ends with display inversion ON (needed for correct,
  // non-photo-negative color on this panel). Arduino_ST77916::tftInit()
  // itself unconditionally calls invertDisplay(false) right after running
  // our init table, which we originally assumed meant "send INVOFF" -- so
  // we called invertDisplay(true) here to undo it. That was backwards:
  // read from the actual installed library source
  // (Arduino_ST77916::invertDisplay), the real implementation is
  //   _bus->sendCommand((_ips ^ i) ? ST77916_INVON : ST77916_INVOFF);
  // and we construct this panel with ips=true below, so the boolean is
  // XORed against that: invertDisplay(false) is what actually sends
  // ST77916_INVON, and invertDisplay(true) sends ST77916_INVOFF -- the
  // opposite of the "true means inverted" reading a caller would expect.
  // tftInit()'s own invertDisplay(false) call therefore already leaves the
  // panel correctly set to INVON, and our previous invertDisplay(true)
  // here was immediately undoing that back to INVOFF -- which is exactly
  // why colors were coming out as a photo negative (white background,
  // blue-hued text) despite the source having the right RGB565 values the
  // whole time. Calling invertDisplay(false) again here is redundant with
  // what tftInit() just did, but keeps this correct even if that library
  // internal ever changes.
  sPanel->invertDisplay(false);
  sCanvas->fillScreen(RGB565_BLACK);
  sCanvas->flush();

  ledcAttach(kPinBacklight, kBacklightPwmFreqHz, kBacklightPwmResBits);
  setBacklight(100);

  return sCanvas;
}

void flush(Arduino_Canvas *canvas) { canvas->flush(); }

} // namespace display
