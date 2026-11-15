// eyes_render.h - draws the two pill eyes on a 3D ball.
//
// The ball is treated as a real 3D sphere. The two pill-shaped eyes are
// painted on its front face, and the whole sphere is rotated (yaw / pitch /
// roll) to make it look around. Eyes therefore foreshorten and lean naturally
// as they turn toward the edge of the ball, exactly like the reference art.
//
// The eyes are "holes" in the ball: they always show the background color.
// In RoboEyes the ball is as big as the round screen, so the screen *is* the face.
//
// Pure C++ (no Arduino dependencies) so it can also run on a PC for previews.
#pragma once
#include <stdint.h>
#include <math.h>

// This is the hot loop: build it with full optimization even though Arduino
// compiles sketches with -Os.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("O2", "fast-math")
#endif

#ifndef EYES_COUNT_3D_PIXEL
#define EYES_COUNT_3D_PIXEL()   // hook for the PC benchmark
#endif

namespace eyes {

struct Color { float r, g, b; };
struct Rect { int x0, y0, x1, y1; };   // inclusive pixel rectangle; empty if x1 < x0
struct Box { float x0, y0, x1, y1; };  // float rectangle in logical (scene) pixels

static inline Color hexColor(uint32_t c) {
  return { float((c >> 16) & 0xFF), float((c >> 8) & 0xFF), float(c & 0xFF) };
}

// Eye geometry on the ball's face, in units of the ball radius.
struct EyeStyle {
  float spacing;   // half the distance between the two eye centers
  float height;    // vertical position of the eye centers (+ = lower)
  float radius;    // half the width of each pill
  float halfLen;   // half the length of the straight part of each pill
  float tilt;      // radians; leans the tops of the pills outward (mirrored per eye)
};

// Everything needed to draw one frame.
struct Scene {
  float ballX, ballY, ballR;   // ball center / radius in screen pixels
  float yaw, pitch, roll;      // radians: + yaw = look right, + pitch = look up, + roll = clockwise
  float blink;                 // 1 = open, 0 = shut
  // mood eyelids (fractions of the pill), smoothly animated by the animator
  float lidTop;                // 0..1: how far the upper lid comes down
  float lidSlant;              // + = outer corners droop (sad), - = inner corners (angry)
  float lidHappy;              // 0..1: curved lower lid pushing up (happy "^ ^")
  Color bgA, fgA;              // eye / face color outside the wipe circle
  Color bgB, fgB;              // eye / face color inside the wipe circle
  float wipeX, wipeY, wipeR;   // wipe circle; wipeR <= 0 means no wipe
  EyeStyle eye;
};

// 565 color, byte-swapped for the panel (it expects big-endian pixels)
static inline uint16_t pack565(float r, float g, float b) {
  int ir = (int)(r + 0.5f), ig = (int)(g + 0.5f), ib = (int)(b + 0.5f);
  ir = ir < 0 ? 0 : (ir > 255 ? 255 : ir);
  ig = ig < 0 ? 0 : (ig > 255 ? 255 : ig);
  ib = ib < 0 ? 0 : (ib > 255 ? 255 : ib);
  uint16_t c = (uint16_t)(((ir & 0xF8) << 8) | ((ig & 0xFC) << 3) | (ib >> 3));
  return (uint16_t)((c >> 8) | (c << 8));
}

static inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

// Per-frame values derived from a Scene (computed once, used for every pixel).
struct Prepared {
  // screen-normal -> ball-local rotation (row-major, m01 is always 0)
  float m00, m02, m10, m11, m12, m20, m21, m22;
  float cr, sr;                      // roll
  float ct, st;                      // pill tilt
  float invR;
  float r2in, r2out;                 // (R-1)^2, (R+1)^2
  float L;                           // half length of the pill, tip to tip
  float B, hl;                       // pill radius (half width) and straight half-length this frame
  float topC, topK, topNorm;         // upper lid line: a = topC + topK * outer
  bool  happyOn;
  float cutA;                        // happy: lower lid rises to here...
  float holeR, holeA;                // ...and a hole opens under the top cap, leaving an arch
  float aMid, openHalf;              // blink closes toward aMid; half-height left open
  bool  eyeOn[2];                    // eye faces the viewer
  Box   eyeBox[2];                   // tight logical-pixel bounds of each eye (incl. anti-aliasing)
  uint16_t bgA565, fgA565, bgB565, fgB565;
};

// face (u,v on the ball's front) -> screen offset from ball center, in ball radii
static inline void faceToScreen(const Prepared &p, float fu, float fv, float &x, float &y, float &z) {
  float u = fu * p.cr - fv * p.sr;               // apply roll
  float v = fu * p.sr + fv * p.cr;
  float w = sqrtf(fmaxf(0.f, 1.f - u * u - v * v));
  x = p.m00 * u + p.m10 * v + p.m20 * w;         // rotate: screen = M^T * local
  y =             p.m11 * v + p.m21 * w;
  z = p.m02 * u + p.m12 * v + p.m22 * w;
}

static inline void prepare(const Scene &s, Prepared &p) {
  float cy = cosf(s.yaw), sy = sinf(s.yaw);
  float cp = cosf(s.pitch), sp = sinf(s.pitch);
  // screen = Ry(yaw) * Rx(pitch) * local   =>   local = M * screen
  p.m00 = cy;      p.m02 = -sy;
  p.m10 = sp * sy; p.m11 = cp;  p.m12 = sp * cy;
  p.m20 = cp * sy; p.m21 = -sp; p.m22 = cp * cy;
  p.cr = cosf(s.roll); p.sr = sinf(s.roll);
  p.ct = cosf(s.eye.tilt); p.st = sinf(s.eye.tilt);
  p.invR = 1.0f / s.ballR;
  p.r2in = (s.ballR - 1) * (s.ballR - 1);
  p.r2out = (s.ballR + 1) * (s.ballR + 1);

  // Pill coordinates: a runs along the pill (top = -L, bottom = +L), b across it.
  // Happy widens the pill so its arch reads clearly.
  const float h = clamp01(s.lidHappy);
  const float B = s.eye.radius * (1.f + 0.6f * h), hl = s.eye.halfLen;
  const float L = hl + B;
  p.L = L; p.B = B; p.hl = hl;
  // upper lid: hides everything above the line a = topC + topK * outer
  p.topC = -L + 2.f * L * clamp01(s.lidTop);
  p.topK = s.lidSlant * L / B;
  p.topNorm = 1.f / sqrtf(1.f + p.topK * p.topK);
  // happy: a lower lid rises to just under the top cap and a hole opens in the
  // cap, so the pill becomes an arch (an upside-down U, like "^ ^")
  p.happyOn = h > 0.001f;
  p.holeA = -hl + 0.1f * B;
  p.holeR = h * 0.55f * B;
  p.cutA = L + ((-hl + 0.3f * B) - L) * h;
  // visible span down the middle of the pill
  float aTop = fmaxf(-L, p.topC), aBot = p.cutA;
  if (aBot < aTop + 0.02f) aBot = aTop + 0.02f;
  // blinks close toward the middle of what's visible (the top of the arch when happy)
  float openMid = 0.5f * (aTop + aBot);
  float closeMid = openMid + (0.5f * (aTop + fmaxf(aTop, p.holeA - p.holeR)) - openMid) * h;
  float bl = clamp01(s.blink);
  p.aMid = closeMid + (openMid - closeMid) * bl;
  // when shut, leave a thin dash (~5 px) so a blink reads as a closed eye
  p.openHalf = fmaxf(bl * 0.5f * (aBot - aTop), 2.4f / s.ballR);

  // Tight screen bounds: trace the rectangle that encloses the visible pill,
  // project it onto the screen, and take the min/max.
  const float A0 = fmaxf(-L, p.aMid - p.openHalf), A1 = fminf(L, p.aMid + p.openHalf);
  const int K = 6;   // samples per side
  for (int e = 0; e < 2; e++) {
    float side = e == 0 ? -1.f : 1.f, st = p.st * side;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, zmax = -2.f;
    for (int k = 0; k <= K; k++) {
      float t = (float)k / K;
      float ta = A0 + (A1 - A0) * t, tb = -B + 2.f * B * t;
      const float pts[4][2] = { { -B, ta }, { B, ta }, { tb, A0 }, { tb, A1 } };
      for (int q = 0; q < 4; q++) {
        float b = pts[q][0], a = pts[q][1];
        float fu = b * p.ct + a * st + side * s.eye.spacing;   // pill axes -> face
        float fv = -b * st + a * p.ct + s.eye.height;
        float x, y, z;
        faceToScreen(p, fu, fv, x, y, z);
        x0 = fminf(x0, x); x1 = fmaxf(x1, x);
        y0 = fminf(y0, y); y1 = fmaxf(y1, y);
        zmax = fmaxf(zmax, z);
      }
    }
    const float m = 3.f;   // anti-aliasing + curvature between samples
    p.eyeOn[e] = zmax > 0.f;
    p.eyeBox[e] = { s.ballX + x0 * s.ballR - m, s.ballY + y0 * s.ballR - m,
                    s.ballX + x1 * s.ballR + m, s.ballY + y1 * s.ballR + m };
  }

  p.bgA565 = pack565(s.bgA.r, s.bgA.g, s.bgA.b);
  p.fgA565 = pack565(s.fgA.r, s.fgA.g, s.fgA.b);
  p.bgB565 = pack565(s.bgB.r, s.bgB.g, s.bgB.b);
  p.fgB565 = pack565(s.fgB.r, s.fgB.g, s.fgB.b);
}

// Logical-pixel rect covering one eye (empty if it faces away).
static inline Rect eyeRect(const Prepared &p, int e) {
  if (!p.eyeOn[e]) return { 0, 0, -1, -1 };
  const Box &b = p.eyeBox[e];
  return { (int)floorf(b.x0), (int)floorf(b.y0), (int)ceilf(b.x1), (int)ceilf(b.y1) };
}

// Signed distance (in face units) to one pill eye, including eyelids.
static inline float pillDist(const Scene &s, const Prepared &p, float fu, float fv, float side) {
  float x = fu - side * s.eye.spacing;
  float y = fv - s.eye.height;
  // rotate into the pill's own axes (tilt mirrored per eye)
  float st = p.st * side;
  float b = x * p.ct - y * st;        // across the pill
  float a = x * st + y * p.ct;        // along the pill
  float aa = fabsf(a);
  float ex = aa - p.hl; ex = ex > 0 ? ex : 0;
  float d = sqrtf(b * b + ex * ex) - p.B;                    // the pill itself
  float outer = b * side;                                    // + toward the outer corner
  float top = (p.topC + p.topK * outer - a) * p.topNorm;     // mood: upper lid
  if (top > d) d = top;
  if (p.happyOn) {                                           // mood: happy arch
    float cut = a - p.cutA;
    if (cut > d) d = cut;
    float da = a - p.holeA;
    float hole = p.holeR - sqrtf(b * b + da * da);
    if (hole > d) d = hole;
  }
  float blink = fabsf(a - p.aMid) - p.openHalf;              // blink lids
  return d > blink ? d : blink;
}

// Coverage (0..1) of the eye holes at a pixel. mask: bit0 = left eye, bit1 = right eye.
static inline float eyeHoleCoverage(const Scene &s, const Prepared &p, float dx, float dy, int mask) {
  EYES_COUNT_3D_PIXEL();
  float nx = dx * p.invR, ny = dy * p.invR;
  float nz2 = 1.f - nx * nx - ny * ny;
  float nz = nz2 > 0 ? sqrtf(nz2) : 0.f;
  float lx = p.m00 * nx + p.m02 * nz;
  float ly = p.m10 * nx + p.m11 * ny + p.m12 * nz;
  float lz = p.m20 * nx + p.m21 * ny + p.m22 * nz;
  if (lz <= 0.02f) return 0.f;                 // that point is on the back of the ball
  // local -> face (undo roll)
  float fu =  lx * p.cr + ly * p.sr;
  float fv = -lx * p.sr + ly * p.cr;
  float d = 1e9f;
  if (mask & 1) d = pillDist(s, p, fu, fv, -1.f);
  if (mask & 2) d = fminf(d, pillDist(s, p, fu, fv, 1.f));
  // face units -> screen pixels (foreshortening makes things smaller near the rim)
  float k = nz / lz; k = k < 0.15f ? 0.15f : (k > 1.f ? 1.f : k);
  return clamp01(0.5f - d * s.ballR * k);
}

// Maps a physical pixel to logical (scene) coordinates, for screen rotation.
struct Xform {
  float ox, oy, xx, xy, yx, yy;   // lx = ox + xx*X + xy*Y ; ly = oy + yx*X + yy*Y
};

static inline Xform makeXform(int rotation, int size) {
  float W = (float)size;
  switch (rotation) {
    case 90:  return { 0, W, 0, 1, -1, 0 };    // lx = Y,     ly = W - X
    case 180: return { W, W, -1, 0, 0, -1 };   // lx = W - X, ly = W - Y
    case 270: return { W, 0, 0, -1, 1, 0 };    // lx = W - Y, ly = X
    default:  return { 0, 0, 1, 0, 0, 1 };
  }
}

// Logical (scene) rect -> physical (panel) rect; the inverse of makeXform.
static inline Rect logicalToPhysical(Rect l, int rotation, int size) {
  if (l.x1 < l.x0 || l.y1 < l.y0) return l;
  float W = (float)size;
  float lx0 = (float)l.x0, lx1 = (float)l.x1 + 1, ly0 = (float)l.y0, ly1 = (float)l.y1 + 1;
  float ax, ay, bx, by;   // two opposite corners
  switch (rotation) {
    case 90:  ax = W - ly0; ay = lx0;     bx = W - ly1; by = lx1;     break;
    case 180: ax = W - lx0; ay = W - ly0; bx = W - lx1; by = W - ly1; break;
    case 270: ax = ly0;     ay = W - lx0; bx = ly1;     by = W - lx1; break;
    default:  ax = lx0;     ay = ly0;     bx = lx1;     by = ly1;     break;
  }
  return { (int)floorf(fminf(ax, bx)), (int)floorf(fminf(ay, by)),
           (int)ceilf(fmaxf(ax, bx)) - 1, (int)ceilf(fmaxf(ay, by)) - 1 };
}

// Render a rectangle of physical pixels [x0, x0+w) x [y0, y0+h) into buf (stride w).
static inline void renderRect(const Scene &s, const Prepared &p, const Xform &xf,
                              uint16_t *buf, int x0, int y0, int w, int h) {
  const bool wiping = s.wipeR > 0.f;
  const float wIn = wiping ? (s.wipeR > 1 ? (s.wipeR - 1) * (s.wipeR - 1) : 0.f) : 0.f;
  const float wOut = (s.wipeR + 1) * (s.wipeR + 1);
  const Box b0 = p.eyeBox[0], b1 = p.eyeBox[1];
  const bool on0 = p.eyeOn[0], on1 = p.eyeOn[1];

  for (int j = 0; j < h; j++) {
    uint16_t *row = buf + j * w;
    float Y = (float)(y0 + j) + 0.5f;
    for (int i = 0; i < w; i++) {
      float X = (float)(x0 + i) + 0.5f;
      float px = xf.ox + xf.xx * X + xf.xy * Y;
      float py = xf.oy + xf.yx * X + xf.yy * Y;

      // which palette? (t = 0 -> A, 1 -> B, between = anti-aliased wipe edge)
      float t = 0.f;
      if (wiping) {
        float wx = px - s.wipeX, wy = py - s.wipeY, w2 = wx * wx + wy * wy;
        if (w2 <= wIn) t = 1.f;
        else if (w2 < wOut) t = clamp01(s.wipeR - sqrtf(w2) + 0.5f);
      }

      // ball coverage
      float dx = px - s.ballX, dy = py - s.ballY, d2 = dx * dx + dy * dy;
      float alpha;
      if (d2 >= p.r2out) alpha = 0.f;
      else {
        alpha = (d2 <= p.r2in) ? 1.f : clamp01(s.ballR - sqrtf(d2) + 0.5f);
        int mask = 0;
        if (on0 && px >= b0.x0 && px <= b0.x1 && py >= b0.y0 && py <= b0.y1) mask |= 1;
        if (on1 && px >= b1.x0 && px <= b1.x1 && py >= b1.y0 && py <= b1.y1) mask |= 2;
        if (mask) alpha *= 1.f - eyeHoleCoverage(s, p, dx, dy, mask);
      }

      // fast paths: solid colors
      if (t == 0.f) {
        if (alpha == 0.f) { row[i] = p.bgA565; continue; }
        if (alpha == 1.f) { row[i] = p.fgA565; continue; }
      } else if (t == 1.f) {
        if (alpha == 0.f) { row[i] = p.bgB565; continue; }
        if (alpha == 1.f) { row[i] = p.fgB565; continue; }
      }
      float bgr = s.bgA.r + (s.bgB.r - s.bgA.r) * t;
      float bgg = s.bgA.g + (s.bgB.g - s.bgA.g) * t;
      float bgb = s.bgA.b + (s.bgB.b - s.bgA.b) * t;
      float fgr = s.fgA.r + (s.fgB.r - s.fgA.r) * t;
      float fgg = s.fgA.g + (s.fgB.g - s.fgA.g) * t;
      float fgb = s.fgA.b + (s.fgB.b - s.fgA.b) * t;
      row[i] = pack565(bgr + (fgr - bgr) * alpha, bgg + (fgg - bgg) * alpha, bgb + (fgb - bgb) * alpha);
    }
  }
}

}  // namespace eyes

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif
