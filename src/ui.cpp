#include <Preferences.h>
#include <WiFi.h>
#include <cmath>
#include "ui.h"
#include "voltra_protocol.h"
#include "wifi_provision.h"
#include "prefs.h"
#include "weight_presets.h"
#include "haptics.h"
#include "version.h"
#include "gfx_aa.h"
#include "ota_update.h"
#include "fonts/FreeSansBold51pt7b.h" // big weight value -- digits/sign only (0x20-0x39)
#include "fonts/FreeSansBold15pt7b.h" // mode label, button label (full ASCII, bold)
#include "fonts/FreeSans12pt7b.h"     // reps/status line, battery% (full ASCII, regular)
#include "fonts/FreeSans9pt7b.h"      // connection status label (full ASCII, regular, smaller)

namespace ui {

enum class Mode { Base, Eccentric, Chains };

// Workout is the normal dial screen (everything this file originally drew).
// Everything else is the settings menu tree this button opens: Settings is
// the scrollable top-level list (see kSettingsItems below), and the rest
// are its sub-screens -- one per row in that list. Kept as one flat enum
// rather than a stack, since the tree is only ever two levels deep -- each
// sub-screen's own "back" always goes to Settings, and Settings' own "back"
// always goes to Workout (see the touch dispatch in update() below), so
// there's never an actual need to remember more than "which screen am I on".
enum class Screen {
  Workout,
  Settings,
  Theme,
  GaugeStyle,
  Wifi,
  Bluetooth,
  Twin,
  Brightness,
  AutoDim,
  StepSize,
  Vibration,
  Presets,
  Units,
  DropMode,
  DropCount,
  DropPercent,
  DropInterval,
  About,
  FirmwareUpdate,
  FactoryReset,
};

static Arduino_Canvas *sCanvas = nullptr;
static Mode sMode = Mode::Base;
static Screen sScreen = Screen::Workout;
static bool sPrevTouched = false;

// Hold-to-arm gesture state for the engage/disengage button. There's no
// physical button on this board's knob (see encoder.h), so "tap to load,
// hold to arm auto-load" -- the reference implementation's own button
// behavior -- is done as a touchscreen press-and-hold instead of a level
// switch. Tap fires on release (so we can tell a tap from a hold before
// acting); hold fires once, mid-press, once HOLD_MS has elapsed.
static constexpr uint32_t kHoldMs = 600;
static bool sPressActive = false; // true while a press that started inside the engage zone is ongoing
static uint32_t sPressStartMs = 0;
static bool sHoldFired = false;

// Same tap-vs-hold classifier, independently tracked, for the chains
// accessory bubble: a tap focuses its shared weight amount for the knob to
// edit (like any other bubble), a hold instead cycles which style
// (Chains/Inverse/Mountain) that amount currently produces -- see
// VoltraClient::setChainsStyle()'s comment. A separate set of state
// variables rather than reusing sPressActive/sPressStartMs/sHoldFired above:
// a touch can only ever be inside one zone at a time (the two are gated by
// mutually-exclusive inRect() checks in update() below), so this never
// tracks two presses at once, but keeping them apart means a change to one
// gesture's timing/reset logic can't silently affect the other's.
static bool sChainsPressActive = false;
static uint32_t sChainsPressStartMs = 0;
static bool sChainsHoldFired = false;

// Same tap-vs-hold classifier again, once per weight-preset slot (see
// kPresetZone0/kPresetZone1 below and weight_presets.h): a tap recalls that
// slot's stored number into whichever value is currently focused, a hold
// instead stores the currently-focused value into that slot. Two full sets
// of state rather than one pair of arrays indexed some other way would be
// equivalent; arrays here since there are two identical slots (unlike the
// engage button and chains bubble above, which are each one-of-a-kind).
static bool sPresetPressActive[2] = {false, false};
static uint32_t sPresetPressStartMs[2] = {0, 0};
static bool sPresetHoldFired[2] = {false, false};

// ---------------------------------------------------------------------------
// Layout. The panel is a 360x360 ROUND display -- the glass/bezel eats into
// the corners of the square framebuffer, so anything drawn out near the
// literal pixel edges gets visually clipped even though it's technically
// inside the 360x360 canvas (this bit us: the old hint text at y=349 and the
// battery badge pinned to the top-right corner were both past the visible
// circle). Every element below is placed to stay inside kSafeR pixels of the
// center, which was sized empirically (a good chunk inside the true 180px
// panel radius) to leave margin for the bezel. If you add new UI elements,
// keep them inside that radius -- check the corner farthest from center for
// rects, not just the middle of an edge.
static constexpr int16_t kCx = 180;
static constexpr int16_t kCy = 180;
static constexpr int16_t kSafeR = 172;
static constexpr int16_t kBezelR = 178;

// Instrument-dial tick ring, just inside the workout-state ring above --
// a generic tick-mark gauge face (the same idiom as a car speedometer or an
// oven dial, not a copy of any one product's screen), showing at a glance
// how far the currently-selected mode's dial value sits within its valid
// range. 60 ticks (one every 6 degrees), every 5th one longer, mirroring
// the common major/minor tick convention on analog gauges.
//
// It does NOT go all the way around: this panel already has real content
// hugging both the top (the status row, whose worst-case width -- originally
// "CONNECTED", now the shorter "LINKED"/"LINKING" connLabelMain() swaps in
// there (see its own comment) -- plus a 3-digit battery percent -- was
// measured, same offline getTextBounds() technique used elsewhere in this
// file, to reach as far as radius ~178-186 in its widest corners; the
// shorter label only ever makes this narrower, so that radius still holds
// as an upper bound) and the bottom (the hint text below
// the START/STOP button, and that button's own bottom corners, reach
// radius ~159-168). A full 360 degree ring at a radius big enough to look
// like a real gauge (kTickOuterR below) would run straight through both, so
// there's a deliberate gap at each: roughly 70 degrees centered on top,
// 60 degrees centered on bottom, leaving two ~115 degree arcs on the left
// and right. The right arc still passes behind the theme-cycle icon
// (kThemeIconCx/Cy/R below) at the same angle, but that icon's own visible
// radius (~130-150) is comfortably inside kTickInnerMinor (163), so the two
// don't actually overlap -- checked the same way, not just assumed.
static constexpr int16_t kTickOuterR = 170;
static constexpr int16_t kTickInnerMinor = kTickOuterR - 7;  // minor tick length
static constexpr int16_t kTickInnerMajor = kTickOuterR - 13; // major tick length
static constexpr int kTickCount = 60;
static constexpr int kTickMajorEvery = 5;
static constexpr float kPi = 3.14159265358979323846f;
// Gap angles in the same convention used to draw the ticks (0 degrees =
// due east/3 o'clock, increasing clockwise, wrapped to [0,360)).
static constexpr float kTickTopGapLo = 235.0f, kTickTopGapHi = 305.0f;
static constexpr float kTickBottomGapLo = 60.0f, kTickBottomGapHi = 120.0f;
static bool tickInGap(float angleDeg) {
  return (angleDeg >= kTickTopGapLo && angleDeg <= kTickTopGapHi) ||
         (angleDeg >= kTickBottomGapLo && angleDeg <= kTickBottomGapHi);
}

// Solid-fill arc endpoints, in Arduino_GFX::fillArc()'s own angle convention.
// Confirmed (not assumed) by reading writeFillArcHelper's geometry in the
// installed library and cross-checking with a from-scratch Python port
// rendered to a PNG (see the antialiasing verification note above -- same
// technique, applied here since there's no direct hardware access from this
// session either): fillArc uses the exact same frame already documented
// above for angleNorm -- 0 degrees = due east, increasing clockwise -- so
// the visible arcs' fillArc() start/end are simply the gap boundaries
// themselves, no conversion needed. fillArc(cx, cy, r1, r2, 305, 60, color)
// draws precisely the right-hand 115-degree arc; no separate "reversed arc"
// handling is needed on our end since the library normalizes and wraps the
// angles itself.
static constexpr float kArcRightStartDeg = kTickTopGapHi;   // 305 -- just past the top gap
static constexpr float kArcRightEndDeg = kTickBottomGapLo;  // 60  -- just before the bottom gap
static constexpr float kArcLeftStartDeg = kTickBottomGapHi; // 120 -- just past the bottom gap
static constexpr float kArcLeftEndDeg = kTickTopGapLo;      // 235 -- just before the top gap
static constexpr float kArcRightSpan = 115.0f; // kArcRightEndDeg - kArcRightStartDeg, wrapped through 0/360
static constexpr float kArcLeftSpan = 115.0f;  // kArcLeftEndDeg - kArcLeftStartDeg
static constexpr float kArcTotalSpan = kArcRightSpan + kArcLeftSpan; // 230 degrees of visible track

// Maps a visible tick's angle to its position (0..kArcTotalSpan) along the
// combined visible track -- walking the right arc first (through due east),
// then the left arc -- so a single linear "how much is filled" comparison
// works across both arcs. Only meaningful where tickInGap() is false.
static float tickTrackPos(float angleNorm) {
  if (angleNorm >= kArcRightStartDeg) return angleNorm - kArcRightStartDeg;
  if (angleNorm <= kArcRightEndDeg) return (angleNorm + 360.0f) - kArcRightStartDeg;
  return kArcRightSpan + (angleNorm - kArcLeftStartDeg);
}

// Touch tap zones, in display pixel coordinates (0,0 top-left, 360x360).
// If taps land on the wrong thing, your CST816's raw X/Y are probably
// swapped or mirrored relative to the panel rotation -- adjust the mapping
// in mapTouch() below rather than these zones.
struct Rect { int16_t x, y, w, h; };
// Two accessory bubbles replace what used to be a single MODE pill here --
// same 216px-wide, x=72..288 footprint the pill occupied (so every distance
// comment below that was measured against "the MODE pill" is still
// geometrically accurate), just split into two independent tap targets
// instead of one 3-way cycle. See drawAccessoryBubble() and the touch
// handling in update() for how tapping either one now works.
static constexpr int16_t kBubbleGap = 16, kBubbleW = 100, kBubbleH = 46;
// Chains/Eccentric are now drawn as circular ring-dials stacked under the
// big weight value (see drawModifierRing()) rather than rectangular bubbles
// -- matching the look of macfitnesscoaching.com's Voltra Remote guide,
// which Mike asked to match. kBubbleW/kBubbleGap above are kept only for
// the weight-preset pills (kPresetZone0/1 below), which did not move.
// Square hit-zones bounding each ring (not circular hit-testing -- a
// square around a circle is simpler and plenty accurate for a touch
// target this size); inRect() below doesn't care that the drawn shape
// inside is round.
static constexpr int16_t kModRingR = 28, kModRingCy = 198;
static constexpr int16_t kModRingLeftCx = 122, kModRingRightCx = 238;
static const Rect kEccBubbleZone    = {kModRingLeftCx - kModRingR, kModRingCy - kModRingR, kModRingR * 2, kModRingR * 2};
static const Rect kChainsBubbleZone = {kModRingRightCx - kModRingR, kModRingCy - kModRingR, kModRingR * 2, kModRingR * 2};
// The big weight value's own tap zone -- tapping it while a modifier
// (Eccentric/Chains) is focused returns focus to base weight, a more
// discoverable way back than the original design's only path (tap the SAME
// ring again to toggle back off -- still supported, unchanged, just easy to
// miss since nothing about re-tapping the thing you just focused reads as
// "go back"). Bounds checked against the big value's own worst-case glyph
// metrics (FreeSansBold51pt7b, "-999"/"+999", see that font's own
// measurement elsewhere in this file): centered at (kCx, 116) with x
// reaching out to ~287 at most and y spanning roughly 87..145, so this rect
// (x=70..290, y=82..150) covers it with margin on every side while staying
// clear of kSettingsTapZone just below/right (y>=155) and the modifier
// rings further down (kModRingCy-kModRingR = 170).
static const Rect kBigValueZone = {70, 82, 220, 68};
static const Rect kEngageZone = {105, 276, 150, 44};
// Weight-preset pills -- same x=72/188 columns and 100px width as the
// accessory bubbles above (kBubbleW/kBubbleGap), for the same visual-column
// alignment reason kEccBubbleZone/kChainsBubbleZone call out, just a
// shorter box: this row sits in the gap between the "LB"/"KG" unit tag
// (baseline y=228) and kEngageZone above (y=276), which only leaves about
// 40px of clearance, not the 46px the accessory bubbles get up top. Only
// drawn/interactive while the status/reps row below would otherwise be
// blank -- see draw()'s status-row comment and update()'s handling below.
static constexpr int16_t kPresetH = 36;
static const Rect kPresetZone0 = {72, 236, kBubbleW, kPresetH};
static const Rect kPresetZone1 = {72 + kBubbleW + kBubbleGap, 236, kBubbleW, kPresetH};
// Settings-menu gear button (opens Screen::Settings). This used to BE the
// theme-cycle button directly -- tapping it toggled the color theme in
// place -- but once a real settings menu existed, it made more sense for
// this same well-tested spot to open that menu instead, with "Theme" as one
// row inside it, than to keep a single-purpose control here and add a
// second icon for Wi-Fi right next to it.
//
// The genuinely unused space on this display is off to the SIDE of the
// centered content column, at the panel's vertical middle -- a round panel
// is widest there (the safe circle's full chord), while the centered
// column (weight value, modifier rings, status line, START/STOP button)
// is comparatively narrow, so there's real dead space to its right that
// nothing else ever touches. Positioned level with the big weight value
// (kCy=180), clear of its worst-case width ("-999"/"+999" measured to
// reach x=287 at most, same offline getTextBounds() technique used
// elsewhere in this file) and of the Chains ring-dial further down
// (kModRingRightCx=238, kModRingR=28, reaching x=266 at most).
//
// x=320 (28deg) originally left a good gap on both of those checks but
// was never checked against the main tick-ring gauge itself
// (kTickOuterR=170, drawn from this same kCx/kCy) -- an oversight from
// when the gauge was still a full ring with no ring-dial modifiers nearby
// to prompt the comparison. At x=320 the gear's own farthest tooth tip
// (320-180=140 from center, +kGearToothOuterR=14 -> 154) came within 3px
// of the gauge's own inner edge (kTickOuterR-13=157, the band Solid Arc's
// base track fills) -- close enough to visually read as overlapping on
// real hardware, which is what prompted moving it to x=310 instead:
// 310-180=130, +14=144, a 13px gap from that same 157 edge, while still
// comfortably clearing both checks above (296 vs the weight value's 287,
// and 296 vs the chains ring's 266).
static constexpr int16_t kGearCx = 310, kGearCy = 180, kGearR = 9;
static constexpr int16_t kGearToothOuterR = kGearR + 5; // how far each gear tooth reaches past the body
// The touch hit-test uses this larger rect around the icon, not just its
// small drawn footprint -- see the accessory-bubble/kEngageZone sizes above for
// why (a small icon-sized zone was far smaller than either of this UI's
// other two tap targets and real taps were missing). Plenty of real
// clearance on every side here (23px to the weight value's worst-case edge,
// 45+px to both the accessory bubbles and the START/STOP button) -- unlike the
// zero-gap mistake from the very first version of this zone, which caused
// real accidental mode toggles on real hardware.
static const Rect kSettingsTapZone = {295, 155, 50, 50};

// ---------------------------------------------------------------------------
// Settings menu tree layout (Screen::Settings/Theme/Wifi). All three reuse
// kEngageZone's exact rect/rounded-pill look for their own "go back" (or,
// while Wi-Fi provisioning is active, "cancel") button -- see kBackZone's
// comment below for why that's a deliberate reuse rather than a new rect.
static const Rect &kBackZone = kEngageZone;

// Settings is a scrollable focused-list (see kSettingsItems below) rather
// than fixed per-item rows -- with 10 items it no longer fits as one row
// per item on this small round display. The encoder scrolls a "focused"
// index through the flat list; only this one centered row is a real tap
// target (tapping it opens that item's sub-screen), while neighboring
// items are drawn as dimmed preview text above/below with no tap zone of
// their own -- see drawSettingsScreen().
static const Rect kSettingsFocusZone = {72, 155, 216, 50};

// Generic prev/next chevron zones, originally built for the Theme
// sub-screen's color cycle. Reused as-is (same rect objects, not copies)
// by every other "cycle through a small preset list" sub-screen this file
// has grown since -- Brightness, Auto-Dim, Step Size, Vibration, Units --
// via drawCycleScreen() below. All of these screens are mutually exclusive
// (sScreen is never more than one of them at once), so sharing one pair of
// rects/tap zones across all of them is the same reuse idiom kBackZone
// above already uses for kEngageZone.
static const Rect kThemePrevZone = {40, 150, 60, 60};
static const Rect kThemeNextZone = {260, 150, 60, 60};

// Generic single-action-button zone, originally built for Wi-Fi's
// "ADD WIFI"/"FORGET WIFI" button. Reused the same way as kThemePrevZone/
// kThemeNextZone above by every other single-action sub-screen -- Bluetooth
// ("RECONNECT") and Factory Reset ("RESET"/"TAP AGAIN?") -- since those are
// also all mutually exclusive with Wi-Fi and with each other.
static const Rect kWifiActionZone = {72, 180, 216, 46};

static bool inRect(const Rect &r, int16_t x, int16_t y) {
  return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// CST816 reports raw touch-controller coordinates, which are not always in
// the same orientation as the framebuffer. Start with a passthrough; if
// tapping the top of the screen registers as the bottom (or left/right is
// swapped), that's controller-vs-panel rotation mismatch -- try swapping x
// and y, or replacing `v` with `360 - v` here, until taps land where you
// touched.
static void mapTouch(int16_t rawX, int16_t rawY, int16_t &x, int16_t &y) {
  x = rawX;
  y = rawY;
}

// ---------------------------------------------------------------------------
// Color themes. Each theme is a full palette (background/card/text hierarchy
// plus its own "armed"/"stop" alert colors), not just one swapped hue --
// text/UI hierarchy within a theme is done with brightness of that theme's
// one main hue (bright = primary/active, dimmer = secondary, dimmest = faint
// captions/borders), the way a classic single-color LCD does it. Amber/red
// are normally kept as genuine alert colors that break the monochrome look
// on purpose for "armed" and stop/critical-battery, so those two states
// still pop against the primary palette -- except where a theme's own
// primary hue already IS amber or red, in which case that theme picks a
// different warn/danger color so "armed"/"stop" don't blend into the normal
// active-text color (see the Warm Amber and Red entries below).
//
// Index 0 (Ice Blue) is the original look and what a fresh board with no
// saved preference yet starts on -- see loadTheme()/cycleTheme() below for
// how the active one is chosen and persisted.
struct ThemeColors {
  const char *name; // shown on the Settings/Theme screens, and logged on change
  uint16_t bg, card, cardBorder, textPrimary, textMuted, textFaint, accent, warn, danger;
};

static const ThemeColors kThemes[] = {
  { // 0: Ice Blue -- cool white/ice-blue monochrome-LCD look (the original palette).
    "Ice Blue",
    RGB565(4, 6, 10), RGB565(14, 20, 28), RGB565(38, 50, 66),
    RGB565(210, 230, 255), RGB565(130, 160, 195), RGB565(70, 88, 110),
    RGB565(210, 230, 255), RGB565(255, 200, 70), RGB565(255, 80, 70),
  },
  { // 1: Warm Amber -- classic amber CRT/LCD look. Primary text is already
    // amber, so "armed" uses a more orange-leaning warn color instead of the
    // usual amber to stay visually distinct from it.
    "Warm Amber",
    RGB565(10, 6, 2), RGB565(28, 18, 8), RGB565(66, 46, 20),
    RGB565(255, 190, 90), RGB565(195, 140, 70), RGB565(110, 78, 40),
    RGB565(255, 190, 90), RGB565(255, 130, 30), RGB565(255, 80, 70),
  },
  { // 2: Green Phosphor -- classic green terminal/monochrome-LCD look.
    "Green Phosphor",
    RGB565(2, 8, 4), RGB565(10, 26, 14), RGB565(30, 66, 38),
    RGB565(140, 255, 150), RGB565(90, 195, 105), RGB565(50, 110, 60),
    RGB565(140, 255, 150), RGB565(255, 200, 70), RGB565(255, 80, 70),
  },
  { // 3: Grayscale -- neutral white-on-black, no color tint.
    "Grayscale",
    RGB565(6, 6, 6), RGB565(22, 22, 22), RGB565(60, 60, 60),
    RGB565(235, 235, 235), RGB565(170, 170, 170), RGB565(95, 95, 95),
    RGB565(235, 235, 235), RGB565(255, 200, 70), RGB565(255, 80, 70),
  },
  { // 4: Red -- dim red-on-black, easier on the eyes/night vision in a dark
    // room. Primary text is already red, so "stop"/critical uses white here
    // instead of red -- the one theme where the usual red danger color would
    // be invisible against the theme's own text.
    "Red",
    RGB565(8, 2, 2), RGB565(26, 8, 8), RGB565(66, 20, 20),
    RGB565(255, 90, 80), RGB565(195, 70, 62), RGB565(110, 40, 35),
    RGB565(255, 90, 80), RGB565(255, 200, 70), RGB565(255, 255, 255),
  },
  { // 5: Cyan -- electric-cyan monochrome, distinct from Ice Blue's
    // cooler blue-white (Ice Blue's primary leans blue; this leans fully
    // toward green-cyan). Default warn/danger both read fine against it.
    "Cyan",
    RGB565(2, 8, 8), RGB565(8, 26, 26), RGB565(24, 66, 66),
    RGB565(90, 255, 245), RGB565(60, 195, 188), RGB565(35, 110, 106),
    RGB565(90, 255, 245), RGB565(255, 200, 70), RGB565(255, 80, 70),
  },
  { // 6: Violet -- purple monochrome. Default warn/danger both read fine
    // against it (neither amber nor red is close to this hue).
    "Violet",
    RGB565(8, 4, 10), RGB565(24, 12, 30), RGB565(60, 32, 74),
    RGB565(190, 140, 255), RGB565(150, 108, 200), RGB565(85, 62, 115),
    RGB565(190, 140, 255), RGB565(255, 200, 70), RGB565(255, 80, 70),
  },
  { // 7: Hot Pink -- magenta monochrome. Primary is pink, not red, so the
    // default red danger color (pure red-orange, no blue) still reads as a
    // distinct alert against it rather than blending in the way it would
    // for the actual Red theme above.
    "Hot Pink",
    RGB565(10, 2, 6), RGB565(30, 8, 18), RGB565(74, 22, 46),
    RGB565(255, 90, 180), RGB565(200, 70, 142), RGB565(115, 40, 82),
    RGB565(255, 90, 180), RGB565(255, 200, 70), RGB565(255, 80, 70),
  },
};
static constexpr uint8_t kThemeCount = sizeof(kThemes) / sizeof(kThemes[0]);

// The "current" palette. Deliberately mutable (not constexpr, despite still
// looking like the single fixed palette this replaced) so applyTheme() can
// repoint every draw()/connColor() color reference at a different theme's
// values without having to touch any of the many call sites elsewhere in
// this file that already reference these by name.
static uint16_t kBg, kCard, kCardBorder, kTextPrimary, kTextMuted, kTextFaint, kAccent, kWarn, kDanger;
// Settings list's focused-row highlight -- a dim blend of kBg/kDanger
// (recomputed whenever the theme changes, see applyTheme() below) rather
// than a fixed color, so it still reads as "the alert/selected hue" in
// every theme instead of literally always being red regardless of theme.
// Matches macfitnesscoaching.com's Voltra Remote guide, which draws its
// settings screens' selected row as a full-width dark-red band rather than
// this project's previous plain kCard-colored outline box.
static uint16_t kSettingsHighlight;

// Blends two RGB565 colors: t=0 returns a, t=1 returns b. Used only to
// derive kSettingsHighlight above from each theme's own bg/danger colors,
// so the blend adapts per-theme instead of hardcoding one literal color.
static uint16_t blendColor565(uint16_t a, uint16_t b, float t) {
  uint8_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  uint8_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  uint8_t rr = (uint8_t)lroundf(ar + (br - ar) * t);
  uint8_t rg = (uint8_t)lroundf(ag + (bg - ag) * t);
  uint8_t rb = (uint8_t)lroundf(ab + (bb - ab) * t);
  return (uint16_t)((rr << 11) | (rg << 5) | rb);
}

static void applyTheme(uint8_t idx) {
  if (idx >= kThemeCount) idx = 0;
  const ThemeColors &t = kThemes[idx];
  kBg = t.bg; kCard = t.card; kCardBorder = t.cardBorder;
  kTextPrimary = t.textPrimary; kTextMuted = t.textMuted; kTextFaint = t.textFaint;
  kAccent = t.accent; kWarn = t.warn; kDanger = t.danger;
  kSettingsHighlight = blendColor565(kBg, kDanger, 0.35f);
}

// Persisted theme choice, in NVS flash via the Preferences library. Like
// WiFi/ArduinoOTA elsewhere in this project, Preferences ships bundled with
// the Arduino-ESP32 core itself, so no lib_deps entry is needed for it.
// Persisting means the theme you land on survives a reboot or battery-off
// instead of always starting back on Ice Blue.
static constexpr const char *kPrefsNamespace = "voltra_ui";
static constexpr const char *kPrefsThemeKey = "theme";
static uint8_t sThemeIndex = 0;

static void loadTheme() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/true);
  sThemeIndex = prefs.getUChar(kPrefsThemeKey, 0);
  prefs.end();
  if (sThemeIndex >= kThemeCount) sThemeIndex = 0; // guard against a stale index from a previous build with fewer themes
  applyTheme(sThemeIndex);
}

