// eyes_anim.h - decides where the eyes look, how they feel, when they blink.
// Produces an eyes::Scene every frame. Pure C++ so it can run in the PC preview.
//
// The website (softobsessing.github.io/doomscroll-eyes) runs a line-for-line
// JavaScript port of this file + eyes_render.h as its live preview (eyes.js).
// If you change shapes, moods or timings here, change eyes.js too and run its
// pixel test (see extras/preview/README in this repo).
//
// Two modes:
//  - standalone: blinks by itself, glances around a resting pose, floats.
//  - linked (the website is connected): the website picks the mood and where
//    to look, and tells the board when to blink, so board and preview match.
//
// Smoothness notes:
//  - Every motion is time-based, so it plays at the same speed at any frame rate.
//  - Gaze and eyelids use a two-stage smoothing filter: each move eases in AND
//    out (no sudden start), and a new target mid-move blends in without a jerk.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "eyes_render.h"

namespace eyes {

static const float DEG = 3.14159265f / 180.f;

// ---- moods (same names as the website's protocol) ----
enum Mood { NEUTRAL = 0, HAPPY = 1, SIDE = 2, SAD = 3 };
static const char *const MOOD_NAMES[4] = { "neutral", "happy", "side", "sad" };

static inline bool parseMood(const char *name, Mood *out) {
  for (int i = 0; i < 4; i++)
    if (strcmp(name, MOOD_NAMES[i]) == 0) { *out = (Mood)i; return true; }
  return false;
}

// How each mood shapes the eyelids, plus a small head tilt (degrees, + = up).
struct MoodStyle { float lidTop, lidSlant, lidHappy, pitch; };
static const MoodStyle MOOD_STYLES[4] = {
  //  top    slant  happy  pitch
  { 0.00f, 0.00f, 0.00f,  0.f },   // neutral: the reference pills
  { 0.00f, 0.00f, 1.00f,  4.f },   // happy ("welcome back"): arches, ^ ^
  { 0.46f, 0.00f, 0.00f,  0.f },   // side-eye: flat, half-closed upper lids
  { 0.34f, 0.85f, 0.00f, -8.f },   // sad ("disappointed"): droopy outer corners, head down
};

// The whole round screen is the ball's face; only the eyes are drawn on it.
struct AnimConfig {
  uint32_t idleFace, idleEyes;      // colors before tapping
  uint32_t tapFace, tapEyes;        // colors after tapping
  float faceRadius;                 // px; slightly larger than the screen radius
  float homeYaw, homePitch, homeRoll;   // degrees: resting gaze (from the reference art)
  float wander;                     // degrees of gaze wander around home (new target every few s)
  float drift;                      // degrees of slow, continuous floating on top of that
  float gazeSpeed;                  // 1 = default; lower = slower, silkier eye movement
  float followYaw, followPitch;     // degrees the eyes turn for a full-scale website look value
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
    lidTop.reset(0); lidSlant.reset(0); lidHappy.reset(0); moodPitch.reset(0);
    mood = NEUTRAL; linked = false; autoBlink = true;
    lookX = lookY = 0; jitX = jitY = 0; nextJitter = now;
    inverted = false;
    wiping = false; wipeK = 0; wipeDir = 1; prevWipeR = 0; ringOn = false;
    lookStep = -1;
    nextGaze = now + 1500;
    nextBlink = now + 1800;
    blinkStart = 0; blinksLeft = 0; blinkEvent = false; blinkEventTwice = false;
    t0 = now; last = now;
  }

  // ---------------- control ----------------
  void setLinked(bool on) {
    if (linked && !on) nextGaze = last;   // back on our own: pick a new glance right away
    linked = on;
  }
  void setMood(Mood m) { mood = m; }
  // -1..1; + = toward the right / bottom of the screen as the viewer sees it
  void setLook(float x, float y) {
    lookX = x < -1 ? -1 : (x > 1 ? 1 : x);
    lookY = y < -1 ? -1 : (y > 1 ? 1 : y);
  }
  void setAutoBlink(bool on) { autoBlink = on; }
  // Blink right now (the website's blink clock). Returns false if already blinking.
  bool blinkNow(uint32_t now, bool twice) {
    if (blinkStart) return false;
    startBlink(now, twice ? 2 : 1);
    return true;
  }
  // The look-around that plays when the screen is tapped.
  void lookAround(uint32_t now) {
    lookStep = 0; lookStepStart = now;
    br.v += 140.f;   // a little "boing"
    // let a blink that's already happening finish smoothly (snapping the
    // eyes open would be a visible pop); just skip any double blink
    if (blinkStart) blinksLeft = 1;
    nextBlink = now + 4000;   // the look-around ends with its own blink
  }
  // Animate the circular color wipe toward the tapped (true) or idle (false) colors.
  void setInverted(bool inv, uint32_t now) {
    if (inv == isInverted()) return;
    tapTime = now;
    if (wiping) wipeDir = -wipeDir;       // changed mind mid-wipe: run it backwards
    else { wiping = true; wipeK = 0; wipeDir = 1; prevWipeR = 0; }
  }
  // What a tap does on the board: look around + flip the colors.
  void tap(uint32_t now) { lookAround(now); setInverted(!isInverted(), now); }

