// eyes_dirty.h - works out which parts of the screen changed since last frame.
//
// The face is one solid color, so normally only the area around each eye
// (where it was + where it is now) needs redrawing. During the color wipe we
// also redraw just the ring the wipe circle grew through since last frame.
// Fewer pixels per frame = more frames per second = smoother motion.
#pragma once
#include "eyes_render.h"

namespace eyes {

static inline bool rectEmpty(const Rect &r) { return r.x1 < r.x0 || r.y1 < r.y0; }

static inline Rect rectUnion(const Rect &a, const Rect &b) {
  if (rectEmpty(a)) return b;
  if (rectEmpty(b)) return a;
  return { a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0,
           a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1 };
}

static inline bool rectOverlap(const Rect &a, const Rect &b) {
  return !rectEmpty(a) && !rectEmpty(b) &&
         !(a.x1 < b.x0 || b.x1 < a.x0 || a.y1 < b.y0 || b.y1 < a.y0);
}

// Clamp to the screen and snap to the panel's 2-pixel grid
// (even start, odd end), which the SH8601/CO5300 require.
static inline Rect rectAlign(Rect r, int size) {
  if (rectEmpty(r)) return r;
  if (r.x0 < 0) r.x0 = 0;
  if (r.y0 < 0) r.y0 = 0;
  if (r.x1 > size - 1) r.x1 = size - 1;
  if (r.y1 > size - 1) r.y1 = size - 1;
  if (rectEmpty(r)) return { 0, 0, -1, -1 };
  r.x0 &= ~1; r.y0 &= ~1;
  r.x1 |= 1;  r.y1 |= 1;
  if (r.x1 > size - 1) r.x1 = size - 1;
  if (r.y1 > size - 1) r.y1 = size - 1;
  return r;
}

class DirtyTracker {
 public:
  static const int MAX_RECTS = 80;

  void begin(int rotation, int screenSize, int ringBand = 16) {
    rot = rotation; size = screenSize; band = ringBand;
    first = true;
    prev[0] = prev[1] = { 0, 0, -1, -1 };
  }

  // Fill `out` with the physical, panel-aligned rects to redraw this frame.
  // ringActive/r0/r1: the wipe ring (screen-centered) that changed this frame.
  int collect(const Prepared &p, bool ringActive, float r0, float r1, Rect *out) {
    int n = 0;
    Rect cur[2];
    for (int e = 0; e < 2; e++) cur[e] = logicalToPhysical(eyeRect(p, e), rot, size);

    if (first) {
      first = false;
      out[n++] = { 0, 0, size - 1, size - 1 };
    } else {
      Rect r[2] = { rectUnion(cur[0], prev[0]), rectUnion(cur[1], prev[1]) };
      if (rectOverlap(r[0], r[1])) { r[0] = rectUnion(r[0], r[1]); r[1] = { 0, 0, -1, -1 }; }
      for (int e = 0; e < 2; e++) {
        Rect a = rectAlign(r[e], size);
        if (!rectEmpty(a)) out[n++] = a;
      }
      if (ringActive) n += ringRects(r0, r1, out + n, MAX_RECTS - n);
    }
    prev[0] = cur[0];
    prev[1] = cur[1];
    return n;
  }

 private:
  int rot = 0, size = 466, band = 16;
  bool first = true;
  Rect prev[2];

  // Rects covering the annulus r0 <= dist <= r1 around the screen center,
  // one horizontal band at a time (left and right pieces).
  int ringRects(float r0, float r1, Rect *out, int room) {
    int n = 0;
    const float c = size * 0.5f;
    if (r0 < 0) r0 = 0;
    if (r1 > size) r1 = (float)size;
    for (int y = 0; y < size && n + 2 <= room; y += band) {
      int y1 = y + band - 1; if (y1 > size - 1) y1 = size - 1;
      float ya = y + 0.5f - c, yb = y1 + 0.5f - c;             // pixel-center offsets
      float dyMin = (ya <= 0 && yb >= 0) ? 0.f : fminf(fabsf(ya), fabsf(yb));
      float dyMax = fmaxf(fabsf(ya), fabsf(yb));
      if (r1 <= dyMin) continue;                               // ring doesn't reach this band
      float xo = sqrtf(r1 * r1 - dyMin * dyMin);               // widest outer extent
      float xi = r0 > dyMax ? sqrtf(r0 * r0 - dyMax * dyMax) : 0.f;   // narrowest hole
      Rect L, R;
      if (xi <= 2.f) {
        L = { (int)floorf(c - xo - 1), y, (int)ceilf(c + xo + 1), y1 };
        R = { 0, 0, -1, -1 };
      } else {
        L = { (int)floorf(c - xo - 1), y, (int)ceilf(c - xi + 1), y1 };
        R = { (int)floorf(c + xi - 1), y, (int)ceilf(c + xo + 1), y1 };
      }
      L = rectAlign(L, size);
      R = rectAlign(R, size);
      if (rectOverlap(L, R)) { L = rectUnion(L, R); R = { 0, 0, -1, -1 }; }
      if (!rectEmpty(L)) out[n++] = L;
      if (!rectEmpty(R)) out[n++] = R;
    }
    return n;
  }
};

}  // namespace eyes