static void setThemeIndex(uint8_t idx) {
  if (idx >= kThemeCount) idx = 0;
  sThemeIndex = idx;
  applyTheme(sThemeIndex);
  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/false);
  prefs.putUChar(kPrefsThemeKey, sThemeIndex);
  prefs.end();
  // Cheap confirmation on the serial monitor -- if a tap on the theme
  // prev/next button ever looks like it does nothing on screen, this line
  // tells you whether the tap was even detected (i.e. whether to go looking
  // at the touch hit zone vs. the drawing code).
  Serial.printf("[ui] theme -> %u (%s)\n", sThemeIndex, kThemes[sThemeIndex].name);
}
static void cycleTheme() { setThemeIndex((sThemeIndex + 1) % kThemeCount); }
static void prevTheme() { setThemeIndex((sThemeIndex + kThemeCount - 1) % kThemeCount); }

// Gauge style: which shape the workout dial's progress indicator draws in,
// independent of Theme's colors above (this picks the shape, Theme picks
// the palette it's drawn in -- any style x any theme combination works,
// since every style below still just draws in kAccent/kCardBorder/kBg).
// Same persisted-preset idiom as Theme (own NVS key, own load/cycle
// functions), and drawn with the same generic drawCycleScreen() plus a
// live shape preview instead of a color swatch -- see drawGaugeStyleAt()
// and drawGaugeStyleScreen() below.
struct GaugeStyleInfo { const char *name; };
static const GaugeStyleInfo kGaugeStyles[] = {
  {"Solid Arc"},     // 0: continuous filled band -- the original/default look.
  {"Lit Ticks"},     // 1: individual tick marks light up and thicken as the
                     // value rises, instead of a continuous band.
  {"Segmented"},     // 2: chunky discrete blocks, like a VU meter/equalizer,
                     // instead of a smooth band or thin ticks.
  {"Gradient Arc"},  // 3: like Solid Arc, but the lit band blends from
                     // kAccent toward kWarn as it nears the top of the range.
  {"Dot Ring"},      // 4: a ring of discrete lit/unlit dots at the major
                     // tick positions, instead of a band or thin ticks.
  {"Pointer"},       // 5: a single marker at the current position on the
                     // track, instead of a cumulative fill.
};
static constexpr uint8_t kGaugeStyleCount = sizeof(kGaugeStyles) / sizeof(kGaugeStyles[0]);
static constexpr const char *kPrefsGaugeStyleKey = "gaugeStyle";
static uint8_t sGaugeStyleIndex = 0;

static void loadGaugeStyle() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/true);
  sGaugeStyleIndex = prefs.getUChar(kPrefsGaugeStyleKey, 0);
  prefs.end();
  if (sGaugeStyleIndex >= kGaugeStyleCount) sGaugeStyleIndex = 0; // guard a stale index from a build with fewer styles
}

static void setGaugeStyleIndex(uint8_t idx) {
  if (idx >= kGaugeStyleCount) idx = 0;
  sGaugeStyleIndex = idx;
  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/false);
  prefs.putUChar(kPrefsGaugeStyleKey, sGaugeStyleIndex);
  prefs.end();
  Serial.printf("[ui] gauge style -> %u (%s)\n", sGaugeStyleIndex, kGaugeStyles[sGaugeStyleIndex].name);
}
static void cycleGaugeStyle() { setGaugeStyleIndex((sGaugeStyleIndex + 1) % kGaugeStyleCount); }
static void prevGaugeStyle() { setGaugeStyleIndex((sGaugeStyleIndex + kGaugeStyleCount - 1) % kGaugeStyleCount); }

// Drop sets: auto-lowers the base weight by a configured percent, either
// after a set number of reps ("Rep Target") or when you let the cable set
// down between sets ("Set Down") -- see updateSetTracking()'s comment
// (further below, near the rest-timer/set-count tracking it shares a
// trigger with) for how each type is actually detected and applied.
// Matches the feature by the same name in macfitnesscoaching.com's Voltra
// Remote guide, simplified to a uniform percent/interval across every drop
// (that guide's own version lets each of up to 4 drops have its own
// separate reps-threshold and percent) -- a full per-drop editor is a much
// bigger, riskier UI to ship untested on real hardware than reusing this
// project's existing proven "persisted index into a small named list, with
// prev/next chevrons" idiom (the same one Theme/Gauge Style/Step Size/etc.
// above already use) four more times for Mode/Count/Percent/Interval.
struct DropModeInfo { const char *name; };
static const DropModeInfo kDropModes[] = {
  {"OFF"},
  {"REP TARGET"}, // drop every kDropIntervals[sDropIntervalIndex] reps
  {"SET DOWN"},   // drop each time the cable is set down between sets
};
static constexpr uint8_t kDropModeCount = sizeof(kDropModes) / sizeof(kDropModes[0]);
static constexpr const char *kPrefsDropModeKey = "dropMode";
static uint8_t sDropModeIndex = 0;

static void loadDropMode() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/true);
  sDropModeIndex = prefs.getUChar(kPrefsDropModeKey, 0);
  prefs.end();
  if (sDropModeIndex >= kDropModeCount) sDropModeIndex = 0;
}
static void setDropModeIndex(uint8_t idx) {
  if (idx >= kDropModeCount) idx = 0;
  sDropModeIndex = idx;
  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/false);
  prefs.putUChar(kPrefsDropModeKey, sDropModeIndex);
  prefs.end();
}
static void cycleDropMode() { setDropModeIndex((sDropModeIndex + 1) % kDropModeCount); }
static void prevDropMode() { setDropModeIndex((sDropModeIndex + kDropModeCount - 1) % kDropModeCount); }

static constexpr uint8_t kDropCounts[] = {0, 1, 2, 3, 4};
static constexpr uint8_t kDropCountOptionCount = sizeof(kDropCounts) / sizeof(kDropCounts[0]);
static constexpr const char *kPrefsDropCountKey = "dropCount";
static uint8_t sDropCountIndex = 2; // default 2 drops, once a mode is turned on

static void loadDropCount() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/true);
  sDropCountIndex = prefs.getUChar(kPrefsDropCountKey, 2);
  prefs.end();
  if (sDropCountIndex >= kDropCountOptionCount) sDropCountIndex = 2;
}
static void setDropCountIndex(uint8_t idx) {
  if (idx >= kDropCountOptionCount) idx = 0;
  sDropCountIndex = idx;
  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/false);
  prefs.putUChar(kPrefsDropCountKey, sDropCountIndex);
  prefs.end();
}
static void cycleDropCount() { setDropCountIndex((sDropCountIndex + 1) % kDropCountOptionCount); }
static void prevDropCount() { setDropCountIndex((sDropCountIndex + kDropCountOptionCount - 1) % kDropCountOptionCount); }

static constexpr uint8_t kDropPercents[] = {5, 10, 15, 20, 25, 30, 40, 50};
static constexpr uint8_t kDropPercentOptionCount = sizeof(kDropPercents) / sizeof(kDropPercents[0]);
static constexpr const char *kPrefsDropPercentKey = "dropPct";
static uint8_t sDropPercentIndex = 3; // default 20%

static void loadDropPercent() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/true);
  sDropPercentIndex = prefs.getUChar(kPrefsDropPercentKey, 3);
  prefs.end();
  if (sDropPercentIndex >= kDropPercentOptionCount) sDropPercentIndex = 3;
}
static void setDropPercentIndex(uint8_t idx) {
  if (idx >= kDropPercentOptionCount) idx = 0;
  sDropPercentIndex = idx;
  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/false);
  prefs.putUChar(kPrefsDropPercentKey, sDropPercentIndex);
  prefs.end();
}
static void cycleDropPercent() { setDropPercentIndex((sDropPercentIndex + 1) % kDropPercentOptionCount); }
static void prevDropPercent() { setDropPercentIndex((sDropPercentIndex + kDropPercentOptionCount - 1) % kDropPercentOptionCount); }

// Only meaningful for DropMode::REP_TARGET (index 1) -- how many reps
// between each drop (cumulative: with interval 8, drops land after rep 8,
// 16, 24, ...). Ignored for SET DOWN, which drops on the idle/rest boundary
// instead -- see updateSetTracking().
static constexpr uint8_t kDropIntervals[] = {2, 4, 6, 8, 10, 12, 15, 20};
static constexpr uint8_t kDropIntervalOptionCount = sizeof(kDropIntervals) / sizeof(kDropIntervals[0]);
static constexpr const char *kPrefsDropIntervalKey = "dropEvery";
static uint8_t sDropIntervalIndex = 3; // default every 8 reps

static void loadDropInterval() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/true);
  sDropIntervalIndex = prefs.getUChar(kPrefsDropIntervalKey, 3);
  prefs.end();
  if (sDropIntervalIndex >= kDropIntervalOptionCount) sDropIntervalIndex = 3;
}
static void setDropIntervalIndex(uint8_t idx) {
  if (idx >= kDropIntervalOptionCount) idx = 0;
  sDropIntervalIndex = idx;
  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/false);
  prefs.putUChar(kPrefsDropIntervalKey, sDropIntervalIndex);
  prefs.end();
}
static void cycleDropInterval() { setDropIntervalIndex((sDropIntervalIndex + 1) % kDropIntervalOptionCount); }
static void prevDropInterval() { setDropIntervalIndex((sDropIntervalIndex + kDropIntervalOptionCount - 1) % kDropIntervalOptionCount); }

