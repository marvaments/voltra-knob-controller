#include "gfx_aa.h"
#include <Arduino_GFX_Library.h>
#include <cmath>

namespace gfxaa {

uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t alpha) {
  if (alpha == 0) return bg;
  if (alpha == 255) return fg;
  // Unpack both colors into 5/6/5 components, interpolate as signed int
  // throughout (an intermediate uint8_t cast of a negative difference would
  // wrap instead of staying negative), then repack.
  int br = (bg >> 11) & 0x1F, bg6 = (bg >> 5) & 0x3F, bb = bg & 0x1F;
  int fr = (fg >> 11) & 0x1F, fg6 = (fg >> 5) & 0x3F, fb = fg & 0x1F;
  int r = br + ((fr - br) * (int)alpha) / 255;
  int g = bg6 + ((fg6 - bg6) * (int)alpha) / 255;
  int b = bb + ((fb - bb) * (int)alpha) / 255;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

void drawCircleAA(Arduino_Canvas *c, int16_t cx, int16_t cy, float r, float strokeW, uint16_t fg, uint16_t bg) {
  float halfW = strokeW * 0.5f;
  int16_t maxOff = (int16_t)ceilf(r + halfW + 1.0f);
  // Walk one quadrant (dx,dy >= 0) and mirror each result into the other
  // three -- a circle is symmetric across both axes, so this does 1/4 the
  // sqrt() calls a full bounding-box scan would.
  for (int16_t dy = 0; dy <= maxOff; dy++) {
    float fy = (float)dy + 0.5f;
    for (int16_t dx = 0; dx <= maxOff; dx++) {
      float fx = (float)dx + 0.5f;
      float dist = sqrtf(fx * fx + fy * fy);
      // Signed distance from the stroke's own edge (0 = exactly on the
      // edge, negative = inside the stroke, positive = outside it).
      float d = fabsf(dist - r) - (halfW - 0.5f);
      if (d >= 0.5f) continue; // fully outside the stroke's AA falloff band
      uint16_t col = (d <= -0.5f) ? fg : blend565(bg, fg, (uint8_t)((0.5f - d) * 255.0f));
      c->drawPixel(cx + dx, cy + dy, col);
      c->drawPixel(cx - dx - 1, cy + dy, col);
      c->drawPixel(cx + dx, cy - dy - 1, col);
      c->drawPixel(cx - dx - 1, cy - dy - 1, col);
    }
  }
}

static inline float fpart(float x) { return x - floorf(x); }
static inline float rfpart(float x) { return 1.0f - fpart(x); }

static inline void plot(Arduino_Canvas *c, int x, int y, uint16_t fg, uint16_t bg, float coverage) {
  if (coverage <= 0.0f) return;
  if (coverage > 1.0f) coverage = 1.0f;
  c->drawPixel(x, y, blend565(bg, fg, (uint8_t)(coverage * 255.0f + 0.5f)));
}

void drawLineAA(Arduino_Canvas *c, float x0, float y0, float x1, float y1, uint16_t fg, uint16_t bg) {
  bool steep = fabsf(y1 - y0) > fabsf(x1 - x0);
  if (steep) {
    float t = x0; x0 = y0; y0 = t;
    t = x1; x1 = y1; y1 = t;
  }
  if (x0 > x1) {
    float t = x0; x0 = x1; x1 = t;
    t = y0; y0 = y1; y1 = t;
  }
  float dx = x1 - x0;
  float dy = y1 - y0;
  float gradient = (dx > -0.0001f && dx < 0.0001f) ? 1.0f : dy / dx;

  // First endpoint.
  float xend = roundf(x0);
  float yend = y0 + gradient * (xend - x0);
  float xgap = rfpart(x0 + 0.5f);
  int xpxl1 = (int)xend;
  int ypxl1 = (int)floorf(yend);
  if (steep) {
    plot(c, ypxl1, xpxl1, fg, bg, rfpart(yend) * xgap);
    plot(c, ypxl1 + 1, xpxl1, fg, bg, fpart(yend) * xgap);
  } else {
    plot(c, xpxl1, ypxl1, fg, bg, rfpart(yend) * xgap);
    plot(c, xpxl1, ypxl1 + 1, fg, bg, fpart(yend) * xgap);
  }
  float intery = yend + gradient;

  // Second endpoint.
  xend = roundf(x1);
  yend = y1 + gradient * (xend - x1);
  xgap = fpart(x1 + 0.5f);
  int xpxl2 = (int)xend;
  int ypxl2 = (int)floorf(yend);
  if (steep) {
    plot(c, ypxl2, xpxl2, fg, bg, rfpart(yend) * xgap);
    plot(c, ypxl2 + 1, xpxl2, fg, bg, fpart(yend) * xgap);
  } else {
    plot(c, xpxl2, ypxl2, fg, bg, rfpart(yend) * xgap);
    plot(c, xpxl2, ypxl2 + 1, fg, bg, fpart(yend) * xgap);
  }

  // Everything in between: one pixel column (or row, if steep) per step,
  // split across the two pixels the line's exact position falls between.
  if (steep) {
    for (int x = xpxl1 + 1; x < xpxl2; x++) {
      plot(c, (int)floorf(intery), x, fg, bg, rfpart(intery));
      plot(c, (int)floorf(intery) + 1, x, fg, bg, fpart(intery));
      intery += gradient;
    }
  } else {
    for (int x = xpxl1 + 1; x < xpxl2; x++) {
      plot(c, x, (int)floorf(intery), fg, bg, rfpart(intery));
      plot(c, x, (int)floorf(intery) + 1, fg, bg, fpart(intery));
      intery += gradient;
    }
  }
}

} // namespace gfxaa
