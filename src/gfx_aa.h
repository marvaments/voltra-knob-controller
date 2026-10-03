#pragma once
#include <cstdint>

class Arduino_Canvas;

// Small antialiasing helpers for the drawing calls Arduino_GFX doesn't
// smooth on its own. Confirmed by reading the installed "GFX Library for
// Arduino" source: drawCircle/drawLine/etc are plain hard-edged Bresenham/
// scanline routines with no blending anywhere in the library. On this
// board's small round panel, the stair-stepping on the outer bezel ring
// and the tick-ring gauge's/gear icon's thin radial lines is genuinely
// visible, so these blend the drawn color toward a caller-supplied
// background color at partially-covered edge pixels instead of picking
// one or the other outright.
//
// Deliberately scoped to strokes/thin lines, not filled shapes -- solid
// fills (the theme swatch, button pills, the workout-state ring, the gear
// body) already read as smooth on this display since there's no fine edge
// detail to alias against; the thin line-art is where hard edges actually
// show. Filled shapes can get the same treatment later if it turns out to
// matter on real hardware.
//
// Every function here blends against a single flat `bg` color rather than
// reading back whatever is already drawn underneath -- this canvas class
// has no pixel-readback call. That's correct as long as the shape is drawn
// directly on top of a flat fillScreen(bg), which is true everywhere these
// are currently called from in ui.cpp.
namespace gfxaa {

// Linear-interpolates an RGB565 color toward fg by alpha/255.
uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t alpha);

// Antialiased circle OUTLINE (not filled), stroke centered on radius r.
// Cost is O((r+strokeW)^2 / 4) sqrt() calls (one quadrant, mirrored to the
// other three) rather than a full bounding-box scan -- cheap enough to
// redraw every frame at this display's sizes (a few ms at most for the
// biggest ring on screen, kBezelR).
void drawCircleAA(Arduino_Canvas *c, int16_t cx, int16_t cy, float r, float strokeW, uint16_t fg, uint16_t bg);

// Antialiased line via Wu's algorithm -- the standard approach for a thin
// smooth line without supersampling: each end, and each pixel column (or
// row, for steep lines) along the way, splits its coverage between the two
// pixels straddling the exact line position, weighted by how close the
// line passes to each.
void drawLineAA(Arduino_Canvas *c, float x0, float y0, float x1, float y1, uint16_t fg, uint16_t bg);

} // namespace gfxaa