// Renders one style of the workout dial's progress indicator at the given
// center/radius. Shared by the real workout screen (full size, at
// kCx/kCy/kTickOuterR) and the Gauge Style settings screen's live preview
// (smaller, so switching styles there shows the actual shape rather than
// just a name -- same reasoning as Theme's color swatch). innerMinor/
// innerMajor are derived from outerR with the same proportions the
// original fixed constants used (delta 7/13 out of a 170 outer radius),
// rounded for whatever size is passed in.
//
// All three styles fill by angular position along the actual visible track
// (tickTrackPos()/kArcTotalSpan), not by raw tick-slot index -- an earlier
// version of the tick-based look compared against loop index instead, which
// ran over the ~22 slots that fall in the gaps and are skipped without
// being drawn, so the right arc filled disproportionately fast relative to
// the true value fraction (at frac=0.5 the entire right arc was already
// full while the left arc hadn't started). Track-position comparison makes
// every style advance linearly with frac across both visible arcs,
// matching what each visually promises.
static void drawGaugeStyleAt(int16_t cx, int16_t cy, int16_t outerR, float frac, uint8_t style) {
  int16_t innerMinor = outerR - (int16_t)lroundf(outerR * 7.0f / 170.0f);
  int16_t innerMajor = outerR - (int16_t)lroundf(outerR * 13.0f / 170.0f);

  float fillDeg = frac * kArcTotalSpan;
  float rightFillDeg = fillDeg < kArcRightSpan ? fillDeg : kArcRightSpan;
  float leftFillDeg = fillDeg > kArcRightSpan ? (fillDeg - kArcRightSpan) : 0.0f;
  if (leftFillDeg > kArcLeftSpan) leftFillDeg = kArcLeftSpan;

  if (style == 1) {
    // Lit Ticks: every visible tick drawn individually -- dim/thin while
    // unlit, accent-colored and thickened once the fill has passed it.
    // Thickening uses the same perpendicular-offset technique as the
    // Settings-gear teeth below (see that comment for why a single 1px
    // radial line reads as spindly) so lit ticks stand out by more than
    // color alone.
    for (int i = 0; i < kTickCount; i++) {
      float angleDeg = -90.0f + (360.0f * i) / kTickCount;
      float angleNorm = angleDeg < 0 ? angleDeg + 360.0f : angleDeg;
      if (tickInGap(angleNorm)) continue;
      bool major = (i % kTickMajorEvery) == 0;
      int16_t innerR = major ? innerMajor : innerMinor;
      float rad = angleDeg * kPi / 180.0f;
      float c = cosf(rad), s = sinf(rad);
      int16_t x0 = cx + (int16_t)(c * outerR);
      int16_t y0 = cy + (int16_t)(s * outerR);
      int16_t x1 = cx + (int16_t)(c * innerR);
      int16_t y1 = cy + (int16_t)(s * innerR);
      bool lit = tickTrackPos(angleNorm) < fillDeg;
      uint16_t color = lit ? kAccent : kCardBorder;
      gfxaa::drawLineAA(sCanvas, x0, y0, x1, y1, color, kBg);
      if (lit) {
        float px = -s, py = c;
        int16_t ox = (int16_t)(px >= 0 ? px + 0.5f : px - 0.5f);
        int16_t oy = (int16_t)(py >= 0 ? py + 0.5f : py - 0.5f);
        if (ox == 0 && oy == 0) { ox = 1; oy = 0; }
        gfxaa::drawLineAA(sCanvas, x0 + ox, y0 + oy, x1 + ox, y1 + oy, color, kBg);
        gfxaa::drawLineAA(sCanvas, x0 - ox, y0 - oy, x1 - ox, y1 - oy, color, kBg);
      }
    }
  } else if (style == 2) {
    // Segmented: each visible arc split into evenly-sized chunky blocks
    // with a small gap between them (a VU-meter/equalizer look), each
    // fully lit or fully dim based on whether the fill has passed its
    // start position -- discrete on/off per block, unlike the smooth Solid
    // Arc or the many thin individual ticks of Lit Ticks.
    constexpr int kSegmentsPerArc = 9;
    constexpr float kSegGapDeg = 3.0f;
    float segSpan = kArcRightSpan / kSegmentsPerArc; // kArcLeftSpan is equal, so this applies to both arcs
    for (int i = 0; i < kSegmentsPerArc; i++) {
      float startOff = i * segSpan;
      float endOff = startOff + segSpan - kSegGapDeg;
      if (endOff <= startOff) endOff = startOff + 1.0f;

      uint16_t rightColor = (startOff < fillDeg) ? kAccent : kCardBorder;
      sCanvas->fillArc(cx, cy, outerR, innerMajor, kArcRightStartDeg + startOff, kArcRightStartDeg + endOff, rightColor);

      float leftTrackPos = kArcRightSpan + startOff;
      uint16_t leftColor = (leftTrackPos < fillDeg) ? kAccent : kCardBorder;
      sCanvas->fillArc(cx, cy, outerR, innerMajor, kArcLeftStartDeg + startOff, kArcLeftStartDeg + endOff, leftColor);
    }
  } else if (style == 3) {
    // Gradient Arc: the same continuous band as Solid Arc, but the lit
    // portion blends from kAccent (just starting to fill) toward kWarn (as
    // it approaches the top of the visible track) instead of staying one
    // flat color -- a quicker "how close to max" read than Solid Arc's
    // single accent color gives. Blended in steps the same size as the
    // tick spacing (360/kTickCount degrees) rather than per-pixel, which
    // keeps the per-frame fillArc() call count in the same ballpark as the
    // tick-notch loops every style already runs.
    sCanvas->fillArc(cx, cy, outerR, innerMajor, kArcRightStartDeg, kArcRightEndDeg, kCardBorder);
    sCanvas->fillArc(cx, cy, outerR, innerMajor, kArcLeftStartDeg, kArcLeftEndDeg, kCardBorder);
    constexpr float kGradStepDeg = 360.0f / kTickCount;
    if (rightFillDeg > 0.0f) {
      for (float off = 0.0f; off < rightFillDeg; off += kGradStepDeg) {
        float segEnd = (off + kGradStepDeg < rightFillDeg) ? off + kGradStepDeg : rightFillDeg;
        uint16_t segColor = blendColor565(kAccent, kWarn, off / kArcTotalSpan);
        sCanvas->fillArc(cx, cy, outerR, innerMajor, kArcRightStartDeg + off, kArcRightStartDeg + segEnd, segColor);
      }
    }
    if (leftFillDeg > 0.0f) {
      for (float off = 0.0f; off < leftFillDeg; off += kGradStepDeg) {
        float segEnd = (off + kGradStepDeg < leftFillDeg) ? off + kGradStepDeg : leftFillDeg;
        float trackPos = kArcRightSpan + off;
        uint16_t segColor = blendColor565(kAccent, kWarn, trackPos / kArcTotalSpan);
        sCanvas->fillArc(cx, cy, outerR, innerMajor, kArcLeftStartDeg + off, kArcLeftStartDeg + segEnd, segColor);
      }
    }
    for (int i = 0; i < kTickCount; i++) {
      float angleDeg = -90.0f + (360.0f * i) / kTickCount;
      float angleNorm = angleDeg < 0 ? angleDeg + 360.0f : angleDeg;
      if (tickInGap(angleNorm)) continue;
      bool major = (i % kTickMajorEvery) == 0;
      int16_t innerR = major ? innerMajor : innerMinor;
      float rad = angleDeg * kPi / 180.0f;
      float c = cosf(rad), s = sinf(rad);
      int16_t x0 = cx + (int16_t)(c * outerR);
      int16_t y0 = cy + (int16_t)(s * outerR);
      int16_t x1 = cx + (int16_t)(c * innerR);
      int16_t y1 = cy + (int16_t)(s * innerR);
      float tPos = tickTrackPos(angleNorm);
      uint16_t localBg = (tPos < fillDeg) ? blendColor565(kAccent, kWarn, tPos / kArcTotalSpan) : kCardBorder;
      gfxaa::drawLineAA(sCanvas, x0, y0, x1, y1, kBg, localBg);
    }
  } else if (style == 4) {
    // Dot Ring: a ring of discrete dots at each major tick position (every
    // kTickMajorEvery-th slot, same slots the other styles mark with a
    // deeper tick) instead of a continuous band or many thin individual
    // ticks -- lit dots are filled solid and larger, unlit dots are small
    // hollow outlines, for a punchier, more minimal look than Lit Ticks or
    // Segmented.
    constexpr int16_t kDotLitR = 6, kDotUnlitR = 3;
    int16_t dotTrackR = (int16_t)((innerMajor + outerR) / 2);
    for (int i = 0; i < kTickCount; i += kTickMajorEvery) {
      float angleDeg = -90.0f + (360.0f * i) / kTickCount;
      float angleNorm = angleDeg < 0 ? angleDeg + 360.0f : angleDeg;
      if (tickInGap(angleNorm)) continue;
      float rad = angleDeg * kPi / 180.0f;
      int16_t dx = cx + (int16_t)(cosf(rad) * dotTrackR);
      int16_t dy = cy + (int16_t)(sinf(rad) * dotTrackR);
      bool lit = tickTrackPos(angleNorm) < fillDeg;
      if (lit) {
        sCanvas->fillCircle(dx, dy, kDotLitR, kAccent);
      } else {
        gfxaa::drawCircleAA(sCanvas, dx, dy, kDotUnlitR, 1.3f, kCardBorder, kBg);
      }
    }
  } else if (style == 5) {
    // Pointer: instead of a growing fill, a single bold marker sits at
    // exactly the current value's position on the track -- the dial reads
    // "where you are" rather than "how much is filled". Confined to the
    // ring's own radial band (innerMinor..outerR, extended a few px either
    // side) rather than reaching toward the screen center -- a true
    // center-hub needle would cut straight across the big weight value and
    // Chains/Eccentric rings that live there on the real workout screen.
    sCanvas->fillArc(cx, cy, outerR, innerMajor, kArcRightStartDeg, kArcRightEndDeg, kCardBorder);
    sCanvas->fillArc(cx, cy, outerR, innerMajor, kArcLeftStartDeg, kArcLeftEndDeg, kCardBorder);
    for (int i = 0; i < kTickCount; i++) {
      float angleDeg = -90.0f + (360.0f * i) / kTickCount;
      float angleNorm = angleDeg < 0 ? angleDeg + 360.0f : angleDeg;
      if (tickInGap(angleNorm)) continue;
      bool major = (i % kTickMajorEvery) == 0;
      int16_t innerR = major ? innerMajor : innerMinor;
      float rad = angleDeg * kPi / 180.0f;
      float c = cosf(rad), s = sinf(rad);
      int16_t x0 = cx + (int16_t)(c * outerR);
      int16_t y0 = cy + (int16_t)(s * outerR);
      int16_t x1 = cx + (int16_t)(c * innerR);
      int16_t y1 = cy + (int16_t)(s * innerR);
      gfxaa::drawLineAA(sCanvas, x0, y0, x1, y1, kBg, kCardBorder);
    }
    // Invert tickTrackPos() to find the angle for the current fill amount,
    // then draw a thickened marker spanning just past the ring's own depth
    // on both sides (same perpendicular-offset thickening Lit Ticks uses).
    float pointerTrackPos = fillDeg;
    float pointerAngleNorm = (pointerTrackPos <= kArcRightSpan)
        ? kArcRightStartDeg + pointerTrackPos
        : kArcLeftStartDeg + (pointerTrackPos - kArcRightSpan);
    if (pointerAngleNorm >= 360.0f) pointerAngleNorm -= 360.0f;
    float pRad = pointerAngleNorm * kPi / 180.0f;
    float pc = cosf(pRad), ps = sinf(pRad);
    int16_t pOuterR = outerR + 6, pInnerR = innerMinor - 6;
    int16_t px0 = cx + (int16_t)(pc * pOuterR), py0 = cy + (int16_t)(ps * pOuterR);
    int16_t px1 = cx + (int16_t)(pc * pInnerR), py1 = cy + (int16_t)(ps * pInnerR);
    gfxaa::drawLineAA(sCanvas, px0, py0, px1, py1, kAccent, kBg);
    float perpX = -ps, perpY = pc;
    int16_t ox = (int16_t)(perpX >= 0 ? perpX + 0.5f : perpX - 0.5f);
    int16_t oy = (int16_t)(perpY >= 0 ? perpY + 0.5f : perpY - 0.5f);
    if (ox == 0 && oy == 0) { ox = 1; oy = 0; }
    gfxaa::drawLineAA(sCanvas, px0 + ox, py0 + oy, px1 + ox, py1 + oy, kAccent, kBg);
    gfxaa::drawLineAA(sCanvas, px0 - ox, py0 - oy, px1 - ox, py1 - oy, kAccent, kBg);
  } else {
    // Solid Arc (style 0, and the fallback for any unrecognized index): a
    // continuous filled band that grows with frac, with tick notches cut
    // into it in the background color as segment dividers -- minor notches
    // nick just the outer edge, major notches (every 5th) cut the full
    // band depth.
    sCanvas->fillArc(cx, cy, outerR, innerMajor, kArcRightStartDeg, kArcRightEndDeg, kCardBorder);
    sCanvas->fillArc(cx, cy, outerR, innerMajor, kArcLeftStartDeg, kArcLeftEndDeg, kCardBorder);
    if (rightFillDeg > 0.0f) {
      sCanvas->fillArc(cx, cy, outerR, innerMajor, kArcRightStartDeg, kArcRightStartDeg + rightFillDeg, kAccent);
    }
    if (leftFillDeg > 0.0f) {
      sCanvas->fillArc(cx, cy, outerR, innerMajor, kArcLeftStartDeg, kArcLeftStartDeg + leftFillDeg, kAccent);
    }
    for (int i = 0; i < kTickCount; i++) {
      float angleDeg = -90.0f + (360.0f * i) / kTickCount;
      float angleNorm = angleDeg < 0 ? angleDeg + 360.0f : angleDeg;
      if (tickInGap(angleNorm)) continue;
      bool major = (i % kTickMajorEvery) == 0;
      int16_t innerR = major ? innerMajor : innerMinor;
      float rad = angleDeg * kPi / 180.0f;
      float c = cosf(rad), s = sinf(rad);
      int16_t x0 = cx + (int16_t)(c * outerR);
      int16_t y0 = cy + (int16_t)(s * outerR);
      int16_t x1 = cx + (int16_t)(c * innerR);
      int16_t y1 = cy + (int16_t)(s * innerR);
      // Drawn in kBg on top of whatever's underneath -- the AA blend needs
      // to know which color that is (accent fill or dim base track) to
      // blend its edge pixels correctly, since this canvas has no
      // pixel-readback to check directly (same constraint documented in
      // gfx_aa.h). Right at the fill boundary this picks one side or the
      // other for that one notch's blend -- a sub-pixel approximation, not
      // visible in practice.
      uint16_t localBg = (tickTrackPos(angleNorm) < fillDeg) ? kAccent : kCardBorder;
      gfxaa::drawLineAA(sCanvas, x0, y0, x1, y1, kBg, localBg);
    }
  }
}

static uint16_t connColor(voltra::ConnState s) {
  switch (s) {
    case voltra::ConnState::Ready: return kAccent;
    case voltra::ConnState::Handshaking:
    case voltra::ConnState::Connecting: return kWarn;
    case voltra::ConnState::Scanning:
    case voltra::ConnState::Idle:
    default: return kDanger;
  }
}

static const char *connLabel(voltra::ConnState s) {
  switch (s) {
    case voltra::ConnState::Ready: return "CONNECTED";
    case voltra::ConnState::Handshaking: return "SYNCING";
    case voltra::ConnState::Connecting: return "CONNECTING";
    case voltra::ConnState::Scanning: return "SCANNING";
    case voltra::ConnState::Idle:
    default: return "OFFLINE";
  }
}

// Shorter stand-ins for connLabel()'s words, used only by the main workout
// screen's corner status label (draw() below, next to the status dot) --
// the tightest space these ever appear in, squeezed between the dot and
// the knob's own battery icon/percent on the same row. Widths measured
// with fonts/FreeSans9pt7b.h (DejaVu Sans at 9pt -- see that file's banner
// comment) via parse_font.py against that row's real, worst-case layout
// (dot right edge at x=99, knob battery group left-pinned against a
// worst-case "100%" reading at x=184 -- see draw()'s own comment): LINKED
// 64px, LINKING 72px, SYNC 48px, SCAN 47px, OFFLINE 73px (kept as-is --
// already short enough, and shortening it further to "OFF" read as
// ambiguous with "powered off"). All five fit the ~77px gap with margin to
// spare; the un-shortened "SYNCING" (81px) and "SCANNING" (95px) would not.
static const char *connLabelMain(voltra::ConnState s) {
  switch (s) {
    case voltra::ConnState::Ready: return "LINKED";
    case voltra::ConnState::Connecting: return "LINKING";
    case voltra::ConnState::Handshaking: return "SYNC";
    case voltra::ConnState::Scanning: return "SCAN";
    case voltra::ConnState::Idle:
    default: return connLabel(s); // "OFFLINE" -- already short enough
  }
}

// Formats an elapsed duration compactly ("12s", "3m", "1h") for the
// connection status row -- shown next to the label while reconnecting so a
// slow reconnect reads as "still actively trying, N seconds so far"
// instead of just looking frozen with no feedback. Bounded to a handful of
// characters regardless of how long the wait has been, so it doesn't blow
// up the row's width the way an unbounded second count eventually would.
static void formatElapsedShort(uint32_t ms, char *buf, size_t bufSize) {
  uint32_t s = ms / 1000;
  if (s < 60) snprintf(buf, bufSize, "%lus", (unsigned long)s);
  else if (s < 3600) snprintf(buf, bufSize, "%lum", (unsigned long)(s / 60));
  else snprintf(buf, bufSize, "%luh", (unsigned long)(s / 3600));
}

// Centers text at (cx, cy). font == nullptr uses the classic built-in
// bitmap font at the given textSize (good for tiny incidental captions);
// otherwise uses the given custom GFXfont at its native size. Accounts for
// getTextBounds()'s x1/y1 offsets so centering is correct for both --
// custom GFXfont glyphs are anchored at the baseline, not the top-left, so
// skipping that offset (as a naive w/2,h/2 centering does) drifts text
// upward as font size grows.
static void drawCentered(Arduino_Canvas *c, const char *text, int16_t cx, int16_t cy,
                          const GFXfont *font, uint8_t textSizeIfClassic, uint16_t color) {
  c->setFont(font);
  c->setTextSize(font ? 1 : textSizeIfClassic);
  c->setTextColor(color);
  int16_t x1, y1;
  uint16_t w, h;
  c->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  c->setCursor(cx - x1 - (int16_t)(w / 2), cy - y1 - (int16_t)(h / 2));
  c->print(text);
}

// Left-anchored version (x is the left edge of the text), vertically
// centered on cy. Used to lay out the dot+label+battery status row, whose
// total width depends on the connection label, so it's assembled from
// pieces rather than one fixed string. Returns the x just past the drawn
// text, so pieces can be chained left-to-right.
static int16_t drawLeftAligned(Arduino_Canvas *c, const char *text, int16_t x, int16_t cy,
                                const GFXfont *font, uint16_t color) {
  c->setFont(font);
  c->setTextSize(1);
  c->setTextColor(color);
  int16_t x1, y1;
  uint16_t w, h;
  c->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  c->setCursor(x - x1, cy - y1 - (int16_t)(h / 2));
  c->print(text);
  return x + w;
}

static uint16_t textWidth(Arduino_Canvas *c, const char *text, const GFXfont *font) {
  c->setFont(font);
  c->setTextSize(1);
  int16_t x1, y1;
  uint16_t w, h;
  c->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  return w;
}

// Small battery pictogram (outline + terminal nub + a fill proportional to
// percent) for the KNOB BOARD's own battery -- deliberately icon-only, no
// percent text, to keep it narrow: this sits at the end of the already-
// fairly-full status row (see draw()), and that row's widest case
// (connLabelMain()'s "CONNECTING"-length "LINKING" + a 3-digit Voltra
// battery percent) is already close to this round panel's safe width
// (originally measured against the longer "CONNECTED"/"CONNECTING", an
// even bigger margin now), so this adds only ~kIconW+gap pixels
// rather than another whole text string. If this specific icon looks
// clipped by the bezel on real hardware, shrink kIconW/kIconH or move its
// position (x, y here) first -- everything else on this row was tuned
// against real hardware already and shouldn't need to move.
static constexpr int16_t kBattIconW = 13, kBattIconH = 8;
static void drawBatteryIcon(Arduino_Canvas *c, int16_t x, int16_t y, int percent, uint16_t color) {
  constexpr int16_t kNubW = 2, kNubH = kBattIconH / 2;
  int16_t bodyW = kBattIconW - kNubW;
  c->drawRoundRect(x, y, bodyW, kBattIconH, 2, color);
  c->fillRect(x + bodyW, y + (kBattIconH - kNubH) / 2, kNubW, kNubH, color);
  if (percent > 0) {
    int16_t innerW = bodyW - 4;
    int16_t p = percent > 100 ? 100 : percent;
    int16_t fillW = (int16_t)((int32_t)innerW * p / 100);
    if (fillW > 0) c->fillRect(x + 2, y + 2, fillW, kBattIconH - 4, color);
  }
}

// How far the currently-selected mode's dial value sits within its valid
// range (0.0 at the minimum, 1.0 at the maximum), for the tick-ring gauge
// below. Mirrors each mode's real clamp range from voltra_protocol.h/
// voltra_ble.cpp (kMinTargetLb/kMaxTargetLb for Base, kMinEccentricLb/
// kMaxEccentricLb for Eccentric) so the gauge always matches what the
// encoder can actually reach. Chains' upper bound is dynamic (capped at the
// current base weight -- see setChainsWeightLb's comment), so it's computed
// the same way here rather than duplicated as a separate constant.
static float modeValueFraction(const voltra::VoltraState &st) {
  int value, lo, hi;
  if (sMode == Mode::Base) {
    value = st.baseWeightLb; lo = voltra::kMinTargetLb; hi = voltra::kMaxTargetLb;
  } else if (sMode == Mode::Eccentric) {
    value = st.eccentricWeightLb; lo = voltra::kMinEccentricLb; hi = voltra::kMaxEccentricLb;
  } else {
    value = st.chainsWeightLb;
    lo = 0;
    hi = (st.baseWeightLb < voltra::kMaxTargetLb) ? st.baseWeightLb : voltra::kMaxTargetLb;
    if (hi <= lo) hi = lo + 1; // avoid a divide-by-zero if base weight is somehow 0 (shouldn't happen -- kMinTargetLb is 5)
  }
  if (hi <= lo) return 0.0f;
  float f = (float)(value - lo) / (float)(hi - lo);
  if (f < 0.0f) f = 0.0f;
  if (f > 1.0f) f = 1.0f;
  return f;
}

