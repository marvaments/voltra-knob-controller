#include "touch_cst816.h"
#include <Arduino.h>
#include <Wire.h>

namespace touch {

static constexpr uint8_t kAddr = 0x15;
static int sRst = -1, sInt = -1;

void begin(int sda, int scl, int rstPin, int intPin) {
  sRst = rstPin;
  sInt = intPin;
  pinMode(sInt, INPUT);
  pinMode(sRst, OUTPUT);

  Wire.begin(sda, scl, 400000);

  digitalWrite(sRst, LOW);
  delay(20);
  digitalWrite(sRst, HIGH);
  delay(50);
}

TouchPoint read() {
  TouchPoint p;

  Wire.beginTransmission(kAddr);
  Wire.write(0x02); // FingerNum register; XposH/L, YposH/L follow at 0x03-0x06
  if (Wire.endTransmission(false) != 0) {
    return p; // device not responding this cycle
  }

  if (Wire.requestFrom((int)kAddr, 5) != 5) {
    return p;
  }

  uint8_t fingerNum = Wire.read();
  uint8_t xh = Wire.read();
  uint8_t xl = Wire.read();
  uint8_t yh = Wire.read();
  uint8_t yl = Wire.read();

  if (fingerNum == 0) {
    return p;
  }

  p.touched = true;
  p.x = ((xh & 0x0F) << 8) | xl;
  p.y = ((yh & 0x0F) << 8) | yl;
  return p;
}

} // namespace touch
