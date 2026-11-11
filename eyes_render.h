// eyes_render.h - draws the "ball with two pill eyes" character.
//
// The ball is treated as a real 3D sphere. The two pill-shaped eyes are
// painted on its front face, and the whole sphere is rotated (yaw / pitch /
// roll) to make it look around. Eyes therefore foreshorten and lean naturally
// as they turn toward the edge of the ball, exactly like the reference art.
//
// The eyes are "holes" in the ball: they always show the background color.
//
// Pure C++ (no Arduino dependencies) so it can also run on a PC for previews.
#pragma once
#include <stdint.h>
#include <math.h>

namespace eyes {

struct Color { float r, g, b; };

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
  Color bgA, fgA;              // background / ball color outside the wipe circle
  Color bgB, fgB;              // background / ball color inside the wipe circle
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
  bool  eyesVisible;
  float eyeBX[2], eyeBY[2], eyeBR2[2];   // screen-space bounding circles of the pills
  float openLen;                     // half-extent left open by the eyelids
  uint16_t bgA565, fgA565, bgB565, fgB565;
};

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

  // Lids: when shut, leave a thin dash (~5 px) so a blink reads as a closed eye.
  float fullLen = s.eye.halfLen + s.eye.radius;
  float minOpen = 2.4f / s.ballR;
  p.openLen = fmaxf(clamp01(s.blink) * fullLen, minOpen);

  // Project each eye center to the screen to get a bounding circle.
  p.eyesVisible = false;
  float bound = (fullLen * 1.2f) * s.ballR + 3.0f;
  for (int i = 0; i < 2; i++) {
    float fu = (i == 0 ? -1.f : 1.f) * s.eye.spacing, fv = s.eye.height;
    // face -> local (apply roll)
    float u = fu * p.cr - fv * p.sr;
    float v = fu * p.sr + fv * p.cr;
    float w = sqrtf(fmaxf(0.f, 1.f - u * u - v * v));
    // local -> screen = M^T * local
    float x = p.m00 * u + p.m10 * v + p.m20 * w;
    float y =             p.m11 * v + p.m21 * w;
    float z = p.m02 * u + p.m12 * v + p.m22 * w;
    p.eyeBX[i] = s.ballX + x * s.ballR;
    p.eyeBY[i] = s.ballY + y * s.ballR;
    p.eyeBR2[i] = bound * bound;
    if (z > -0.35f) p.eyesVisible = true;
  }

  p.bgA565 = pack565(s.bgA.r, s.bgA.g, s.bgA.b);
  p.fgA565 = pack565(s.fgA.r, s.fgA.g, s.fgA.b);
  p.bgB565 = pack565(s.bgB.r, s.bgB.g, s.bgB.b);
  p.fgB565 = pack565(s.fgB.r, s.fgB.g, s.fgB.b);
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
  float ex = aa - s.eye.halfLen; ex = ex > 0 ? ex : 0;
  float d = sqrtf(b * b + ex * ex) - s.eye.radius;
  float lid = aa - p.openLen;         // top + bottom eyelids
  return d > lid ? d : lid;
}

// Coverage (0..1) of the eye holes at a pixel inside the ball.
static inline float eyeHoleCoverage(const Scene &s, const Prepared &p, float dx, float dy) {
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
  float d = fminf(pillDist(s, p, fu, fv, -1.f), pillDist(s, p, fu, fv, 1.f));
  // face units -> screen pixels (foreshortening makes things smaller near the rim)
  float k = nz / lz; k = k < 0.15f ? 0.15f : (k > 1.f ? 1.f : k);
  return clamp01(0.5f - d * s.ballR * k);
}

struct Rect { int x0, y0, x1, y1; };   // inclusive pixel rectangle

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

// Render a rectangle of physical pixels [x0, x0+w) x [y0, y0+h) into buf (stride w).
static inline void renderRect(const Scene &s, const Prepared &p, const Xform &xf,
                              uint16_t *buf, int x0, int y0, int w, int h) {
  const bool wiping = s.wipeR > 0.f;
  const float wIn = wiping ? (s.wipeR > 1 ? (s.wipeR - 1) * (s.wipeR - 1) : 0.f) : 0.f;
  const float wOut = (s.wipeR + 1) * (s.wipeR + 1);

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
        if (p.eyesVisible) {
          bool near = false;
          for (int e = 0; e < 2; e++) {
            float ex = px - p.eyeBX[e], ey = py - p.eyeBY[e];
            if (ex * ex + ey * ey < p.eyeBR2[e]) { near = true; break; }
          }
          if (near) alpha *= 1.f - eyeHoleCoverage(s, p, dx, dy);
        }
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