// ---------------------------------------------------------------------------
// Settings top-level list (Screen::Settings). One row per sub-screen this
// menu opens; see the big comment on the Screen enum for why this stays a
// flat list rather than a stack. sSettingsFocus is the currently-centered
// item, moved by the encoder (see update()) and clamped (NOT wrapped) to
// [0, kSettingsItemCount-1] -- wrapping would let a fast spin silently loop
// past "Factory Reset" back to "Theme" with no visual cue that it wrapped,
// which is exactly the kind of surprise a destructive item like Factory
// Reset shouldn't be one careless flick away from.
struct SettingsItem { const char *label; Screen screen; };
static const SettingsItem kSettingsItems[] = {
  {"Theme", Screen::Theme},
  {"Gauge Style", Screen::GaugeStyle},
  {"Wi-Fi", Screen::Wifi},
  {"Bluetooth", Screen::Bluetooth},
  {"Twin Mode", Screen::Twin},
  {"Brightness", Screen::Brightness},
  {"Auto-Dim", Screen::AutoDim},
  {"Step Size", Screen::StepSize},
  {"Vibration", Screen::Vibration},
  {"Weight Presets", Screen::Presets},
  {"Units", Screen::Units},
  {"Drop Mode", Screen::DropMode},
  {"Drop Count", Screen::DropCount},
  {"Drop %", Screen::DropPercent},
  {"Drop Every", Screen::DropInterval},
  {"About", Screen::About},
  {"Update", Screen::FirmwareUpdate},
  {"Factory Reset", Screen::FactoryReset},
};
static constexpr uint8_t kSettingsItemCount = sizeof(kSettingsItems) / sizeof(kSettingsItems[0]);
static int8_t sSettingsFocus = 0;

// Eased scroll position for the settings list, in item-index units (e.g.
// 2.0 == sitting exactly on item 2; 1.6 == 60% of the way from item 1 to
// item 2). Every update() tick this closes part of the remaining distance
// to (float)sSettingsFocus -- see kSettingsAnimEase below -- so turning the
// encoder slides the list smoothly through a fixed center "window" instead
// of the label just snapping to its new text. Reset to match sSettingsFocus
// whenever Settings is freshly (re-)opened (see the kSettingsTapZone
// handler in update()) so a visit never animates in from wherever it was
// left last time.
static float sSettingsAnimY = 0.0f;
// Fraction of the remaining distance to close per ~20ms update() tick (this
// project polls input at 50Hz -- see loop()'s delay(20) in main.cpp).
// ~0.35 settles a one-item move in well under 200ms: fast enough to feel
// immediate on a knob turn, slow enough to actually read as a slide rather
// than a blink.
static constexpr float kSettingsAnimEase = 0.35f;
// Once within this many item-index-units of the target, snap exactly to it
// and stop forcing redraws -- otherwise the asymptotic ease approaches but
// never quite reaches the target, redrawing forever over an invisible
// fraction of a pixel.
static constexpr float kSettingsAnimSnapEps = 0.01f;

// Which item's value/status line (e.g. "100%", "Connected", "Cyan") is
// currently shown -- deliberately NOT always sSettingsFocus. Updating the
// value line the instant the encoder moves (i.e. reading it straight off
// sSettingsFocus) meant it popped to the new item's status before that
// item's label had even finished sliding into the window, which read as
// mismatched/premature. Instead this only catches up to sSettingsFocus once
// the reel actually finishes settling there (see the anim-easing block in
// update()), so the value line changes right as the new item arrives
// in the window, not before.
static uint8_t sSettingsValueFocus = 0;

// Two-tap confirm gesture for Factory Reset -- see drawFactoryResetScreen()/
// the touch dispatch in update(). Chosen over a duplicate of the engage
// button's hold-to-arm state machine to keep this simpler: a destructive
// action just needs "are you sure", not a timed press.
static constexpr uint32_t kFactoryResetArmWindowMs = 5000;
static bool sFactoryResetArmed = false;
static uint32_t sFactoryResetArmedAtMs = 0;

// ---------------------------------------------------------------------------
// Settings menu tree screens (Screen::Settings and everything it opens).
// These share a plain bezel-only background (none of the workout screen's
// tick ring/status row/etc apply here) and the same rounded-pill "back"
// button as the workout screen's own engage button, drawn via
// drawBackButton() below.

static void drawSubScreenBezel() {
  sCanvas->fillScreen(kBg);
  gfxaa::drawCircleAA(sCanvas, kCx, kCy, kBezelR, 1.3f, kCardBorder, kBg);
}

static void drawBackButton(const char *label) {
  sCanvas->fillRoundRect(kBackZone.x, kBackZone.y, kBackZone.w, kBackZone.h, 22, kCard);
  sCanvas->drawRoundRect(kBackZone.x, kBackZone.y, kBackZone.w, kBackZone.h, 22, kCardBorder);
  drawCentered(sCanvas, label, kBackZone.x + kBackZone.w / 2, kBackZone.y + kBackZone.h / 2,
               &FreeSansBold15pt7b, 1, kTextPrimary);
}

// The rounded-rect focus-row background/border the Settings list's picker
// wheel slides its label *through* (see drawSettingsScreen()) -- split out
// from the old drawMenuRow() (which also drew the label+value text baked to
// this exact rect) since the reel redraws that text fresh each frame
// instead. Filled with kSettingsHighlight/bordered in kDanger rather than
// the plain neutral kCard/kCardBorder every other card in this file uses,
// to match macfitnesscoaching.com's Voltra Remote guide, which draws its
// settings screens' selected row as a full-width dark-red highlighted band
// -- the encoder-driven scrolling/animation itself (sSettingsAnimY etc.,
// already tested on real hardware) is unchanged, just this one color.
static void drawCardFrame(const Rect &r) {
  sCanvas->fillRoundRect(r.x, r.y, r.w, r.h, 16, kSettingsHighlight);
  sCanvas->drawRoundRect(r.x, r.y, r.w, r.h, 16, kDanger);
}

// A single tappable pill button with one line of bold text -- the same
// rounded-rect look as drawMenuRow's card, generalized for the various
// single-action sub-screens (Wi-Fi's ADD/FORGET, Bluetooth's RECONNECT,
// Factory Reset's RESET/TAP AGAIN?) so each doesn't redraw this by hand.
static void drawPillButton(const Rect &r, const char *label, uint16_t color) {
  sCanvas->fillRoundRect(r.x, r.y, r.w, r.h, 16, kCard);
  sCanvas->drawRoundRect(r.x, r.y, r.w, r.h, 16, kCardBorder);
  drawCentered(sCanvas, label, r.x + r.w / 2, r.y + r.h / 2, &FreeSansBold15pt7b, 1, color);
}

// Live status text for a Settings row, shown as the dimmer second line of
// the focused item's pill (see drawSettingsScreen()) -- e.g. the current
// theme name, the saved Wi-Fi SSID, "ON"/"OFF". Needs the live VoltraState
// for the Bluetooth row's connection status.
static void settingsItemValue(uint8_t idx, const voltra::VoltraState &st, char *buf, size_t bufSize,
                               uint16_t &color) {
  color = kTextMuted;
  switch (kSettingsItems[idx].screen) {
    case Screen::Theme:
      snprintf(buf, bufSize, "%s", kThemes[sThemeIndex].name);
      break;
    case Screen::GaugeStyle:
      snprintf(buf, bufSize, "%s", kGaugeStyles[sGaugeStyleIndex].name);
      break;
    case Screen::Wifi: {
      wifiprov::Status wst = wifiprov::status();
      const char *v = (wst.state == wifiprov::State::Connected)    ? wst.ssid
                     : (wst.state == wifiprov::State::Connecting)  ? "Connecting..."
                     : (wst.state == wifiprov::State::Provisioning) ? "Setting up..."
                     : wst.hasSaved                                 ? "Not connected"
                                                                     : "Not set up";
      snprintf(buf, bufSize, "%s", v);
      if (wst.state == wifiprov::State::Connected) color = kAccent;
      break;
    }
    case Screen::Bluetooth:
      snprintf(buf, bufSize, "%s", connLabel(st.connState));
      color = connColor(st.connState);
      break;
    case Screen::Twin:
      if (st.twinState == voltra::TwinState::Twinned) {
        snprintf(buf, bufSize, "Twinned");
        color = kAccent;
      } else {
        snprintf(buf, bufSize, "Off");
      }
      break;
    case Screen::Brightness:
      snprintf(buf, bufSize, "%u%%", prefs::brightnessPercent());
      break;
    case Screen::AutoDim: {
      uint16_t s = prefs::autoDimSeconds();
      if (s == 0) snprintf(buf, bufSize, "Never");
      else snprintf(buf, bufSize, "%us", (unsigned)s);
      break;
    }
    case Screen::StepSize:
      snprintf(buf, bufSize, "%u lb", prefs::stepSizeLb());
      break;
    case Screen::Vibration:
      snprintf(buf, bufSize, "%s", prefs::hapticsEnabled() ? "On" : "Off");
      break;
    case Screen::Presets:
      snprintf(buf, bufSize, "%s", prefs::presetsEnabled() ? "On" : "Off");
      break;
    case Screen::Units:
      snprintf(buf, bufSize, "%s", prefs::unitsLabel(prefs::units()));
      break;
    case Screen::DropMode:
      snprintf(buf, bufSize, "%s", kDropModes[sDropModeIndex].name);
      if (sDropModeIndex != 0) color = kAccent;
      break;
    case Screen::DropCount:
      snprintf(buf, bufSize, "%u", (unsigned)kDropCounts[sDropCountIndex]);
      break;
    case Screen::DropPercent:
      snprintf(buf, bufSize, "%u%%", (unsigned)kDropPercents[sDropPercentIndex]);
      break;
    case Screen::DropInterval:
      snprintf(buf, bufSize, "%u reps", (unsigned)kDropIntervals[sDropIntervalIndex]);
      break;
    case Screen::About:
      snprintf(buf, bufSize, "v%s", fw::kVersion);
      break;
    case Screen::FirmwareUpdate: {
      otaupdate::Status ost = otaupdate::status();
      if (ost.state == otaupdate::State::UpdateAvailable) {
        snprintf(buf, bufSize, "v%s available", ost.availableVersion);
        color = kAccent;
      } else if (ost.state == otaupdate::State::Downloading) {
        snprintf(buf, bufSize, "Installing... %u%%", ost.progressPercent);
        color = kAccent;
      } else {
        snprintf(buf, bufSize, "v%s", fw::kVersion);
      }
      break;
    }
    case Screen::FactoryReset:
    default:
      buf[0] = '\0';
      break;
  }
}

// Vertical spacing between adjacent items in the settings list reel, in
// pixels -- matches this screen's original fixed label positions exactly
// (item center 180, one neighbor at 110, the other at 250 -- all 70px
// apart), so the list's resting/settled look is unchanged from before this
// grew an animation; only the motion between those positions, and the
// font used to draw them, are new.
static constexpr int16_t kSettingsRowSpacing = 70;
// How far (in item-index units) a row can drift from center and still be
// drawn. Capped below 1.5 (a full row step) rather than at it: a
// mid-transition row can drift most of the way toward the *next* slot
// before it's replaced by the item after it, and at a full 1.5 that drift
// reaches y positions that collide with the title text above (bottom edge
// ~66px) or the BACK button below (top edge 276px) -- checked against both
// real measured text heights (parse_font.py) and a rendered preview.
// 1.2 keeps a comfortable ~10px clearance on both sides at the
// most extreme drift a row is ever drawn at, while still visibly sliding
// most of the way between rows rather than popping in late.
static constexpr float kSettingsVisibleRows = 1.2f;

// Settings' scrollable top-level list. Turning the encoder eases
// sSettingsAnimY toward sSettingsFocus a bit every update() tick (see
// update()), and this draws every item still within kSettingsVisibleRows
// of that live, possibly-mid-transition position at its own interpolated
// height -- so scrolling reads as a smooth "picker wheel" slide through a
// fixed center window/pill, rather than the old instant text swap. Each
// row's font/color also steps down with distance from center (bold+bright
// in the window, a real (not the tiny built-in bitmap) font dimmed one
// step out) for a legible depth cue instead of one-size-fits-all text.
static void drawSettingsScreen(const voltra::VoltraState &st) {
  drawSubScreenBezel();
  drawCentered(sCanvas, "SETTINGS", kCx, 56, &FreeSansBold15pt7b, 1, kTextPrimary);

  drawCardFrame(kSettingsFocusZone);

  int lo = (int)floorf(sSettingsAnimY - kSettingsVisibleRows);
  int hi = (int)ceilf(sSettingsAnimY + kSettingsVisibleRows);
  if (lo < 0) lo = 0;
  if (hi > (int)kSettingsItemCount - 1) hi = (int)kSettingsItemCount - 1;
  for (int i = lo; i <= hi; i++) {
    float d = (float)i - sSettingsAnimY;
    float absd = fabsf(d);
    if (absd > kSettingsVisibleRows) continue;
    // The -9 offset (rather than dead center) matches where the focused
    // label always sat within its pill, above the value/status line below
    // it -- applied uniformly here so that label lands in exactly the same
    // spot once the reel settles (absd == 0).
    int16_t y = (int16_t)lroundf((float)(kCy - 9) + d * (float)kSettingsRowSpacing);
    const GFXfont *font;
    uint16_t color;
    if (absd < 0.5f) {
      font = &FreeSansBold15pt7b;
      color = kTextPrimary;
    } else {
      font = &FreeSans12pt7b;
      color = kTextMuted;
    }
    drawCentered(sCanvas, kSettingsItems[i].label, kCx, y, font, 1, color);
  }

  // The value/status line stays put rather than riding the reel -- it's
  // secondary detail, not something that needs its own slide -- and reads
  // sSettingsValueFocus rather than sSettingsFocus, so it only switches to
  // the new item's status once that item has actually finished sliding
  // into the window, instead of popping to the new value while the old
  // label is still mid-slide out (see sSettingsValueFocus's comment).
  char valueBuf[24];
  uint16_t valueColor;
  settingsItemValue(sSettingsValueFocus, st, valueBuf, sizeof(valueBuf), valueColor);
  drawCentered(sCanvas, valueBuf, kCx, kSettingsFocusZone.y + kSettingsFocusZone.h / 2 + 13,
               &FreeSans9pt7b, 1, valueColor);

  drawBackButton("BACK");
  sCanvas->flush();
}

// Generic "cycle through a small preset list" sub-screen -- prev/next
// chevrons (kThemePrevZone/kThemeNextZone) step through the list, showing
// the current value centered. swatchColor >= 0 additionally draws a color
// swatch above the value (used only by Theme); pass -1 for plain text-only
// screens (Brightness, Auto-Dim, Step Size, Vibration, Units).
static void drawCycleScreen(const char *title, const char *valueText, int32_t swatchColor) {
  drawSubScreenBezel();
  drawCentered(sCanvas, title, kCx, 56, &FreeSansBold15pt7b, 1, kTextPrimary);

  if (swatchColor >= 0) {
    // A big swatch of the color itself, plus its name below -- the truest
    // on-device preview would be the whole screen re-rendered in the
    // candidate palette, but that means leaving (and instantly losing
    // context on) the workout screen just to compare colors; this swatch
    // plus name answers "which theme is this, and roughly what does it
    // look like" without that round-trip.
    sCanvas->fillCircle(kCx, 150, 36, (uint16_t)swatchColor);
    drawCentered(sCanvas, valueText, kCx, 210, &FreeSansBold15pt7b, 1, kTextPrimary);
  } else {
    drawCentered(sCanvas, valueText, kCx, 178, &FreeSansBold15pt7b, 1, kTextPrimary);
  }

  // Prev/next chevrons, drawn as simple filled triangles. Both touch zones
  // are considerably bigger than the drawn glyph -- the same icon-vs-tap-
  // zone lesson as kSettingsTapZone above.
  sCanvas->fillTriangle(kThemePrevZone.x + 38, kThemePrevZone.y + 12,
                        kThemePrevZone.x + 38, kThemePrevZone.y + 48,
                        kThemePrevZone.x + 14, kThemePrevZone.y + 30, kTextMuted);
  sCanvas->fillTriangle(kThemeNextZone.x + 22, kThemeNextZone.y + 12,
                        kThemeNextZone.x + 22, kThemeNextZone.y + 48,
                        kThemeNextZone.x + 46, kThemeNextZone.y + 30, kTextMuted);

  drawBackButton("BACK");
  sCanvas->flush();
}

static void drawThemeScreen() {
  drawCycleScreen("THEME", kThemes[sThemeIndex].name, kThemes[sThemeIndex].accent);
}

// Gauge Style doesn't fit drawCycleScreen()'s swatch-or-plain-text shape --
// it needs an actual live shape preview, not a color dot, so this is its
// own small variant of the same layout (same title position, same
// prev/next chevrons and back button) with drawGaugeStyleAt() standing in
// for the swatch. Fixed frac=0.62 so the preview always shows a partially
// filled, easy-to-read state regardless of the real workout's current
// value. outerR=58 (vs the workout screen's real kTickOuterR=170) was
// checked the same offline-geometry way as everything else in this file:
// at that radius the visible arcs' nearest approach to the top/bottom gaps
// lands at y=103/y=200, comfortably clear of both the title (ends y=69)
// and the value-name text below (starts y=207) -- see drawGaugeStyleAt()'s
// gap-angle math for where those numbers come from.
static void drawGaugeStyleScreen() {
  drawSubScreenBezel();
  drawCentered(sCanvas, "GAUGE STYLE", kCx, 56, &FreeSansBold15pt7b, 1, kTextPrimary);

  drawGaugeStyleAt(kCx, 150, 58, 0.62f, sGaugeStyleIndex);
  drawCentered(sCanvas, kGaugeStyles[sGaugeStyleIndex].name, kCx, 220, &FreeSansBold15pt7b, 1, kTextPrimary);

  sCanvas->fillTriangle(kThemePrevZone.x + 38, kThemePrevZone.y + 12,
                        kThemePrevZone.x + 38, kThemePrevZone.y + 48,
                        kThemePrevZone.x + 14, kThemePrevZone.y + 30, kTextMuted);
  sCanvas->fillTriangle(kThemeNextZone.x + 22, kThemeNextZone.y + 12,
                        kThemeNextZone.x + 22, kThemeNextZone.y + 48,
                        kThemeNextZone.x + 46, kThemeNextZone.y + 30, kTextMuted);

  drawBackButton("BACK");
  sCanvas->flush();
}

static void drawBrightnessScreen() {
  char buf[8];
  snprintf(buf, sizeof(buf), "%u%%", prefs::brightnessPercent());
  drawCycleScreen("BRIGHTNESS", buf, -1);
}

static void drawAutoDimScreen() {
  char buf[12];
  uint16_t s = prefs::autoDimSeconds();
  if (s == 0) snprintf(buf, sizeof(buf), "Never");
  else snprintf(buf, sizeof(buf), "%us", (unsigned)s);
  drawCycleScreen("AUTO-DIM", buf, -1);
}