  // ---------------- per frame ----------------
  void update(uint32_t now) {
    float dt = (int32_t)(now - last) * 0.001f;
    if (dt < 0) dt = 0;
    if (dt > 0.1f) dt = 0.1f;
    last = now;
    float t = (now - t0) * 0.001f;

    if (c.autoReturnMs && isInverted() && !wiping && now - tapTime > c.autoReturnMs) setInverted(false, now);

    // ---- small random glances while the website drives the gaze ----
    if (linked && autoBlink) {
      if ((int32_t)(now - nextJitter) >= 0) {
        jitX = frand(-0.05f, 0.05f);
        jitY = frand(-0.04f, 0.04f);
        nextJitter = now + rand(400, 1600);
      }
    } else {
      jitX = jitY = 0;
    }

    // ---- gaze targets ----
    float omega = 5.5f;
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
        if (!linked) { goHome(); nextGaze = now + rand(1800, 3000); }
        if (autoBlink) { startBlink(now, 1); blinkEvent = true; blinkEventTwice = false; }
      }
    }
    if (lookStep < 0) {
      if (linked) {
        omega = 11.f;
        float lx = lookX + jitX, ly = lookY + jitY;
        tYaw = lx * c.followYaw * DEG;
        tPitch = -ly * c.followPitch * DEG;
        tRoll = -lx * 5.f * DEG;
      } else {
        omega = 5.5f;   // idle: slow, gentle glides
        if ((int32_t)(now - nextGaze) >= 0) {
          pickIdleGaze();
          nextGaze = now + rand(1500, 4000);
        }
      }
    }
    omega *= c.gazeSpeed;
    yaw.step(tYaw, omega, dt);
    pitch.step(tPitch, omega, dt);
    roll.step(tRoll, omega * 0.8f, dt);

    // ---- mood eyelids morph smoothly ----
    const MoodStyle &ms = MOOD_STYLES[mood];
    lidTop.step(ms.lidTop, 11.f, dt);
    lidSlant.step(ms.lidSlant, 11.f, dt);
    lidHappy.step(ms.lidHappy, 11.f, dt);
    moodPitch.step(ms.pitch * DEG, 8.f, dt);

    // ---- slow continuous float, so the eyes never sit perfectly still ----
    float fx = 0, fy = 0;
    if (!linked) {
      fx = c.drift * DEG * (sinf(t * 0.63f + 0.7f) * 0.7f + sinf(t * 1.17f) * 0.3f);
      fy = c.drift * DEG * (sinf(t * 0.51f) * 0.6f + sinf(t * 0.93f + 2.1f) * 0.4f);
    }
    // face radius only ever bulges outward (tap "boing"), so the rim never shows
    br.step(c.faceRadius, 9.f, dt);
    if (br.x < c.faceRadius) { br.x = c.faceRadius; if (br.v < 0) br.v = 0; }

    // ---- blinking (both eyes together) ----
    if (autoBlink && blinkStart == 0 && (int32_t)(now - nextBlink) >= 0) {
      bool twice = next() % 100 < 20;
      startBlink(now, twice ? 2 : 1);
      blinkEvent = true; blinkEventTwice = twice;
    }
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

    // ---- color wipe (circle from the center; reverses if changed mid-way) ----
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
    s.yaw = yaw.value() + fx;
    s.pitch = pitch.value() + fy + moodPitch.value();
    s.roll = roll.value();
    s.blink = open;
    s.lidTop = lidTop.value();
    s.lidSlant = lidSlant.value();
    s.lidHappy = lidHappy.value();
    setPalette(inverted, s.bgA, s.fgA);
    setPalette(!inverted, s.bgB, s.fgB);
    s.wipeX = half; s.wipeY = half;
    s.wipeR = wiping ? fmaxf(wipeR, 0.001f) : 0.f;
    s.eye = c.eye;
  }

  // ---------------- output ----------------
  const Scene &scene() const { return s; }

  // The ring of the wipe that changed this frame (screen-centered), if any.
  bool wipeRing(float &r0, float &r1) const { r0 = ringR0; r1 = ringR1; return ringOn; }

  // Where the colors are heading (true = tapped colors).
  bool isInverted() const { return wiping ? (wipeDir > 0 ? !inverted : inverted) : inverted; }

  // A blink this animator started by itself (so the website can tell the board).
  bool takeBlinkEvent(bool &twice) {
    if (!blinkEvent) return false;
    blinkEvent = false; twice = blinkEventTwice;
    return true;
  }
  float jitterX() const { return jitX; }
  float jitterY() const { return jitY; }
  Mood currentMood() const { return mood; }
  bool blinking() const { return blinkStart != 0; }

 private:
  AnimConfig c;
  Scene s;
  Spring br;
  Smooth2 yaw, pitch, roll, lidTop, lidSlant, lidHappy, moodPitch;
  float tYaw = 0, tPitch = 0, tRoll = 0;
  Mood mood = NEUTRAL;
  bool linked = false, autoBlink = true;
  float lookX = 0, lookY = 0, jitX = 0, jitY = 0;
  uint32_t nextJitter = 0;
  bool inverted = false, wiping = false, ringOn = false;
  float wipeK = 0, wipeDir = 1, prevWipeR = 0, ringR0 = 0, ringR1 = 0;
  uint32_t tapTime = 0;
  int lookStep = -1;
  uint32_t lookStepStart = 0;
  uint32_t nextGaze = 0, nextBlink = 0, blinkStart = 0;
  int blinksLeft = 0;
  bool blinkEvent = false, blinkEventTwice = false;
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

  void startBlink(uint32_t now, int count) {
    blinkStart = now ? now : 1;
    blinksLeft = count;
  }
};

}  // namespace eyes
