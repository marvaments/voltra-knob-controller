#include "weight_presets.h"
#include <Arduino.h>
#include <Preferences.h>

namespace wpresets {

static constexpr const char *kNamespace = "voltra_wpre";
static constexpr const char *kKeyHave[2] = {"have0", "have1"};
static constexpr const char *kKeyVal[2] = {"val0", "val1"};

static bool sHave[2] = {false, false};
static int sValue[2] = {0, 0};

void begin() {
  Preferences p;
  p.begin(kNamespace, /*readOnly=*/true);
  for (uint8_t i = 0; i < 2; i++) {
    sHave[i] = p.getBool(kKeyHave[i], false);
    sValue[i] = p.getInt(kKeyVal[i], 0);
  }
  p.end();
}

bool hasValue(uint8_t slot) { return slot < 2 && sHave[slot]; }
int valueLb(uint8_t slot) { return (slot < 2) ? sValue[slot] : 0; }

void store(uint8_t slot, int lb) {
  if (slot >= 2) return;
  sHave[slot] = true;
  sValue[slot] = lb;
  Preferences p;
  p.begin(kNamespace, /*readOnly=*/false);
  p.putBool(kKeyHave[slot], true);
  p.putInt(kKeyVal[slot], lb);
  p.end();
  Serial.printf("[wpresets] slot %u -> %d lb\n", (unsigned)slot, lb);
}

} // namespace wpresets