static void drawStepSizeScreen() {
  char buf[16];
  snprintf(buf, sizeof(buf), "%u lb/click", prefs::stepSizeLb());
  drawCycleScreen("STEP SIZE", buf, -1);
}

static void drawVibrationScreen() {
  drawCycleScreen("VIBRATION", prefs::hapticsEnabled() ? "ON" : "OFF", -1);
}

static void drawPresetsScreen() {
  // "Weight Presets" rather than just "Presets" here (unlike the terser
  // kSettingsItems label, which has less room) -- disambiguates from Theme/
  // Gauge Style presets, since this screen only ever refers to the P1/P2
  // weight-recall pills on the main workout screen.
  drawCycleScreen("WEIGHT PRESETS", prefs::presetsEnabled() ? "ON" : "OFF", -1);
}

static void drawUnitsScreen() {
  drawCycleScreen("UNITS", prefs::unitsLabel(prefs::units()), -1);
}

static void drawDropModeScreen() {
  drawCycleScreen("DROP MODE", kDropModes[sDropModeIndex].name, -1);
}
static void drawDropCountScreen() {
  char buf[4];
  snprintf(buf, sizeof(buf), "%u", (unsigned)kDropCounts[sDropCountIndex]);
  drawCycleScreen("DROP COUNT", buf, -1);
}
static void drawDropPercentScreen() {
  char buf[8];
  snprintf(buf, sizeof(buf), "%u%%", (unsigned)kDropPercents[sDropPercentIndex]);
  drawCycleScreen("DROP %", buf, -1);
}
static void drawDropIntervalScreen() {
  char buf[16];
  snprintf(buf, sizeof(buf), "%u reps", (unsigned)kDropIntervals[sDropIntervalIndex]);
  drawCycleScreen("DROP EVERY", buf, -1);
}

static void drawBluetoothScreen(const voltra::VoltraState &st) {
  drawSubScreenBezel();
  drawCentered(sCanvas, "BLUETOOTH", kCx, 56, &FreeSansBold15pt7b, 1, kTextPrimary);
  drawCentered(sCanvas, connLabel(st.connState), kCx, 130, &FreeSansBold15pt7b, 1, connColor(st.connState));
  if (st.connState != voltra::ConnState::Ready) {
    char elapsedBuf[8];
    formatElapsedShort(millis() - st.disconnectedSinceMs, elapsedBuf, sizeof(elapsedBuf));
    char line2[24];
    snprintf(line2, sizeof(line2), "for %s", elapsedBuf);
    drawCentered(sCanvas, line2, kCx, 158, nullptr, 1, kTextFaint);
  }
  // Forces a disconnect + immediate rescan (see voltra_ble.cpp's
  // disconnectAndRescan()) instead of waiting out whatever backoff delay
  // update() would otherwise be sitting in -- useful if a reconnect looks
  // stuck, or just to force a fresh handshake after power-cycling the
  // Voltra itself.
  drawPillButton(kWifiActionZone, "RECONNECT", kAccent);
  drawBackButton("BACK");
  sCanvas->flush();
}

// Which candidate (of however many startTwinDiscovery() found) the Twin
// screen is currently showing -- cycled with kThemePrevZone/kThemeNextZone,
// same reuse idiom as the Theme/GaugeStyle/Brightness/etc. cycle screens.
// Reset to 0 whenever a fresh discovery scan starts (see the kWifiActionZone
// handler in update()) so a new result set never opens mid-list.
static uint8_t sTwinCandidateIdx = 0;
// Distinguishes "never scanned yet" from "scanned, found nothing" -- both
// have twinCandidateCount()==0, but read very differently to the user (see
// drawTwinScreen()). Reset to false whenever a fresh scan starts.
static bool sTwinHasScanned = false;

// Settings > Twin Mode -- see voltra_protocol.h's kCmdTwinLink comment for
// the protocol this drives. Same one-status-line-plus-one-action-pill
// layout as drawBluetoothScreen()/drawWifiScreen() above, reusing
// kWifiActionZone for the single action and kThemePrevZone/kThemeNextZone
// to cycle candidates when more than one nearby Voltra was found.
static void drawTwinScreen(const voltra::VoltraState &st, voltra::VoltraClient &voltraClient) {
  drawSubScreenBezel();
  drawCentered(sCanvas, "TWIN MODE", kCx, 56, &FreeSansBold15pt7b, 1, kTextPrimary);

  if (st.twinState == voltra::TwinState::Twinned) {
    drawCentered(sCanvas, "TWINNED", kCx, 130, &FreeSansBold15pt7b, 1, kAccent);
    drawCentered(sCanvas, st.twinFollowerAddrStr, kCx, 158, nullptr, 1, kTextFaint);
    // Only shown for the brief window between sending the link command and
    // the next kCmdTwinStatus poll actually confirming it (or a stale
    // twinState from a previous session/spontaneous untwin getting
    // corrected back to "not twinned" instead) -- see voltra_ble.cpp's
    // applyTwinStatus. Shorter than the address line above it, which
    // already proves this font/width fits here.
    if (!st.twinConfirmedByHost) {
      drawCentered(sCanvas, "confirming...", kCx, 172, nullptr, 1, kTextMuted);
    }
    drawPillButton(kWifiActionZone, "UN-TWIN", kDanger);
  } else if (st.twinPendingAcceptance) {
    // The host reports it's already twinned with someone, but this knob
    // didn't initiate it -- see voltra_ble.h's twinPendingAcceptance
    // comment for why this is offered as a choice (BACK to leave it
    // alone) rather than switched into automatically, unlike the
    // voltra-pedal project's fully-automatic join.
    drawCentered(sCanvas, "Twin available", kCx, 118, nullptr, 1, kTextFaint);
    drawCentered(sCanvas, st.twinPendingPeerAddrStr, kCx, 144, &FreeSansBold15pt7b, 1, kWarn);
    drawCentered(sCanvas, "reported by this Voltra", kCx, 166, nullptr, 1, kTextFaint);
    drawPillButton(kWifiActionZone, "ENTER TWIN MODE", kAccent);
  } else if (st.connState != voltra::ConnState::Ready) {
    // Twin setup needs an active connection to send the link command
    // through -- see linkFollower()'s comment on why the primary connection
    // is left untouched by it, but it still has to exist first.
    drawCentered(sCanvas, "Connect first", kCx, 130, &FreeSansBold15pt7b, 1, kTextMuted);
    drawPillButton(kWifiActionZone, "FIND VOLTRA", kTextMuted);
  } else if (voltraClient.twinDiscoveryInProgress()) {
    drawCentered(sCanvas, "Searching...", kCx, 130, &FreeSansBold15pt7b, 1, kWarn);
    drawPillButton(kWifiActionZone, "WAIT...", kTextMuted);
  } else if (voltraClient.twinCandidateCount() > 0) {
    if (sTwinCandidateIdx >= voltraClient.twinCandidateCount()) sTwinCandidateIdx = 0;
    const voltra::TwinCandidate &c = voltraClient.twinCandidate(sTwinCandidateIdx);
    drawCentered(sCanvas, "Twin with:", kCx, 118, nullptr, 1, kTextFaint);
    drawCentered(sCanvas, c.addressStr, kCx, 144, &FreeSansBold15pt7b, 1, kTextPrimary);
    if (voltraClient.twinCandidateCount() > 1) {
      // Cycled with the ENCODER (see the Screen::Twin case in update()'s
      // encoder-handling block), not touch chevrons -- kThemePrevZone/
      // kThemeNextZone visually and spatially overlap kWifiActionZone
      // (checked: 72-288 vs 40-100/260-320 in x, 150-210 vs 180-226 in y),
      // which is exactly why every OTHER screen that reuses these zones
      // only ever uses one pair or the other, never both at once. Turning
      // the knob to pick a candidate also just fits this knob-first UI
      // better than a touch chevron would here.
      char countBuf[16];
      snprintf(countBuf, sizeof(countBuf), "%u of %u -- turn knob", (unsigned)(sTwinCandidateIdx + 1), (unsigned)voltraClient.twinCandidateCount());
      drawCentered(sCanvas, countBuf, kCx, 166, nullptr, 1, kTextFaint);
    }
    drawPillButton(kWifiActionZone, "TWIN WITH THIS", kAccent);
  } else if (sTwinHasScanned) {
    drawCentered(sCanvas, "No other Voltra found", kCx, 130, &FreeSansBold15pt7b, 1, kTextMuted);
    drawPillButton(kWifiActionZone, "FIND VOLTRA", kAccent);
  } else {
    drawCentered(sCanvas, "Not twinned", kCx, 130, &FreeSansBold15pt7b, 1, kTextMuted);
    drawPillButton(kWifiActionZone, "FIND VOLTRA", kAccent);
  }
  drawBackButton("BACK");
  sCanvas->flush();
}

