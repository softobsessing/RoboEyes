// eyes_anim.h - decides where the ball looks, when it blinks, and handles taps.
// Produces an eyes::Scene every frame. Pure C++ so it can run in the PC preview.
#pragma once
#include <stdint.h>
#include <math.h>
#include "eyes_render.h"

namespace eyes {

static const float DEG = 3.14159265f / 180.f;

struct AnimConfig {
  uint32_t idleBg, idleBall;        // colors before tapping
  uint32_t tapBg, tapBall;          // colors after tapping
  float ballRadius;                 // px
  float homeYaw, homePitch, homeRoll;   // degrees: resting gaze (from the reference art)
  float wander;                     // degrees of subtle gaze wander around home
  float drift;                      // px of gentle floating
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

class Animator {
 public:
  void begin(const AnimConfig &cfg, uint32_t seed, uint32_t now) {
    c = cfg;
    rng = seed ? seed : 0x9E3779B9u;
    float half = c.screenSize * 0.5f;
    bx.x = half; by.x = half; br.x = c.ballRadius;
    yaw.x = c.homeYaw * DEG; pitch.x = c.homePitch * DEG; roll.x = c.homeRoll * DEG;
    tYaw = yaw.x; tPitch = pitch.x; tRoll = roll.x;
    inverted = false;
    wipeStart = 0; wiping = false; justFinishedWipe = true;
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
    if (c.autoReturnMs && inverted && !wiping && now - tapTime > c.autoReturnMs) onTap(now);

    // ---- gaze targets ----
    float omega = 9.f;
    if (lookStep >= 0) {
      // quick look-around after a tap
      static const float seq[][3] = {   // yaw, pitch, roll (deg)
        { -38.f,  8.f,  10.f },
        {  42.f, 22.f, -14.f },
        { -10.f, -6.f,   4.f },
      };
      static const uint32_t dur[] = { 240, 280, 240 };
      const int n = sizeof(dur) / sizeof(dur[0]);
      if (lookStep < n) {
        tYaw = seq[lookStep][0] * DEG; tPitch = seq[lookStep][1] * DEG; tRoll = seq[lookStep][2] * DEG;
        omega = 16.f;
        if (now - lookStepStart > dur[lookStep]) { lookStep++; lookStepStart = now; }
      } else {
        lookStep = -1;
        goHome();
        nextGaze = now + rand(1400, 2600);
        startBlink(now, false);
      }
    } else if ((int32_t)(now - nextGaze) >= 0) {
      pickIdleGaze();
      nextGaze = now + rand(1200, 3600);
    }
    yaw.step(tYaw, omega, dt);
    pitch.step(tPitch, omega, dt);
    roll.step(tRoll, omega * 0.8f, dt);

    // ---- ball position: gentle float + lean toward where it's looking ----
    float half = c.screenSize * 0.5f;
    float fx = c.drift * (sinf(t * 0.63f + 0.7f) * 0.7f + sinf(t * 1.17f) * 0.3f);
    float fy = c.drift * (sinf(t * 0.51f) * 0.6f + sinf(t * 0.93f + 2.1f) * 0.4f);
    float leanX = (yaw.x - c.homeYaw * DEG) * 22.f;
    float leanY = -(pitch.x - c.homePitch * DEG) * 22.f;
    bx.step(half + fx + leanX, 6.f, dt);
    by.step(half + fy + leanY, 6.f, dt);
    br.step(c.ballRadius, 10.f, dt);

    // ---- blinking (both eyes together) ----
    if (blinkStart == 0 && (int32_t)(now - nextBlink) >= 0) startBlink(now, true);
    float open = 1.f;
    if (blinkStart && (int32_t)(now - blinkStart) >= 0) {
      uint32_t bt = now - blinkStart;
      const uint32_t CLOSE = 70, HOLD = 50, OPEN = 120;
      if (bt < CLOSE) open = 1.f - ease(bt / (float)CLOSE);
      else if (bt < CLOSE + HOLD) open = 0.f;
      else if (bt < CLOSE + HOLD + OPEN) open = ease((bt - CLOSE - HOLD) / (float)OPEN);
      else if (--blinksLeft > 0) blinkStart = now + 90;   // double blink after a short gap
      else { blinkStart = 0; nextBlink = now + rand(2500, 6000); }
    }

    // ---- color wipe ----
    float wipeR = 0.f;
    justFinishedWipe = false;
    if (wiping) {
      float k = (now - wipeStart) / (float)c.wipeMs;
      if (k >= 1.f) {
        wiping = false;
        inverted = wipeToInverted;
        justFinishedWipe = true;
      } else {
        float e = 1.f - (1.f - k) * (1.f - k) * (1.f - k);   // ease-out
        wipeR = 1.f + e * wipeMaxR;
      }
    }

    // ---- fill the scene ----
    s.ballX = bx.x; s.ballY = by.x; s.ballR = br.x;
    s.yaw = yaw.x; s.pitch = pitch.x; s.roll = roll.x;
    s.blink = open;
    bool baseInv = inverted;
    setPalette(baseInv, s.bgA, s.fgA);
    setPalette(wiping ? wipeToInverted : baseInv, s.bgB, s.fgB);
    s.wipeX = wipeX; s.wipeY = wipeY; s.wipeR = wiping ? wipeR : 0.f;
    s.eye = c.eye;
  }

  const Scene &scene() const { return s; }
  // True while the background is changing: the whole screen must be redrawn.
  bool needsFullRedraw() const { return wiping || justFinishedWipe; }
  bool isInverted() const { return wiping ? wipeToInverted : inverted; }

 private:
  AnimConfig c;
  Scene s;
  Spring bx, by, br, yaw, pitch, roll;
  float tYaw, tPitch, tRoll;
  bool inverted, wiping, wipeToInverted, justFinishedWipe;
  uint32_t wipeStart, tapTime = 0;
  float wipeX = 0, wipeY = 0, wipeMaxR = 0;
  int lookStep;
  uint32_t lookStepStart = 0;
  uint32_t nextGaze, nextBlink, blinkStart;
  int blinksLeft;
  uint32_t t0, last;
  uint32_t rng;

  uint32_t next() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
  uint32_t rand(uint32_t lo, uint32_t hi) { return lo + next() % (hi - lo + 1); }
  float frand(float lo, float hi) { return lo + (hi - lo) * (next() % 10001) / 10000.f; }
  static float ease(float x) { return x * x * (3.f - 2.f * x); }

  void setPalette(bool inv, Color &bg, Color &fg) const {
    bg = hexColor(inv ? c.tapBg : c.idleBg);
    fg = hexColor(inv ? c.tapBall : c.idleBall);
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
    if (wiping) inverted = wipeToInverted;     // finish any wipe in progress instantly
    wipeToInverted = !inverted;
    wiping = true;
    wipeStart = now;
    // wipe grows from the ball's center until it covers the farthest screen corner
    wipeX = bx.x; wipeY = by.x;
    float W = (float)c.screenSize, mx = 0;
    const float cx[4] = { 0, W, 0, W }, cy[4] = { 0, 0, W, W };
    for (int i = 0; i < 4; i++) {
      float dx = cx[i] - wipeX, dy = cy[i] - wipeY, d = sqrtf(dx * dx + dy * dy);
      if (d > mx) mx = d;
    }
    wipeMaxR = mx + 2.f;
    // look around, with a little "boing"
    lookStep = 0; lookStepStart = now;
    br.v += 260.f;
    blinkStart = 0; blinksLeft = 0;
    nextBlink = now + 4000;   // the look-around ends with its own blink
  }
};

}  // namespace eyes
