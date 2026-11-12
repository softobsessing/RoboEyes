// eyes_anim.h - decides where the eyes look, when they blink, and handles taps.
// Produces an eyes::Scene every frame. Pure C++ so it can run in the PC preview.
//
// Smoothness notes:
//  - Every motion is time-based, so it plays at the same speed at any frame rate.
//  - Gaze uses a two-stage smoothing filter: each move eases in AND out with a
//    bell-shaped speed curve (no sudden start), and a new target arriving
//    mid-move blends in without a jerk.
#pragma once
#include <stdint.h>
#include <math.h>
#include "eyes_render.h"

namespace eyes {

static const float DEG = 3.14159265f / 180.f;

// The whole round screen is the ball's face; only the eyes are drawn on it.
struct AnimConfig {
  uint32_t idleFace, idleEyes;      // colors before tapping
  uint32_t tapFace, tapEyes;        // colors after tapping
  float faceRadius;                 // px; slightly larger than the screen radius
  float homeYaw, homePitch, homeRoll;   // degrees: resting gaze (from the reference art)
  float wander;                     // degrees of gaze wander around home (new target every few s)
  float drift;                      // degrees of slow, continuous floating on top of that
  float gazeSpeed;                  // 1 = default; lower = slower, silkier eye movement
  uint32_t wipeMs;                  // how long the color inversion takes
  uint32_t autoReturnMs;            // 0 = tap toggles, else revert after this long
  EyeStyle eye;
  int screenSize;
};

// Critically damped spring: smooth, no overshoot, frame-rate independent.
struct Spring {
  float x = 0, v = 0;
  void step(float target, float omega, float dt) {
    float d = x - target;
    float tmp = (v + omega * d) * dt;
    float e = expf(-omega * dt);
    x = target + (d + tmp) * e;
    v = (v - omega * tmp) * e;
  }
};

// Two springs in series: velocity rises and falls smoothly (ease-in + ease-out).
struct Smooth2 {
  Spring a, b;
  void reset(float x) { a.x = b.x = x; a.v = b.v = 0; }
  void step(float target, float omega, float dt) {
    // sub-step so a long frame (e.g. the first full-screen draw) stays accurate
    int n = dt > 0.02f ? (int)(dt / 0.02f) + 1 : 1;
    float h = dt / n;
    for (int i = 0; i < n; i++) { a.step(target, omega, h); b.step(a.x, omega, h); }
  }
  float value() const { return b.x; }
};

class Animator {
 public:
  void begin(const AnimConfig &cfg, uint32_t seed, uint32_t now) {
    c = cfg;
    if (c.gazeSpeed <= 0) c.gazeSpeed = 1;
    rng = seed ? seed : 0x9E3779B9u;
    br.x = c.faceRadius; br.v = 0;
    goHome();
    yaw.reset(tYaw); pitch.reset(tPitch); roll.reset(tRoll);
    inverted = false;
    wiping = false; wipeK = 0; wipeDir = 1; prevWipeR = 0; ringOn = false;
    lookStep = -1;
    nextGaze = now + 1500;
    nextBlink = now + 1800;
    blinkStart = 0; blinksLeft = 0;
    t0 = now; last = now;
  }