static void drawAboutScreen() {
  drawSubScreenBezel();
  drawCentered(sCanvas, "ABOUT", kCx, 56, &FreeSansBold15pt7b, 1, kTextPrimary);

  char versionBuf[24];
  snprintf(versionBuf, sizeof(versionBuf), "Firmware v%s", fw::kVersion);
  drawCentered(sCanvas, versionBuf, kCx, 140, &FreeSansBold15pt7b, 1, kTextPrimary);

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macBuf[24];
  snprintf(macBuf, sizeof(macBuf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  drawCentered(sCanvas, macBuf, kCx, 172, nullptr, 1, kTextFaint);

  // Info-only screen -- no action pill, just Back.
  drawBackButton("BACK");
  sCanvas->flush();
}

// Settings > Update -- pull-based firmware updates (see ota_update.h for
// the actual check/download/flash logic; this just reflects its Status).
// Follows the same "status text(s) + one action pill" layout as the Wi-Fi/
// Bluetooth screens above. Every string drawn in the big bold font here
// (both line1's fixed per-state text and the action pill labels) was
// measured offline against its zone the same way every other label in this
// file was -- see ota_update.h's comment on Status::message for why the
// module itself only ever hands this short, pre-measured strings rather
// than passing an arbitrary-length library error straight through.
static void drawFirmwareUpdateScreen(bool workoutActive) {
  drawSubScreenBezel();
  drawCentered(sCanvas, "UPDATE", kCx, 56, &FreeSansBold15pt7b, 1, kTextPrimary);

  otaupdate::Status ost = otaupdate::status();
  char line1[24] = "";
  char line2[32] = "";
  uint16_t line1Color = kTextMuted;

  switch (ost.state) {
    case otaupdate::State::Checking:
      snprintf(line1, sizeof(line1), "Checking...");
      line1Color = kWarn;
      break;
    case otaupdate::State::UpdateAvailable:
      snprintf(line1, sizeof(line1), "Update available");
      snprintf(line2, sizeof(line2), "v%s -> v%s", fw::kVersion, ost.availableVersion);
      line1Color = kAccent;
      break;
    case otaupdate::State::Downloading:
      // No live percentage here on purpose: the whole download+flash+
      // verify runs inside one blocking call (see ota_update.cpp's
      // runInstall()), so this screen cannot redraw again until it's done
      // -- a number would just freeze at whatever it was and look like a
      // hang. Static, honest copy instead: this can genuinely take a
      // couple of minutes with no visible change, and that's normal.
      snprintf(line1, sizeof(line1), "Installing...");
      snprintf(line2, sizeof(line2), "May take a few min -- don't power off");
      line1Color = kAccent;
      break;
    case otaupdate::State::Error:
      snprintf(line1, sizeof(line1), "%s", ost.message);
      line1Color = kDanger;
      break;
    case otaupdate::State::Idle:
    default:
      snprintf(line1, sizeof(line1), "Current version");
      snprintf(line2, sizeof(line2), "v%s", fw::kVersion);
      break;
  }
  drawCentered(sCanvas, line1, kCx, 118, &FreeSansBold15pt7b, 1, line1Color);
  if (line2[0] != '\0') drawCentered(sCanvas, line2, kCx, 144, nullptr, 1, kTextFaint);

  const char *actionLabel;
  uint16_t actionColor;
  switch (ost.state) {
    case otaupdate::State::UpdateAvailable:
      actionLabel = "INSTALL";
      actionColor = kAccent;
      break;
    case otaupdate::State::Checking:
    case otaupdate::State::Downloading:
      actionLabel = "WAIT...";
      actionColor = kTextMuted;
      break;
    default:
      actionLabel = "CHECK NOW";
      actionColor = kAccent;
      break;
  }
  drawPillButton(kWifiActionZone, actionLabel, actionColor);
  if (workoutActive && ost.state == otaupdate::State::UpdateAvailable) {
    // Installing is blocked mid-workout (see ota_update.h's beginInstall()
    // comment) -- flagged here too so tapping INSTALL isn't a silent no-op.
    drawCentered(sCanvas, "Finish set first", kCx, 232, nullptr, 1, kWarn);
  }
  drawBackButton("BACK");
  sCanvas->flush();
}

static void drawFactoryResetScreen() {
  drawSubScreenBezel();
  drawCentered(sCanvas, "RESET", kCx, 56, &FreeSansBold15pt7b, 1, kTextPrimary);
  drawCentered(sCanvas, "Erases all settings", kCx, 118, nullptr, 1, kTextFaint);
  drawCentered(sCanvas, "and saved Wi-Fi", kCx, 136, nullptr, 1, kTextFaint);

  // Two-tap confirm: first tap arms (see the touch dispatch in update()) and
  // flips this label to "TAP AGAIN?"; a second tap within
  // kFactoryResetArmWindowMs actually resets. Tapping Back, or just letting
  // the window expire, disarms with no effect.
  drawPillButton(kWifiActionZone, sFactoryResetArmed ? "TAP AGAIN?" : "RESET", kDanger);
  drawBackButton("CANCEL");
  sCanvas->flush();
}

static void drawWifiScreen() {
  drawSubScreenBezel();
  drawCentered(sCanvas, "WI-FI", kCx, 56, &FreeSansBold15pt7b, 1, kTextPrimary);

  wifiprov::Status wst = wifiprov::status();
  char line1[40] = "";
  char line2[40] = "";
  uint16_t line1Color = kTextMuted;

  if (wst.state == wifiprov::State::Provisioning) {
    snprintf(line1, sizeof(line1), "Connect your phone to:");
    snprintf(line2, sizeof(line2), "%s", wst.apName);
  } else if (wst.state == wifiprov::State::Connected) {
    snprintf(line1, sizeof(line1), "Connected");
    snprintf(line2, sizeof(line2), "%s", wst.ssid);
    line1Color = kAccent;
  } else if (wst.state == wifiprov::State::Connecting) {
    snprintf(line1, sizeof(line1), "Connecting...");
    snprintf(line2, sizeof(line2), "%s", wst.ssid);
    line1Color = kWarn;
  } else {
    snprintf(line1, sizeof(line1), "%s", wst.hasSaved ? "Not connected" : "No network set up");
    line1Color = kDanger;
  }
  drawCentered(sCanvas, line1, kCx, 118, &FreeSansBold15pt7b, 1, line1Color);
  if (line2[0] != '\0') drawCentered(sCanvas, line2, kCx, 144, nullptr, 1, kTextFaint);

  if (wst.state == wifiprov::State::Provisioning) {
    drawCentered(sCanvas, "then open any website", kCx, 164, nullptr, 1, kTextFaint);
    sCanvas->fillRoundRect(kWifiActionZone.x, kWifiActionZone.y, kWifiActionZone.w, kWifiActionZone.h, 16, kCard);
    sCanvas->drawRoundRect(kWifiActionZone.x, kWifiActionZone.y, kWifiActionZone.w, kWifiActionZone.h, 16, kCardBorder);
    drawCentered(sCanvas, "Waiting...", kWifiActionZone.x + kWifiActionZone.w / 2,
                 kWifiActionZone.y + kWifiActionZone.h / 2, &FreeSansBold15pt7b, 1, kTextFaint);
    drawBackButton("CANCEL");
  } else {
    // "ADD NETWORK"/"FORGET NETWORK" were measured too wide for this pill
    // (247px/305px vs the 216px pill, re-measured against the current font
    // -- fonts/FreeSansBold15pt7b.h is now DejaVu Sans Bold at 14pt, see
    // that file's banner comment -- same offline getTextBounds() check used
    // throughout this file) -- "WIFI" instead of "NETWORK" fits with real
    // margin (139px/192px).
    const char *actionLabel = wst.hasSaved ? "FORGET WIFI" : "ADD WIFI";
    sCanvas->fillRoundRect(kWifiActionZone.x, kWifiActionZone.y, kWifiActionZone.w, kWifiActionZone.h, 16, kCard);
    sCanvas->drawRoundRect(kWifiActionZone.x, kWifiActionZone.y, kWifiActionZone.w, kWifiActionZone.h, 16, kCardBorder);
    drawCentered(sCanvas, actionLabel, kWifiActionZone.x + kWifiActionZone.w / 2,
                 kWifiActionZone.y + kWifiActionZone.h / 2, &FreeSansBold15pt7b, 1,
                 wst.hasSaved ? kDanger : kAccent);
    drawBackButton("BACK");
  }
  sCanvas->flush();
}

// Draws one accessory bubble: a short bold tag on top (e.g. "ECC", "MTN"),
// a smaller value line below it (e.g. "+20 LB", "OFF"). `active` means
// "this is what the knob currently edits" -- the same meaning the old MODE
// pill's accent-colored text used to carry, just per-bubble now instead of
// for a single 3-way cycle. `pending` overrides the border to kWarn while a
// hold gesture is in progress but hasn't resolved yet (only the chains
// bubble ever passes true here -- see its hold-to-cycle-style gesture in
// update()), the same "warn = something's about to happen" color this file
// already uses for the armed/counting-down engage states below.
// ---------------------------------------------------------------------------
// Set count, rest timer, and drop sets. All three share one trigger: a
// pause of kSetEndIdleMs with no new rep while still loaded, which this
// project has no direct signal for (the Voltra reports a live rep COUNT,
// not a "cable set down"/"mid-pull" phase -- see linkFollower()'s sibling
// comments elsewhere in this project for the same kind of protocol-detail
// gap), so it's inferred the same way macfitnesscoaching.com's Voltra
// Remote guide's own SET_DOWN default effectively works: no rep change for
// a few seconds means the set ended. Matches that guide's "During a set"/
// "Drop sets" behavior: the rep count shown resets to 0 for each new set
// (not a running total since load), and set count + a rest timer appear
// once a pause is detected, replacing the rep count until the next pull.
static constexpr uint32_t kSetEndIdleMs = 4000;

static int sSetCount = 0;         // completed sets since this load started
static int sLastSetReps = 0;      // reps in the most recently completed set
static int sRepsAtSetStart = 0;   // st.reps value when the CURRENT set began
static int sLastRepsSeen = -1;    // last st.reps observed, -1 = not seen yet this load
static uint32_t sLastRepChangeMs = 0;
static bool sInRestPeriod = false;
static uint32_t sRestTimerStartMs = 0;
static bool sSetTrackingWasLoaded = false; // edge-detects a fresh load start, independent of draw()'s own sLastLoaded

// Drop-set runtime (not persisted -- resets every fresh load, like the set
// counters above): which configured drop we're on this set. dropCount==0 or
// DropMode==OFF means drops never fire regardless of this index.
static int sDropIndex = 0;

// Applies one drop: reduces base weight by dropPct percent (never below
// kMinTargetLb), same setBaseWeightLb() path the dial itself uses, so this
// is indistinguishable from the user having turned the dial down that much
// themselves -- no separate wire command. Deliberately does NOT touch
// chains/eccentric ("a drop clears modifiers... until you finish one full
// pull" in the reference guide) -- reproducing that exact bottom-of-next-
// rep timing would need extra live BLE writes keyed off a rep-phase signal
// this project doesn't have (see kSetEndIdleMs's comment), so this version
// only drops the base weight; chains/eccentric are left as you set them.
static void applyDrop(voltra::VoltraClient &voltraClient, const voltra::VoltraState &st, uint8_t dropPct) {
  int newBase = (int)lroundf(st.baseWeightLb * (1.0f - (float)dropPct / 100.0f));
  if (newBase < voltra::kMinTargetLb) newBase = voltra::kMinTargetLb;
  voltraClient.setBaseWeightLb(newBase);
  if (prefs::hapticsEnabled()) haptics::pulse(); // one buzz -- see haptics.h, no double-pulse primitive exists here
}

// Called unconditionally every update() tick (like the Factory Reset arm
// window and Settings reel-easing checks right where this is invoked from)
// so set/rest/drop state keeps advancing even while the knob is showing a
// Settings screen mid-set, not just while Screen::Workout is on-screen --
// the Voltra itself doesn't care what the knob's display is showing.
static void updateSetTracking(voltra::VoltraClient &voltraClient, uint32_t now, bool &changed) {
  const voltra::VoltraState &st = voltraClient.state();
  if (st.loaded && !sSetTrackingWasLoaded) {
    // Fresh load started -- see VoltraClient::engage()/armAutoLoad(), which
    // already reset st.reps to 0 themselves; this just resets the rest of
    // this file's own set/drop bookkeeping to match.
    sSetCount = 0;
    sLastSetReps = 0;
    sRepsAtSetStart = 0;
    sLastRepsSeen = st.reps;
    sLastRepChangeMs = now;
    sInRestPeriod = false;
    sDropIndex = 0;
  }
  sSetTrackingWasLoaded = st.loaded;
  if (!st.loaded) return;

  uint8_t dropCount = kDropCounts[sDropCountIndex];
  uint8_t dropPct = kDropPercents[sDropPercentIndex];

  if (sLastRepsSeen < 0) sLastRepsSeen = st.reps; // first tick seen while already loaded (e.g. reconnect mid-set)

  if (st.reps != sLastRepsSeen) {
    if (sInRestPeriod) {
      // A new pull after a rest period -- that rest period's set is fully
      // behind us now, so the next set starts counting from here.
      sInRestPeriod = false;
      sRepsAtSetStart = sLastRepsSeen;
    }
    sLastRepsSeen = st.reps;
    sLastRepChangeMs = now;
    changed = true;

    if (sDropModeIndex == 1 /* REP TARGET */ && sDropIndex < dropCount) {
      int curSetReps = st.reps - sRepsAtSetStart;
      uint8_t interval = kDropIntervals[sDropIntervalIndex];
      if (interval > 0 && curSetReps >= (int)interval * (sDropIndex + 1)) {
        applyDrop(voltraClient, st, dropPct);
        sDropIndex++;
      }
    }
  } else if (!sInRestPeriod && now - sLastRepChangeMs >= kSetEndIdleMs) {
    // No new rep for a while -- treat the set as finished (cable presumably
    // set down), same inference kSetEndIdleMs's comment above explains.
    sInRestPeriod = true;
    sRestTimerStartMs = now;
    sLastSetReps = st.reps - sRepsAtSetStart;
    sSetCount++;
    changed = true;
    if (sDropModeIndex == 2 /* SET DOWN */ && sDropIndex < dropCount) {
      applyDrop(voltraClient, st, dropPct);
      sDropIndex++;
    }
  }
}

static void drawAccessoryBubble(const Rect &r, const char *tag, const char *value, bool active, bool pending) {
  uint16_t border = pending ? kWarn : active ? kAccent : kCardBorder;
  uint16_t tagColor = pending ? kWarn : active ? kAccent : kTextPrimary;
  sCanvas->fillRoundRect(r.x, r.y, r.w, r.h, 14, kCard);
  sCanvas->drawRoundRect(r.x, r.y, r.w, r.h, 14, border);
  drawCentered(sCanvas, tag, r.x + r.w / 2, r.y + 15, &FreeSansBold15pt7b, 1, tagColor);
  drawCentered(sCanvas, value, r.x + r.w / 2, r.y + 33, &FreeSans9pt7b, 1, kTextFaint);
}

// Circular ring-progress dial for the Chains/Eccentric readouts -- replaces
// drawAccessoryBubble() above on the workout screen (still used as-is by
// nothing else now, kept only because its doc comment is referenced
// elsewhere; drawModifierRing() is the one actually called from draw()).
// A thin dim track (kCardBorder) all the way around, an accent-colored
// (or warn/pending) progress arc overlaid clockwise from 12 o'clock for
// frac of the circle, and the value centered inside -- same visual idiom
// as this file's main tick-ring gauge (drawGaugeStyleAt()), just circular
// and per-modifier instead of a 230-degree instrument arc. tag is drawn
// small and muted above the ring (not inside it -- see kModRingR's comment
// on why the ring itself is sized for the value text, not the label).
static void drawModifierRing(int16_t cx, int16_t cy, int16_t r, float frac, const char *tag,
                              const char *value, bool active, bool pending) {
  uint16_t ringColor = pending ? kWarn : active ? kAccent : kTextMuted;
  constexpr int16_t kRingThickness = 4;
  gfxaa::drawCircleAA(sCanvas, cx, cy, r, 1.3f, kCardBorder, kBg);
  if (frac > 0.0f) {
    float endDeg = 270.0f + 360.0f * frac; // 270 == due north in fillArc's 0=east/clockwise convention
    sCanvas->fillArc(cx, cy, r, r - kRingThickness, 270.0f, endDeg, ringColor);
  }
  drawCentered(sCanvas, tag, cx, cy - r - 12, &FreeSans9pt7b, 1, ringColor);
  drawCentered(sCanvas, value, cx, cy, &FreeSans9pt7b, 1, active ? kAccent : kTextPrimary);
}

// Fraction (0..1) of each modifier's own valid range the given state's
// current value represents -- same clamp ranges modeValueFraction() already
// uses for whichever mode is focused, just always computed for BOTH
// modifiers regardless of focus, since drawModifierRing() draws both rings
// unconditionally (unlike the old bubbles, the rings' progress arcs aren't
// only meaningful for the focused one).
static float eccentricFraction(const voltra::VoltraState &st) {
  // Eccentric can be negative (kMinEccentricLb is negative -- see
  // voltra_protocol.h), so its "fraction of range" is measured from that
  // negative minimum, same as modeValueFraction()'s Eccentric branch.
  float f = (float)(st.eccentricWeightLb - voltra::kMinEccentricLb) /
            (float)(voltra::kMaxEccentricLb - voltra::kMinEccentricLb);
  if (f < 0.0f) f = 0.0f;
  if (f > 1.0f) f = 1.0f;
  return f;
}
static float chainsFraction(const voltra::VoltraState &st) {
  int hi = (st.baseWeightLb < voltra::kMaxTargetLb) ? st.baseWeightLb : voltra::kMaxTargetLb;
  if (hi <= 0) return 0.0f;
  float f = (float)st.chainsWeightLb / (float)hi;
  if (f < 0.0f) f = 0.0f;
  if (f > 1.0f) f = 1.0f;
  return f;
}

// Weight-preset pill -- same rounded-box idiom as drawAccessoryBubble()
// above, but only 36px tall (see kPresetZone0/1's own comment for why),
// which isn't room for that helper's two stacked lines, so tag and value
// share a single centered line instead (e.g. "P1 90", "P2 --"). `pending`
// means a hold-to-store gesture on this slot is in progress but hasn't
// resolved yet, same kWarn-border meaning drawAccessoryBubble's `pending`
// carries.
static void drawPresetPill(const Rect &r, const char *tag, const char *valueStr, bool pending) {
  uint16_t border = pending ? kWarn : kCardBorder;
  uint16_t textColor = pending ? kWarn : kTextMuted;
  char buf[24];
  snprintf(buf, sizeof(buf), "%s %s", tag, valueStr);
  sCanvas->fillRoundRect(r.x, r.y, r.w, r.h, 10, kCard);
  sCanvas->drawRoundRect(r.x, r.y, r.w, r.h, 10, border);
  drawCentered(sCanvas, buf, r.x + r.w / 2, r.y + 24, &FreeSans9pt7b, 1, textColor);
}

static void draw(const voltra::VoltraState &st, int boardBatteryPercent, voltra::VoltraClient &voltraClient) {
  if (sScreen == Screen::Settings) { drawSettingsScreen(st); return; }
  if (sScreen == Screen::Theme) { drawThemeScreen(); return; }
  if (sScreen == Screen::GaugeStyle) { drawGaugeStyleScreen(); return; }
  if (sScreen == Screen::Wifi) { drawWifiScreen(); return; }
  if (sScreen == Screen::Bluetooth) { drawBluetoothScreen(st); return; }
  if (sScreen == Screen::Twin) { drawTwinScreen(st, voltraClient); return; }
  if (sScreen == Screen::Brightness) { drawBrightnessScreen(); return; }
  if (sScreen == Screen::AutoDim) { drawAutoDimScreen(); return; }
  if (sScreen == Screen::StepSize) { drawStepSizeScreen(); return; }
  if (sScreen == Screen::Vibration) { drawVibrationScreen(); return; }
  if (sScreen == Screen::Presets) { drawPresetsScreen(); return; }
  if (sScreen == Screen::Units) { drawUnitsScreen(); return; }
  if (sScreen == Screen::DropMode) { drawDropModeScreen(); return; }
  if (sScreen == Screen::DropCount) { drawDropCountScreen(); return; }
  if (sScreen == Screen::DropPercent) { drawDropPercentScreen(); return; }
  if (sScreen == Screen::DropInterval) { drawDropIntervalScreen(); return; }
  if (sScreen == Screen::About) { drawAboutScreen(); return; }
  if (sScreen == Screen::FirmwareUpdate) { drawFirmwareUpdateScreen(st.armed || st.loaded); return; }
  if (sScreen == Screen::FactoryReset) { drawFactoryResetScreen(); return; }

  sCanvas->fillScreen(kBg);

  // Bezel: a faint ring just inside the true panel edge frames the round
  // face instead of leaving content floating with no boundary. The
  // workout-state ring just inside it (armed = amber, loaded = red, doubles
  // as a big at-a-glance "you're mid-set" indicator that uses the round
  // shape) is only drawn when it means something, to keep idle/connected
  // state visually calm.
  gfxaa::drawCircleAA(sCanvas, kCx, kCy, kBezelR, 1.3f, kCardBorder, kBg);
  if (st.loaded) {
    sCanvas->fillArc(kCx, kCy, kBezelR - 4, kBezelR - 9, 0, 360, kDanger);
  } else if (st.armed) {
    sCanvas->fillArc(kCx, kCy, kBezelR - 4, kBezelR - 9, 0, 360, kWarn);
  }

  // Instrument-dial progress indicator -- see kTickOuterR's comment above
  // for why it has two gaps instead of running all the way around, and
  // drawGaugeStyleAt() below for the actual per-style drawing (Solid Arc /
  // Lit Ticks / Segmented, picked in Settings > Gauge Style).
  drawGaugeStyleAt(kCx, kCy, kTickOuterR, modeValueFraction(st), sGaugeStyleIndex);

  // Corner row: connection state as a small colored dot (see connColor())
  // plus a short text label (connLabelMain() -- "LINKED"/"LINKING"/"SYNC"/
  // "SCAN"/"OFFLINE") at upper-left, and the KNOB BOARD's own battery
  // (icon + percent) at upper-right. The dot-only version of this row
  // (matching macfitnesscoaching.com's Voltra Remote guide, which tucks an
  // unlabeled status glyph into the face) turned out too cryptic in
  // practice -- a color-coded dot with no key means nothing to someone who
  // hasn't memorized it, so the short label was added back to make the dot
  // self-explanatory at a glance. The Voltra's own battery percent and the
  // verbose elapsed-reconnect timer are still left off this screen
  // entirely -- full connection detail (and the longer connLabel()
  // wording) is still available on Settings > Bluetooth.
  {
    const int16_t rowY = 48;
    sCanvas->fillCircle(94, rowY, 5, connColor(st.connState));
    // Label starts just past the dot's right edge (94+5=99, +4px gap).
    // Its widest case (connLabelMain()'s "OFFLINE", 73px -- see that
    // function's own comment) ends at 103+73=176, a comfortable 8px short
    // of this row's knob-battery group, whose own worst case (a "100%"
    // reading) starts no earlier than x=184 -- computed the same way just
    // below. The two are independent (connection state and battery level
    // can both be at their worst simultaneously), so both worst cases are
    // checked against each other here, not just against the row's ends.
    drawLeftAligned(sCanvas, connLabelMain(st.connState), 103, rowY, &FreeSans9pt7b, connColor(st.connState));

    uint16_t knobBattColor = (boardBatteryPercent >= 0 && boardBatteryPercent <= 15) ? kDanger : kTextMuted;
    char knobBattBuf[8];
    if (boardBatteryPercent >= 0) snprintf(knobBattBuf, sizeof(knobBattBuf), "%d%%", boardBatteryPercent);
    else snprintf(knobBattBuf, sizeof(knobBattBuf), "--");
    uint16_t bw = textWidth(sCanvas, knobBattBuf, &FreeSans12pt7b);
    constexpr int16_t kGap = 6;
    int16_t bx = 266 - kBattIconW - kGap - bw;
    drawBatteryIcon(sCanvas, bx, rowY - kBattIconH / 2, boardBatteryPercent, knobBattColor);
    drawLeftAligned(sCanvas, knobBattBuf, bx + kBattIconW + kGap, rowY, &FreeSans12pt7b, knobBattColor);
  }

  // Display-only unit conversion (see the big-value block below for the
  // full explanation) -- computed once up here so both the modifier rings
  // and the big value can share it.
  bool showKg = prefs::units() == prefs::Units::Kg;

  // Unit tag, now above the big value rather than below it (see the big
  // value block below), matching macfitnesscoaching.com's Voltra Remote
  // guide's "LB"/"KG" placement.
  drawCentered(sCanvas, sMode == Mode::Chains ? "%" : (showKg ? "KG" : "LB"), kCx, 70, &FreeSans12pt7b, 1, kTextFaint);

  // Drop-set status dot: white = drops still available this set, grey =
  // this set's drops are used up, matching macfitnesscoaching.com's Voltra
  // Remote guide's white/grey drop-set icon states (its third state, red
  // for "a drop is imminent", isn't reproduced -- this project has no
  // rep-phase signal precise enough to say a drop is about to land a
  // fraction of a rep early, only that it landed; see updateSetTracking()).
  // Only drawn once a drop mode is actually turned on with at least one
  // drop configured, so an idle knob's face stays exactly as before.
  if (sDropModeIndex != 0 && kDropCounts[sDropCountIndex] > 0) {
    bool dropsUsedUp = sDropIndex >= kDropCounts[sDropCountIndex];
    sCanvas->fillCircle(kCx + 46, 70, 5, dropsUsedUp ? kTextMuted : kTextPrimary);
  }

  // Big value. Originally sized against FreeSans Bold at 51pt (bumped up
  // from an earlier 48pt); now DejaVu Sans Bold rendered at 40pt (see
  // fonts/FreeSansBold51pt7b.h's banner comment) -- DejaVu's digits run
  // wider per point than FreeSans's, so the point size was scaled down to
  // land back on the same worst-case measurement this comment has always
  // described: checked against the worst-case 3-digit-plus-sign string
  // ("-999"/"+999") offline against this font's actual glyph metrics
  // (tools/parse_font.py) -- width unaffected by this block's repositioning
  // above, only its y moved (178 -> 116, to leave room below for the
  // modifier rings -- see kModRingCy's comment). Chains weight is never
  // negative (see setChainsWeightLb's comment) so it never needs the "+"
  // sign eccentric does, and its max digit count (3, same as base/
  // eccentric) already fits this same worst-case check.
  int value = (sMode == Mode::Base) ? st.baseWeightLb
            : (sMode == Mode::Eccentric) ? st.eccentricWeightLb
            : st.chainsWeightLb;
  bool haveValue = (sMode == Mode::Base) ? st.haveBaseWeight
                 : (sMode == Mode::Eccentric) ? st.haveEccentricWeight
                 : st.haveChainsWeight;
  // Display-only unit conversion: the Voltra protocol, the encoder step
  // math, and every clamp/range constant all stay in lb throughout this
  // file and voltra_ble.cpp -- only the number actually drawn on screen
  // changes here, when the user has picked Kg on Settings > Units. Rounded
  // to the nearest whole kg (no decimal) rather than showing a fraction, to
  // stay inside the same "-999"/"+999" 4-character width budget this big
  // font was measured against -- a kg value is always numerically <= its lb
  // equivalent, so this can't regress that check. Chains/Inverse/Mountain's
  // shared amount is the one exception: it's always clamped to 0..base
  // weight (see setChainsWeightLb's own clamp), which is exactly what a
  // percent-of-base-weight already is, so it's shown as that percentage
  // instead -- matching how chains/accommodating-resistance amounts are
  // actually talked about in strength training ("20% chains") and staying
  // meaningful across a base-weight change instead of quietly becoming a
  // different fraction of it. Units (Settings > Units) don't apply to a
  // percentage, so showKg is skipped entirely for this branch.
  int displayValue = (sMode == Mode::Chains)
      ? (st.baseWeightLb > 0 ? (int)lroundf(100.0f * value / st.baseWeightLb) : 0)
      : showKg ? (int)lroundf(value * 0.45359237f) : value;
  // While twinned, the Voltra's own on-screen weight shows the COMBINED
  // total across both cables (confirmed on real hardware: dialing to 30 lb
  // on the knob makes the Voltra's own display show 60) -- a device-side
  // display convention, not a protocol bug. Only Base weight is twin-
  // synced this way (eccentric/chains stay per-unit settings), so double
  // just the number drawn here to match what the Voltra itself shows. The
  // underlying `value` above -- used for the gauge fraction and for
  // storing/recalling presets -- is intentionally left alone, so encoder
  // steps, clamps, and presets keep operating on the real per-unit
  // register value. kMaxTargetLb is 200, so the doubled max (400) still
  // fits well inside the "-999"/"+999" width budget noted above.
  if (sMode == Mode::Base && st.twinState == voltra::TwinState::Twinned) {
    displayValue *= 2;
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "%s%d", (sMode == Mode::Eccentric && displayValue > 0) ? "+" : "", displayValue);
  // Color: faint if there's no value yet, red while loaded (matching
  // macfitnesscoaching.com's Voltra Remote guide's white-when-set/
  // red-when-loaded convention -- his ring around the edge does the same;
  // this project's ring instead keeps its own accent color and uses this
  // number for the loaded cue), primary/white otherwise. st.loaded is
  // global (not per-mode), same as the bezel ring above, so this applies
  // whichever of Base/Eccentric/Chains is currently focused.
  uint16_t bigValueColor = !haveValue ? kTextFaint : st.loaded ? kDanger : kTextPrimary;
  drawCentered(sCanvas, buf, kCx, 116, &FreeSansBold51pt7b, 1, bigValueColor);

  // Chains/Eccentric ring-dials, stacked directly under the big value (see
  // kModRingLeftCx/RightCx/Cy/R above) -- replaces the old rectangular
  // accessory bubbles with a circular ring-progress dial per modifier,
  // matching macfitnesscoaching.com's Voltra Remote guide. Each always
  // shows its own live value regardless of focus -- unlike a single 3-way
  // cycle, which could only ever show ONE of Base/Eccentric/Chains' values
  // at a time -- so both an eccentric offset and a chains-style weight can
  // be dialed in and visibly active at once (feature request #2, "Mountain
  // mode/simultaneous accessories"). Tapping a ring focuses it for the knob
  // to edit (or returns focus to base weight if it's already focused), the
  // same tap-to-cycle meaning the old pill's accent-colored text used to
  // carry -- see update()'s touch handling, unchanged (it keys off
  // kEccBubbleZone/kChainsBubbleZone, which still bound these rings' tap
  // targets). The chains ring also has a hold gesture (see
  // VoltraClient::setChainsStyle) to cycle which of Chains/Inverse/Mountain
  // its shared amount currently produces. Values are shown as bare numbers
  // (no "LB"/"KG" suffix) -- the unit tag above the big value already sets
  // that context for the whole screen, same convention the weight-preset
  // pills below already use.
  {
    char eccBuf[16];
    if (!st.haveEccentricWeight || st.eccentricWeightLb == 0) {
      snprintf(eccBuf, sizeof(eccBuf), "OFF");
    } else {
      int eccDisplay = showKg ? (int)lroundf(st.eccentricWeightLb * 0.45359237f) : st.eccentricWeightLb;
      snprintf(eccBuf, sizeof(eccBuf), "%s%d", eccDisplay > 0 ? "+" : "", eccDisplay);
    }
    drawModifierRing(kModRingLeftCx, kModRingCy, kModRingR, eccentricFraction(st), "ECC", eccBuf,
                      sMode == Mode::Eccentric, false);

    // Tag reflects whichever style is currently selected -- "CHN" (plain
    // chains) is the default/neither-flag-set state, not a separate wire
    // value; see voltra_protocol.h's PARAM_FITNESS_INVERSE_CHAIN/
    // PARAM_MOUNTAIN_CURVE comment.
    const char *chainsTag = (st.chainsStyle == voltra::ChainsStyle::Inverse) ? "INV"
                           : (st.chainsStyle == voltra::ChainsStyle::Mountain) ? "MTN"
                           : "CHN";
    char chainsBuf[16];
    if (!st.haveChainsWeight || st.chainsWeightLb == 0) {
      snprintf(chainsBuf, sizeof(chainsBuf), "OFF");
    } else {
      // Shown as a percentage of base weight, not raw lb -- same reasoning
      // as the big value's own Chains branch above (setChainsWeightLb's
      // clamp already keeps this 0..base weight, i.e. 0..100%). Appended
      // directly here, unlike the big value above, since this ring is
      // always visible regardless of focus -- it can't rely on the top
      // unit tag, which only shows "%" while Chains is the FOCUSED mode.
      int chainsPct = st.baseWeightLb > 0 ? (int)lroundf(100.0f * st.chainsWeightLb / st.baseWeightLb) : 0;
      snprintf(chainsBuf, sizeof(chainsBuf), "%d%%", chainsPct);
    }
    bool chainsPending = sChainsPressActive && !sChainsHoldFired;
    drawModifierRing(kModRingRightCx, kModRingCy, kModRingR, chainsFraction(st), chainsTag, chainsBuf,
                      sMode == Mode::Chains, chainsPending);
  }

  // Settings gear button: opens the settings menu (Screen::Settings), which
  // now holds both Theme (what a three-color ring used to control directly
  // here) and Wi-Fi. Drawn as a plain gear -- body ring plus 8 radial
  // teeth, reusing the same cos/sin radial-line technique as the tick-ring
  // gauge above -- in a single neutral color rather than the old ring's
  // three theme-preview colors, since this button no longer changes the
  // theme by itself and a gear is the more universally understood "settings"
  // idiom anyway. The "SETTINGS" caption below it, styled exactly like the
  // "ACCESSORIES" caption above the accessory bubbles, is what marks this
  // as a real labeled control rather than decoration. Tapping it (see
  // kSettingsTapZone handling in update(), a larger invisible zone around
  // this same spot) opens the settings menu.
  {
    constexpr int kTeeth = 8;
    sCanvas->fillCircle(kGearCx, kGearCy, kGearR, kTextMuted);
    for (int i = 0; i < kTeeth; i++) {
      float rad = (360.0f * i / kTeeth) * kPi / 180.0f;
      float c = cosf(rad), s = sinf(rad);
      int16_t x0 = kGearCx + (int16_t)(c * kGearR);
      int16_t y0 = kGearCy + (int16_t)(s * kGearR);
      int16_t x1 = kGearCx + (int16_t)(c * kGearToothOuterR);
      int16_t y1 = kGearCy + (int16_t)(s * kGearToothOuterR);
      gfxaa::drawLineAA(sCanvas, x0, y0, x1, y1, kTextMuted, kBg);
      // Same perpendicular-offset thickening as the tick-ring gauge's lit
      // ticks -- a single 1px radial line reads as a spindly spoke, not a
      // gear tooth.
      float px = -s, py = c;
      int16_t ox = (int16_t)(px >= 0 ? px + 0.5f : px - 0.5f);
      int16_t oy = (int16_t)(py >= 0 ? py + 0.5f : py - 0.5f);
      if (ox == 0 && oy == 0) { ox = 1; oy = 0; }
      gfxaa::drawLineAA(sCanvas, x0 + ox, y0 + oy, x1 + ox, y1 + oy, kTextMuted, kBg);
      gfxaa::drawLineAA(sCanvas, x0 - ox, y0 - oy, x1 - ox, y1 - oy, kTextMuted, kBg);
    }
    sCanvas->fillCircle(kGearCx, kGearCy, kGearR - 5, kBg); // punch the gear's center hole
  }
  drawCentered(sCanvas, "SETTINGS", kGearCx, kGearCy + kGearToothOuterR + 12, nullptr, 1, kTextFaint);

  // Status/reps line: rep count while loaded, arm/countdown status while
  // armed. This row used to draw nothing at all while idle -- an earlier
  // version filled it with a "turn knob to set weight" hint, but turning the
  // knob to set the weight is the one interaction this whole screen exists
  // to invite; spelling it out cost a full row of otherwise-idle screen
  // space for something the big dial-style value in the middle already
  // implies. That freed-up idle space is now the two weight-preset pills
  // (kPresetZone0/1, weight_presets.h) instead of staying blank -- tap a
  // pill to recall its stored number into whichever value is currently
  // focused, hold to store the current one there. Still hidden the instant
  // a set starts (loaded) or auto-load is armed, same as the REPS/ARMED text
  // that replaces them then, so the presets can never be mistaken for live
  // workout state.
  char statusBuf[32] = "";
  uint16_t statusColor = kTextMuted;
  if (st.loaded) {
    if (sInRestPeriod) {
      // Between sets (see updateSetTracking()): show what the just-finished
      // set totaled and a rest timer counting UP since it ended, in place of
      // the live rep count -- matches macfitnesscoaching.com's Voltra Remote
      // guide's "During a set"/rest-timer behavior. Resets back to the plain
      // "REPS n" line below the instant a new pull is detected.
      uint32_t restMs = millis() - sRestTimerStartMs;
      unsigned restS = restMs / 1000;
      // Plain ASCII separators, not a UTF-8 middle-dot -- this project's
      // GFXfont headers only cover the printable ASCII range (32-126, see
      // tools/parse_font.py's own "first=32 last=126" output on any of
      // them), so a non-ASCII glyph here would just fail to render.
      snprintf(statusBuf, sizeof(statusBuf), "SET %d - %d REPS - %u:%02u",
               sSetCount, sLastSetReps, restS / 60, restS % 60);
    } else {
      // The running total since this load started -- always matches the
      // Voltra's own on-device rep count exactly (st.reps only resets when
      // a fresh load begins, never mid-set -- see VoltraState::reps's own
      // comment in voltra_ble.h). This used to show st.reps - sRepsAtSetStart
      // (reps within the CURRENT inferred set only), but that baseline gets
      // pushed forward every time kSetEndIdleMs's idle-timeout heuristic
      // (see updateSetTracking()'s own comment on why it's only an
      // inference, not a real "cable set down" signal) decides a set just
      // ended -- including a false positive from an ordinary pause between
      // reps mid-set. When that happened, the next real rep made the
      // displayed count drop back down (as low as "REPS 1") even though the
      // Voltra's own screen kept counting the same set the whole time.
      // Showing the raw total here instead can never regress like that --
      // sRepsAtSetStart/sLastSetReps are still used below for the
      // "SET n - X reps" BETWEEN-set summary, which isn't live and was
      // never what diverged from the Voltra's screen.
      snprintf(statusBuf, sizeof(statusBuf), "REPS  %d", st.reps);
    }
    statusColor = kTextPrimary;
  } else if (st.armed) {
    if (st.guidedState == voltra::kGuidedStateCountdown && st.guidedCountdownMs >= 0) {
      snprintf(statusBuf, sizeof(statusBuf), "PULL! %.1fs", st.guidedCountdownMs / 1000.0f);
    } else if (st.guidedState == voltra::kGuidedStateEngaging) {
      snprintf(statusBuf, sizeof(statusBuf), "ENGAGING...");
    } else {
      snprintf(statusBuf, sizeof(statusBuf), "ARMED - pull to start");
    }
    statusColor = kWarn;
  }
  if (statusBuf[0] != '\0') {
    drawCentered(sCanvas, statusBuf, kCx, 254, &FreeSans12pt7b, 1, statusColor);
  } else if (prefs::presetsEnabled()) {
    for (uint8_t i = 0; i < 2; i++) {
      const Rect &z = (i == 0) ? kPresetZone0 : kPresetZone1;
      char tag[4];
      snprintf(tag, sizeof(tag), "P%u", (unsigned)(i + 1));
      char valBuf[12];
      if (!wpresets::hasValue(i)) {
        snprintf(valBuf, sizeof(valBuf), "--");
      } else {
        int v = wpresets::valueLb(i);
        int dv = showKg ? (int)lroundf(v * 0.45359237f) : v;
        snprintf(valBuf, sizeof(valBuf), "%d", dv);
      }
      bool pending = sPresetPressActive[i] && !sPresetHoldFired[i];
      drawPresetPill(z, tag, valBuf, pending);
    }
  }

  // Engage/disengage/cancel button. Tap: load now (idle) or stop/cancel
  // (loaded or armed). Hold from idle: arm auto-load -- there's no physical
  // button on this board to map that to, so it's a touch-and-hold gesture
  // on this same button instead (see the press-tracking in update()).
  uint16_t btnColor = st.loaded ? kDanger : st.armed ? kWarn : kAccent;
  const char *btnLabel = st.loaded ? "STOP" : st.armed ? "CANCEL" : "START";
  uint16_t btnTextColor = RGB565(10, 11, 13); // dark text on the bright accent/warn/danger fill reads better than white
  sCanvas->fillRoundRect(kEngageZone.x, kEngageZone.y, kEngageZone.w, kEngageZone.h, 22, btnColor);
  drawCentered(sCanvas, btnLabel, kEngageZone.x + kEngageZone.w / 2, kEngageZone.y + kEngageZone.h / 2,
               &FreeSansBold15pt7b, 1, btnTextColor);

  int hintY = kEngageZone.y + kEngageZone.h + 18;
  if (!st.loaded && !st.armed) {
    if (sPressActive && !sHoldFired) {
      drawCentered(sCanvas, "hold to arm...", kCx, hintY, nullptr, 1, kWarn);
    } else {
      drawCentered(sCanvas, "tap: load   hold: arm", kCx, hintY, nullptr, 1, kTextFaint);
    }
  }

  sCanvas->flush();
}

void begin(Arduino_Canvas *canvas) {
  sCanvas = canvas;
  loadTheme();
  loadGaugeStyle();
  loadDropMode();
  loadDropCount();
  loadDropPercent();
  loadDropInterval();
  draw(voltra::VoltraState{}, -1, voltra::gVoltra); // sScreen starts on Workout, so the Twin-screen dispatch (which needs a real client) is never reached by this initial blank draw
}

void update(int32_t encoderDelta, const touch::TouchPoint &tpRaw, voltra::VoltraClient &voltraClient,
            int boardBatteryPercent) {
  bool changed = false;
  // Set alongside `changed = true` at TAP-driven branches only (never the
  // encoder, and never the periodic redraw timers below) -- mirrors the
  // `changed` flag's own pattern so a haptic pulse fires once per
  // meaningful tap rather than being sprinkled at every call site by hand.
  bool didTapAction = false;

  // The encoder adjusts the dial weight on the workout screen (scaled by
  // the user's step-size preference -- see prefs::stepSizeLb(); this
  // multiplies encoder.cpp's raw +/-1-per-detent delta rather than
  // replacing it, so the underlying encoder driver is untouched), or
  // scrolls the focused item on the Settings list -- it's otherwise
  // ignored on every other settings sub-screen (those are touch-only; see
  // the cycle-screen prev/next buttons below).
  if (sScreen == Screen::Workout && encoderDelta != 0) {
    changed = true;
    const auto &st = voltraClient.state();
    int step = (int)encoderDelta * (int)prefs::stepSizeLb();
    if (sMode == Mode::Base) {
      voltraClient.setBaseWeightLb(st.baseWeightLb + step);
    } else if (sMode == Mode::Eccentric) {
      voltraClient.setEccentricWeightLb(st.eccentricWeightLb + step);
    } else {
      // Dialed in percentage points of base weight, not raw lb -- see the
      // big-value draw() block's own comment on why (matches how chains
      // amounts are actually talked about in strength training, and keeps
      // meaning the same fraction of the load across a base-weight change
      // instead of silently becoming a different one). kChainsPercentStep
      // is its own fixed increment rather than reusing prefs::stepSizeLb()
      // -- that preference's values (e.g. 1/2/5/10) are calibrated for lb
      // jumps on Base/Eccentric, and conflating the two under one knob
      // would make Step Size's meaning depend on which mode is focused.
      constexpr int kChainsPercentStep = 1;
      int curPct = st.baseWeightLb > 0 ? (int)lroundf(100.0f * st.chainsWeightLb / st.baseWeightLb) : 0;
      int newPct = curPct + (int)encoderDelta * kChainsPercentStep;
      if (newPct < 0) newPct = 0;
      if (newPct > 100) newPct = 100;
      voltraClient.setChainsWeightLb((int)lroundf(st.baseWeightLb * newPct / 100.0f));
    }
  } else if (sScreen == Screen::Settings && encoderDelta != 0) {
    int newFocus = (int)sSettingsFocus + (int)encoderDelta;
    if (newFocus < 0) newFocus = 0;
    if (newFocus > (int)kSettingsItemCount - 1) newFocus = kSettingsItemCount - 1;
    sSettingsFocus = (int8_t)newFocus;
    changed = true;
  } else if (sScreen == Screen::Twin && encoderDelta != 0 && voltraClient.twinCandidateCount() > 1) {
    // Wraps (unlike Settings' clamp above) -- this is a short cyclic list of
    // equally-valid choices, not an ordered menu with a meaningful start/end,
    // so looping past the last candidate back to the first reads as normal
    // rather than surprising.
    int count = (int)voltraClient.twinCandidateCount();
    int newIdx = ((int)sTwinCandidateIdx + (int)encoderDelta) % count;
    if (newIdx < 0) newIdx += count;
    sTwinCandidateIdx = (uint8_t)newIdx;
    changed = true;
  }

  touch::TouchPoint tp = tpRaw;
  uint32_t now = millis();

  // Set count / rest timer / drop sets -- see updateSetTracking()'s own
  // comment for why this runs unconditionally, every tick, regardless of
  // which screen is currently on-screen.
  updateSetTracking(voltraClient, now, changed);

  // Factory Reset's arm window expires on its own even with no further
  // input at all (not just a tap elsewhere) -- checked here, every
  // update() call, rather than only inside the touch branches below.
  if (sFactoryResetArmed && now - sFactoryResetArmedAtMs >= kFactoryResetArmWindowMs) {
    sFactoryResetArmed = false;
    changed = true;
  }
  // Ease the settings list's animated scroll position toward whatever
  // sSettingsFocus was just set to (by the encoder branch above, or by a
  // fresh Settings entry below) -- checked unconditionally, every tick,
  // like the arm-window check just above, so the slide keeps advancing and
  // redrawing across several update() calls after the encoder input that
  // triggered it, not just the one call where sSettingsFocus changed.
  if (sScreen == Screen::Settings) {
    float target = (float)sSettingsFocus;
    float diff = target - sSettingsAnimY;
    if (fabsf(diff) > kSettingsAnimSnapEps) {
      sSettingsAnimY += diff * kSettingsAnimEase;
      if (fabsf(target - sSettingsAnimY) <= kSettingsAnimSnapEps) sSettingsAnimY = target;
      changed = true;
    }
    // Only reveal the new item's value/status line once the reel has
    // actually arrived there (sSettingsAnimY caught back up to target) --
    // see sSettingsValueFocus's own comment for why.
    if (sSettingsAnimY == target) {
      sSettingsValueFocus = (uint8_t)sSettingsFocus;
    }
  }
  if (tp.touched) {
    int16_t x, y;
    mapTouch(tp.x, tp.y, x, y);
    bool isNewTap = !sPrevTouched;
    if (sScreen == Screen::Workout) {
      if (isNewTap) {
        if (inRect(kEccBubbleZone, x, y)) {
          // Plain instant tap, no hold gesture -- focus this bubble's value
          // for the knob to edit, or return to base weight if it's already
          // focused. Same tap-to-cycle meaning the old MODE pill's
          // accent-colored text used to carry, just as a direct toggle on
          // its own tap target instead of a step in a 3-way cycle.
          sMode = (sMode == Mode::Eccentric) ? Mode::Base : Mode::Eccentric;
          changed = true;
          didTapAction = true;
        } else if (inRect(kChainsBubbleZone, x, y)) {
          // Don't act yet -- same tap-vs-hold deferral as kEngageZone below,
          // just tracked with its own state (sChainsPressActive/etc.): a
          // tap here focuses the shared chains amount (resolved on release,
          // below), a hold instead cycles Chains/Inverse/Mountain (resolved
          // mid-press, in the sChainsPressActive block right after this
          // one) -- see VoltraClient::setChainsStyle()'s comment.
          sChainsPressActive = true;
          sChainsPressStartMs = now;
          sChainsHoldFired = false;
        } else if (sMode != Mode::Base && inRect(kBigValueZone, x, y)) {
          // Tap the big number itself to return to editing base weight --
          // see kBigValueZone's own comment above for why this exists
          // alongside (not instead of) the original same-ring-toggle path.
          // Guarded on sMode != Base so tapping the weight while it's
          // already the focused value is a no-op, not a wasted haptic
          // buzz (didTapAction below fires exactly one pulse per real
          // change).
          sMode = Mode::Base;
          changed = true;
          didTapAction = true;
        } else if (inRect(kSettingsTapZone, x, y)) {
          sScreen = Screen::Settings;
          sSettingsFocus = 0;
          sSettingsAnimY = 0.0f;     // snap the reel to match -- no animating in from last visit
          sSettingsValueFocus = 0;   // and show item 0's value immediately, not the last-shown one
          changed = true;
          didTapAction = true;
        } else if (inRect(kEngageZone, x, y)) {
          // Don't act yet -- wait to see whether this turns into a tap (acts
          // on release) or a hold (acts mid-press, once kHoldMs elapses).
          // This is a touchscreen stand-in for the reference implementation's
          // physical-button tap-vs-hold classifier (app_logic.h/.cpp), since
          // this board's knob has no push-button.
          sPressActive = true;
          sPressStartMs = now;
          sHoldFired = false;
          // No haptic pulse here yet -- we don't know if this is a tap or a
          // hold until it resolves (release, or kHoldMs elapsed below), and
          // pulsing both here AND on that resolution would double-buzz a
          // single quick tap.
        } else if (inRect(kPresetZone0, x, y) || inRect(kPresetZone1, x, y)) {
          // Same don't-act-yet deferral as kEngageZone/kChainsBubbleZone
          // above. Only a live tap target while these pills are actually
          // drawn (idle -- see draw()'s status-row comment); gated on the
          // same st.loaded/st.armed check that hides them there, so a touch
          // that lands in this screen space during an active set (where the
          // REPS/ARMED text lives instead) can't be mistaken for a preset
          // press.
          const auto &pst = voltraClient.state();
          if (!pst.loaded && !pst.armed && prefs::presetsEnabled()) {
            uint8_t slot = inRect(kPresetZone0, x, y) ? 0 : 1;
            sPresetPressActive[slot] = true;
            sPresetPressStartMs[slot] = now;
            sPresetHoldFired[slot] = false;
          }
        }
      }
      if (sChainsPressActive) {
        if (!inRect(kChainsBubbleZone, x, y)) {
          // Finger slid off the bubble -- cancel so lifting off-zone can't fire a tap.
          sChainsPressActive = false;
          changed = true;
        } else if (!sChainsHoldFired && now - sChainsPressStartMs >= kHoldMs) {
          sChainsHoldFired = true;
          const auto &st = voltraClient.state();
          voltra::ChainsStyle next = (st.chainsStyle == voltra::ChainsStyle::Chains) ? voltra::ChainsStyle::Inverse
                                    : (st.chainsStyle == voltra::ChainsStyle::Inverse) ? voltra::ChainsStyle::Mountain
                                    : voltra::ChainsStyle::Chains;
          voltraClient.setChainsStyle(next);
          changed = true;
          didTapAction = true;
        } else if (!sChainsHoldFired) {
          changed = true; // redraw so the bubble's pending/kWarn border stays live
        }
      }
      if (sPressActive) {
        if (!inRect(kEngageZone, x, y)) {
          // Finger slid off the button -- cancel so lifting off-zone can't fire a tap.
          sPressActive = false;
          changed = true;
        } else if (!sHoldFired && now - sPressStartMs >= kHoldMs) {
          sHoldFired = true;
          const auto &st = voltraClient.state();
          if (!st.loaded && !st.armed) voltraClient.armAutoLoad();
          changed = true;
          didTapAction = true;
        } else if (!sHoldFired) {
          changed = true; // redraw so the "hold to arm..." hint / progress stays live
        }
      }
      for (uint8_t i = 0; i < 2; i++) {
        if (!sPresetPressActive[i]) continue;
        const Rect &z = (i == 0) ? kPresetZone0 : kPresetZone1;
        if (!inRect(z, x, y)) {
          // Finger slid off the pill -- cancel so lifting off-zone can't fire a tap.
          sPresetPressActive[i] = false;
          changed = true;
        } else if (!sPresetHoldFired[i] && now - sPresetPressStartMs[i] >= kHoldMs) {
          sPresetHoldFired[i] = true;
          const auto &st = voltraClient.state();
          int focusedValue = (sMode == Mode::Base) ? st.baseWeightLb
                            : (sMode == Mode::Eccentric) ? st.eccentricWeightLb
                            : st.chainsWeightLb;
          wpresets::store(i, focusedValue);
          changed = true;
          didTapAction = true;
        } else if (!sPresetHoldFired[i]) {
          changed = true; // redraw so the pill's pending/kWarn border stays live
        }
      }
    } else if (isNewTap) {
      // Settings menu tree: plain single-tap navigation/actions, no
      // hold-gesture semantics (those only exist for the workout screen's
      // engage button and chains bubble above).
      if (sScreen == Screen::Settings) {
        if (inRect(kSettingsFocusZone, x, y)) {
          sScreen = kSettingsItems[sSettingsFocus].screen;
          changed = true;
          didTapAction = true;
        } else if (inRect(kBackZone, x, y)) {
          sScreen = Screen::Workout;
          changed = true;
          didTapAction = true;
        }
      } else if (sScreen == Screen::Theme) {
        if (inRect(kThemePrevZone, x, y)) {
          prevTheme();
          changed = true;
          didTapAction = true;
        } else if (inRect(kThemeNextZone, x, y)) {
          cycleTheme();
          changed = true;
          didTapAction = true;
        } else if (inRect(kBackZone, x, y)) {
          sScreen = Screen::Settings;
          changed = true;
          didTapAction = true;
        }
      } else if (sScreen == Screen::GaugeStyle) {
        if (inRect(kThemePrevZone, x, y)) {
          prevGaugeStyle();
          changed = true;
          didTapAction = true;
        } else if (inRect(kThemeNextZone, x, y)) {
          cycleGaugeStyle();
          changed = true;
          didTapAction = true;
        } else if (inRect(kBackZone, x, y)) {
          sScreen = Screen::Settings;
          changed = true;
          didTapAction = true;
        }
      } else if (sScreen == Screen::Wifi) {
        wifiprov::Status wst = wifiprov::status();
        if (wst.state == wifiprov::State::Provisioning) {
          if (inRect(kBackZone, x, y)) { // this slot reads "CANCEL" while provisioning
            wifiprov::cancelProvisioning();
            changed = true;
            didTapAction = true;
          }
        } else if (inRect(kWifiActionZone, x, y)) {
          if (wst.hasSaved) {
            wifiprov::forgetNetwork();
          } else {
            wifiprov::startProvisioning();
          }
          changed = true;
          didTapAction = true;
        } else if (inRect(kBackZone, x, y)) {
          sScreen = Screen::Settings;
          changed = true;
          didTapAction = true;
        }
      } else if (sScreen == Screen::Bluetooth) {
        if (inRect(kWifiActionZone, x, y)) {
          voltraClient.disconnectAndRescan();
          changed = true;
          didTapAction = true;
        } else if (inRect(kBackZone, x, y)) {
          sScreen = Screen::Settings;
          changed = true;
          didTapAction = true;
        }
      } else if (sScreen == Screen::Twin) {
        const auto &tst = voltraClient.state();
        if (inRect(kWifiActionZone, x, y)) {
          if (tst.twinState == voltra::TwinState::Twinned) {
            voltraClient.untwin();
          } else if (tst.twinPendingAcceptance) {
            voltraClient.acceptPendingTwin();
          } else if (tst.connState == voltra::ConnState::Ready && !voltraClient.twinDiscoveryInProgress()) {
            // The twinDiscoveryInProgress() guard matters here, not just as
            // a visual nicety: without it, a candidate that latched mid-scan
            // (twinCandidateCount() can already be >0 while the scan is
            // still running and possibly still finding more) could be
            // twinned with while the NimBLEScan is still active, racing the
            // scan against linkFollower()'s own brief second connection.
            // drawTwinScreen() shows "WAIT..." during this window for the
            // same reason -- this makes the tap actually match that state.
            if (voltraClient.twinCandidateCount() > 0) {
              voltraClient.twinWithCandidate(sTwinCandidateIdx);
            } else {
              sTwinCandidateIdx = 0;
              sTwinHasScanned = true;
              voltraClient.startTwinDiscovery();
            }
          }
          changed = true;
          didTapAction = true;
        } else if (inRect(kBackZone, x, y)) {
          sScreen = Screen::Settings;
          changed = true;
          didTapAction = true;
        }
      } else if (sScreen == Screen::Brightness) {
        if (inRect(kThemePrevZone, x, y)) {
          prefs::cycleBrightnessPrev();
          changed = true;
          didTapAction = true;
        } else if (inRect(kThemeNextZone, x, y)) {
          prefs::cycleBrightness();
          changed = true;
          didTapAction = true;
        } else if (inRect(kBackZone, x, y)) {
          sScreen = Screen::Settings;
          changed = true;
          didTapAction = true;
        }
      } else if (sScreen == Screen::AutoDim) {
        if (inRect(kThemePrevZone, x, y)) {
          prefs::cycleAutoDimPrev();
          changed = true;
          didTapAction = true;
        } else if (inRect(kThemeNextZone, x, y)) {
          prefs::cycleAutoDim();
          changed = true;
          didTapAction = true;
        } else if (inRect(kBackZone, x, y)) {
          sScreen = Screen::Settings;
          changed = true;
          didTapAction = true;
        }
      } else if (sScreen == Screen::StepSize) {
        if (inRect(kThemePrevZone, x, y)) {
          prefs::cycleStepSizePrev();
          changed = true;
          didTapAction = true;
        } else if (inRect(kThemeNextZone, x, y)) {
          prefs::cycleStepSize();
          changed = true;
          didTapAction = true;
        } else if (inRect(kBackZone, x, y)) {
          sScreen = Screen::Settings;
          changed = true;
          didTapAction = true;
        }
      } else if (sScreen == Screen::Vibration) {
        // Only two states, so prev and next both just toggle -- there's no
        // meaningful "direction" between ON and OFF.
        if (inRect(kThemePrevZone, x, y) || inRect(kThemeNextZone, x, y)) {
          prefs::toggleHaptics();
          changed = true;
          didTapAction = true; // fires with the new setting already applied -- audible confirmation either way
        } else if (inRect(kBackZone, x, y)) {
          sScreen = Screen::Settings;
          changed = true;
          didTapAction = true;
        }
      } else if (sScreen == Screen::Presets) {
        // Same two-state toggle as Vibration above.
        if (inRect(kThemePrevZone, x, y) || inRect(kThemeNextZone, x, y)) {
          prefs::togglePresets();
          changed = true;
          didTapAction = true;
        } else if (inRect(kBackZone, x, y)) {
          sScreen = Screen::Settings;
          changed = true;
          didTapAction = true;
        }
      } else if (sScreen == Screen::Units) {
        if (inRect(kThemePrevZone, x, y) || inRect(kThemeNextZone, x, y)) {
          prefs::cycleUnits();
          changed = true;
          didTapAction = true;
        } else if (inRect(kBackZone, x, y)) {
          sScreen = Screen::Settings;
          changed = true;
          didTapAction = true;
        }
      } else if (sScreen == Screen::DropMode) {
        if (inRect(kThemePrevZone, x, y)) {
          prevDropMode();
          changed = true;
          didTapAction = true;
        } else if (inRect(kThemeNextZone, x, y)) {
          cycleDropMode();
          changed = true;
          didTapAction = true;
        } else if (inRect(kBackZone, x, y)) {
          sScreen = Screen::Settings;
          changed = true;
          didTapAction = true;
        }
      } else if (sScreen == Screen::DropCount) {
        if (inRect(kThemePrevZone, x, y)) {
          prevDropCount();
          changed = true;
          didTapAction = true;
        } else if (inRect(kThemeNextZone, x, y)) {
          cycleDropCount();
          changed = true;
          didTapAction = true;
        } else if (inRect(kBackZone, x, y)) {
          sScreen = Screen::Settings;
          changed = true;
          didTapAction = true;
        }
      } else if (sScreen == Screen::DropPercent) {
        if (inRect(kThemePrevZone, x, y)) {
          prevDropPercent();
          changed = true;
          didTapAction = true;
        } else if (inRect(kThemeNextZone, x, y)) {
          cycleDropPercent();
          changed = true;
          didTapAction = true;
        } else if (inRect(kBackZone, x, y)) {
          sScreen = Screen::Settings;
          changed = true;
          didTapAction = true;
        }
      } else if (sScreen == Screen::DropInterval) {
        if (inRect(kThemePrevZone, x, y)) {
          prevDropInterval();
          changed = true;
          didTapAction = true;
        } else if (inRect(kThemeNextZone, x, y)) {
          cycleDropInterval();
          changed = true;
          didTapAction = true;
        } else if (inRect(kBackZone, x, y)) {
          sScreen = Screen::Settings;
          changed = true;
          didTapAction = true;
        }
      } else if (sScreen == Screen::About) {
        if (inRect(kBackZone, x, y)) {
          sScreen = Screen::Settings;
          changed = true;
          didTapAction = true;
        }
      } else if (sScreen == Screen::FirmwareUpdate) {
        otaupdate::Status ost = otaupdate::status();
        if (inRect(kWifiActionZone, x, y)) {
          if (ost.state == otaupdate::State::UpdateAvailable) {
            const auto &st = voltraClient.state();
            otaupdate::beginInstall(st.armed || st.loaded); // reboots on success -- does not return
          } else if (ost.state != otaupdate::State::Checking && ost.state != otaupdate::State::Downloading) {
            otaupdate::checkForUpdate();
          }
          changed = true;
          didTapAction = true;
        } else if (inRect(kBackZone, x, y)) {
          sScreen = Screen::Settings;
          changed = true;
          didTapAction = true;
        }
      } else if (sScreen == Screen::FactoryReset) {
        if (inRect(kWifiActionZone, x, y)) {
          if (sFactoryResetArmed) {
            didTapAction = true;
            prefs::factoryReset(); // reboots -- does not return
          } else {
            sFactoryResetArmed = true;
            sFactoryResetArmedAtMs = now;
            changed = true;
            didTapAction = true;
          }
        } else if (inRect(kBackZone, x, y)) {
          sScreen = Screen::Settings;
          sFactoryResetArmed = false;
          changed = true;
          didTapAction = true;
        }
      }
    }
  } else {
    if (sScreen == Screen::Workout && sPrevTouched && sPressActive && !sHoldFired) {
      // Released before the hold threshold: a tap.
      const auto &st = voltraClient.state();
      if (st.loaded || st.armed) {
        voltraClient.disengage();
      } else {
        voltraClient.engage();
      }
      changed = true;
      didTapAction = true;
    }
    if (sScreen == Screen::Workout && sPrevTouched && sChainsPressActive && !sChainsHoldFired) {
      // Released before the hold threshold: a tap -- focus the shared
      // chains amount (or return to base weight if it was already focused),
      // same toggle-or-focus meaning the eccentric bubble's plain tap has.
      sMode = (sMode == Mode::Chains) ? Mode::Base : Mode::Chains;
      changed = true;
      didTapAction = true;
    }
    for (uint8_t i = 0; i < 2; i++) {
      if (sScreen == Screen::Workout && sPrevTouched && sPresetPressActive[i] && !sPresetHoldFired[i] &&
          wpresets::hasValue(i)) {
        // Released before the hold threshold: a tap -- recall this slot's
        // stored number into whichever value is currently focused. A slot
        // that's never been stored to (hasValue false) does nothing on tap
        // rather than snapping the focused value to 0.
        int lb = wpresets::valueLb(i);
        if (sMode == Mode::Base) {
          voltraClient.setBaseWeightLb(lb);
        } else if (sMode == Mode::Eccentric) {
          voltraClient.setEccentricWeightLb(lb);
        } else {
          voltraClient.setChainsWeightLb(lb);
        }
        changed = true;
        didTapAction = true;
      }
      sPresetPressActive[i] = false;
      sPresetHoldFired[i] = false;
    }
    sPressActive = false;
    sHoldFired = false;
    sChainsPressActive = false;
    sChainsHoldFired = false;
  }
  sPrevTouched = tp.touched;

  if (didTapAction && prefs::hapticsEnabled()) {
    haptics::pulse();
  }

  // While on any settings screen that shows live/async status -- Wi-Fi and
  // Bluetooth's connection state, Settings' own focused-row preview of
  // either, or Factory Reset's arm countdown -- redraw periodically even
  // with no touch/encoder input, so a change shows up without requiring a
  // tap (e.g. a scan finishing, "Connecting..." turning into "Connected",
  // or the reset arm window silently expiring). Firmware Update joins this
  // list for the same reason: its Downloading progress percent and any
  // Checking->UpdateAvailable/Error transition happen with no new touch
  // input either.
  static uint32_t sLastMenuRedrawMs = 0;
  if ((sScreen == Screen::Wifi || sScreen == Screen::Settings || sScreen == Screen::Bluetooth ||
       sScreen == Screen::Twin || sScreen == Screen::FactoryReset || sScreen == Screen::FirmwareUpdate) &&
      now - sLastMenuRedrawMs >= 500) {
    changed = true;
    sLastMenuRedrawMs = now;
  }

  static voltra::ConnState sLastConnState = voltra::ConnState::Idle;
  static int sLastBase = -9999, sLastEcc = -9999, sLastChains = -9999;
  static bool sLastLoaded = false, sLastArmed = false;
  static int sLastReps = -1, sLastBattery = -2, sLastGuidedState = -2;
  static int sLastBoardBattery = -2;
  const auto &st = voltraClient.state();
  if (st.connState != sLastConnState || st.baseWeightLb != sLastBase ||
      st.eccentricWeightLb != sLastEcc || st.chainsWeightLb != sLastChains ||
      st.loaded != sLastLoaded ||
      st.armed != sLastArmed || st.reps != sLastReps || st.batteryPct != sLastBattery ||
      st.guidedState != sLastGuidedState || boardBatteryPercent != sLastBoardBattery) {
    changed = true;
  }
  // While counting down, redraw periodically even with no other change so
  // the countdown number visibly ticks (guidedCountdownMs itself isn't part
  // of the change check above -- it updates several times a second and
  // would otherwise force a full redraw every loop iteration).
  static uint32_t sLastCountdownRedrawMs = 0;
  if (st.guidedState == voltra::kGuidedStateCountdown && now - sLastCountdownRedrawMs >= 200) {
    changed = true;
    sLastCountdownRedrawMs = now;
  }
  // Same idea while reconnecting: the status row's elapsed-time text (see
  // draw()) only visibly ticks if something forces a redraw once a second,
  // since connState itself isn't changing during a single long scan.
  static uint32_t sLastReconnectRedrawMs = 0;
  if (st.connState != voltra::ConnState::Ready && now - sLastReconnectRedrawMs >= 1000) {
    changed = true;
    sLastReconnectRedrawMs = now;
  }

  if (changed) {
    draw(st, boardBatteryPercent, voltraClient);
    sLastConnState = st.connState;
    sLastBase = st.baseWeightLb;
    sLastEcc = st.eccentricWeightLb;
    sLastChains = st.chainsWeightLb;
    sLastLoaded = st.loaded;
    sLastArmed = st.armed;
    sLastReps = st.reps;
    sLastBattery = st.batteryPct;
    sLastGuidedState = st.guidedState;
    sLastBoardBattery = boardBatteryPercent;
  }
}

} // namespace ui