  // Call once per frame. tapped = true on the frame a new touch starts.
  void update(uint32_t now, bool tapped) {
    float dt = (now - last) * 0.001f;
    if (dt > 0.1f) dt = 0.1f;
    last = now;
    float t = (now - t0) * 0.001f;

    if (tapped) onTap(now);
    if (c.autoReturnMs && isInverted() && !wiping && now - tapTime > c.autoReturnMs) onTap(now);

    // ---- gaze targets ----
    float omega = 5.5f;                       // idle: slow, gentle glides
    if (lookStep >= 0) {
      // look-around after a tap: a smooth sweep left, then right, then back
      static const float seq[][3] = {   // yaw, pitch, roll (deg)
        { -36.f,  8.f,  9.f },
        {  38.f, 20.f, -12.f },
        {   0.f,  2.f,  0.f },
      };
      static const uint32_t hold[] = { 650, 750, 500 };
      const int n = sizeof(hold) / sizeof(hold[0]);
      omega = 8.5f;
      if (lookStep < n) {
        tYaw = seq[lookStep][0] * DEG; tPitch = seq[lookStep][1] * DEG; tRoll = seq[lookStep][2] * DEG;
        if (now - lookStepStart > (uint32_t)(hold[lookStep] / c.gazeSpeed)) { lookStep++; lookStepStart = now; }
      } else {
        lookStep = -1;
        goHome();
        nextGaze = now + rand(1800, 3000);
        startBlink(now, false);
      }
    } else if ((int32_t)(now - nextGaze) >= 0) {
      pickIdleGaze();
      nextGaze = now + rand(1500, 4000);
    }
    omega *= c.gazeSpeed;
    yaw.step(tYaw, omega, dt);
    pitch.step(tPitch, omega, dt);
    roll.step(tRoll, omega * 0.8f, dt);

    // ---- slow continuous float, so the eyes never sit perfectly still ----
    float fx = c.drift * DEG * (sinf(t * 0.63f + 0.7f) * 0.7f + sinf(t * 1.17f) * 0.3f);
    float fy = c.drift * DEG * (sinf(t * 0.51f) * 0.6f + sinf(t * 0.93f + 2.1f) * 0.4f);
    // face radius only ever bulges outward (tap "boing"), so the rim never shows
    br.step(c.faceRadius, 9.f, dt);
    if (br.x < c.faceRadius) { br.x = c.faceRadius; if (br.v < 0) br.v = 0; }

    // ---- blinking (both eyes together) ----
    if (blinkStart == 0 && (int32_t)(now - nextBlink) >= 0) startBlink(now, true);
    float open = 1.f;
    if (blinkStart && (int32_t)(now - blinkStart) >= 0) {
      uint32_t bt = now - blinkStart;
      const uint32_t CLOSE = 90, HOLD = 40, OPEN = 150;
      if (bt < CLOSE) open = 1.f - ease(bt / (float)CLOSE);
      else if (bt < CLOSE + HOLD) open = 0.f;
      else if (bt < CLOSE + HOLD + OPEN) open = ease((bt - CLOSE - HOLD) / (float)OPEN);
      else if (--blinksLeft > 0) blinkStart = now + 110;   // double blink after a short gap
      else { blinkStart = 0; nextBlink = now + rand(2500, 6000); }
    }

    // ---- color wipe (circle from the center; reverses if tapped mid-way) ----
    const float endR = c.screenSize * 0.5f + 3.f;   // just past the visible edge
    float wipeR = 0.f;
    ringOn = false;
    if (wiping) {
      wipeK += wipeDir * dt * 1000.f / (float)c.wipeMs;
      if (wipeK >= 1.f) {                 // finished: inverted colors everywhere
        wiping = false; inverted = !inverted;
        ringOn = true; ringR0 = prevWipeR - 2.f; ringR1 = 1e6f;
      } else if (wipeK <= 0.f) {          // reversed all the way back
        wiping = false;
        ringOn = true; ringR0 = 0.f; ringR1 = prevWipeR + 2.f;
      } else {
        wipeR = endR * (0.5f - 0.5f * cosf(wipeK * 3.14159265f));   // ease in + out
        ringOn = true;
        ringR0 = fminf(prevWipeR, wipeR) - 2.f;
        ringR1 = fmaxf(prevWipeR, wipeR) + 2.f;
        prevWipeR = wipeR;
      }
      if (!wiping) prevWipeR = 0.f;
    }

    // ---- fill the scene ----
    float half = c.screenSize * 0.5f;
    s.ballX = half; s.ballY = half; s.ballR = br.x;
    s.yaw = yaw.value() + fx; s.pitch = pitch.value() + fy; s.roll = roll.value();
    s.blink = open;
    setPalette(inverted, s.bgA, s.fgA);
    setPalette(!inverted, s.bgB, s.fgB);
    s.wipeX = half; s.wipeY = half;
    s.wipeR = wiping ? fmaxf(wipeR, 0.001f) : 0.f;
    s.eye = c.eye;
  }

  const Scene &scene() const { return s; }

  // The ring of the wipe that changed this frame (screen-centered), if any.
  bool wipeRing(float &r0, float &r1) const { r0 = ringR0; r1 = ringR1; return ringOn; }

  // Where the colors are heading (true = tapped colors).
  bool isInverted() const { return wiping ? (wipeDir > 0 ? !inverted : inverted) : inverted; }

 private:
  AnimConfig c;
  Scene s;
  Spring br;
  Smooth2 yaw, pitch, roll;
  float tYaw = 0, tPitch = 0, tRoll = 0;
  bool inverted = false, wiping = false, ringOn = false;
  float wipeK = 0, wipeDir = 1, prevWipeR = 0, ringR0 = 0, ringR1 = 0;
  uint32_t tapTime = 0;
  int lookStep = -1;
  uint32_t lookStepStart = 0;
  uint32_t nextGaze = 0, nextBlink = 0, blinkStart = 0;
  int blinksLeft = 0;
  uint32_t t0 = 0, last = 0;
  uint32_t rng = 1;

  uint32_t next() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
  uint32_t rand(uint32_t lo, uint32_t hi) { return lo + next() % (hi - lo + 1); }
  float frand(float lo, float hi) { return lo + (hi - lo) * (next() % 10001) / 10000.f; }
  static float ease(float x) { return x * x * (3.f - 2.f * x); }

  // renderer terms: fg = the ball (the face), bg = what shows through the eye holes
  void setPalette(bool inv, Color &bg, Color &fg) const {
    bg = hexColor(inv ? c.tapEyes : c.idleEyes);
    fg = hexColor(inv ? c.tapFace : c.idleFace);
  }

  void goHome() { tYaw = c.homeYaw * DEG; tPitch = c.homePitch * DEG; tRoll = c.homeRoll * DEG; }

  void pickIdleGaze() {
    uint32_t r = next() % 100;
    if (r < 55) {          // small drift around the resting pose
      tYaw = (c.homeYaw + frand(-c.wander, c.wander)) * DEG;
      tPitch = (c.homePitch + frand(-c.wander * 0.7f, c.wander * 0.7f)) * DEG;
      tRoll = (c.homeRoll + frand(-3.f, 3.f)) * DEG;
    } else if (r < 75) {   // back to the exact reference pose
      goHome();
    } else if (r < 88) {   // glance at the viewer
      tYaw = frand(-6.f, 6.f) * DEG; tPitch = frand(-4.f, 6.f) * DEG; tRoll = frand(-3.f, 3.f) * DEG;
    } else {               // glance to the other side
      tYaw = frand(-30.f, -15.f) * DEG; tPitch = frand(5.f, 22.f) * DEG; tRoll = frand(2.f, 8.f) * DEG;
    }
  }

  void startBlink(uint32_t now, bool allowDouble) {
    blinkStart = now ? now : 1;
    blinksLeft = (allowDouble && next() % 100 < 20) ? 2 : 1;
  }

  void onTap(uint32_t now) {
    tapTime = now;
    if (wiping) wipeDir = -wipeDir;       // tapped mid-wipe: run it backwards
    else { wiping = true; wipeK = 0; wipeDir = 1; prevWipeR = 0; }
    // look around, with a little "boing"
    lookStep = 0; lookStepStart = now;
    br.v += 140.f;
    // let a blink that's already happening finish smoothly (snapping the
    // eyes open would be a visible pop); just skip any double blink
    if (blinkStart) blinksLeft = 1;
    nextBlink = now + 4000;   // the look-around ends with its own blink
  }
};

}  // namespace eyes
