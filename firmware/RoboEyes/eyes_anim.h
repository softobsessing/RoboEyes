// eyes_anim.h - decides where the eyes look, how they feel, when they blink.
// Produces an eyes::Scene every frame. Pure C++ so it can run in the PC preview.
//
// The RoboEyes web app (roboeyes.js in the root of this repo) runs a
// line-for-line JavaScript port of this file + eyes_render.h as its live
// preview. If you change shapes, moods, colors or timings here, change
// roboeyes.js too and run its pixel test (see the README).
//
// Two modes:
//  - standalone: blinks by itself, glances around a resting pose, floats.
//  - linked (something sends E lines): it picks the mood and where to look,
//    and tells the board when to blink.
//
// Moves, on top of either mode (each one starts from wherever the eyes are, so
// nothing ever jumps):
//  - follow(): the point between the eyes glides to a finger and tracks it.
//  - dart(): a quick glance to the screen's left or right, then back.
//  - shake(): the eyes bounce around the round screen like a shaken toy, come to
//    rest, blink, and float home. A small physics sim, stepped every millisecond
//    so it plays the same at any frame rate.
//  The pills squash and stretch with their speed and wobble when they hit the edge.
//  - setHearts(): the pills melt into hearts (with a little pop) that beat
//    "lub-dub" until they're turned off. They still blink, glance and bounce.
//
// Touch: the face and eye colors swap (circular wipe). The eyes themselves
// don't change: same pills, same gaze. Touch again to swap back.
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

// After a blink copied from the person watching, no random blinks for this long.
static const uint32_t MIRROR_QUIET_MS = 9000;

// ---- moves ----
// How far the point between the eyes may go from the screen center, in ball
// radii: an ellipse, because the pair of eyes is wider than it is tall. Past
// this the outer eye would slide off the glass.
static const float REACH_X = 0.76f, REACH_Y = 0.84f;
static const float FOLLOW_OMEGA = 32.f;          // finger follow speed (higher = snappier, lower = floatier)
static const uint32_t FOLLOW_REST_MS = 900;      // after the finger lifts, the eyes stay put this long
static const float DART_X = 0.70f;               // how far a dart goes (ball radii)
static const uint32_t DART_HOLD_MS = 850;        // a dart looks over, holds, and starts back after this long
static const float SHAKE_BOUNCE = 0.85f;          // how bouncy the screen's edge is (1 = loses nothing)
static const uint32_t SHAKE_MAX_MS = 3800;       // a shake never lasts longer than this
static const int SHAKE_KICKS = 6;                // the shakes: when, and how hard (ball radii / s)
static const uint16_t SHAKE_KICK_MS[SHAKE_KICKS] = { 0, 170, 340, 510, 680, 850 };
static const float SHAKE_KICK_SPEED[SHAKE_KICKS] = { 8.0f, 7.5f, 7.0f, 6.5f, 5.5f, 4.5f };

// ---- heart eyes ----
static const uint32_t HEARTBEAT_MS = 1000;       // one "lub-dub" per second
static const float HEARTBEAT_LUB = 0.10f, HEARTBEAT_DUB = 0.06f;   // how much each beat swells the hearts

// The whole round screen is the ball's face; only the eyes are drawn on it.
struct AnimConfig {
  uint32_t face, eyes;              // starting colors (0xRRGGBB); a touch swaps them
  float faceRadius;                 // px; slightly larger than the screen radius
  float homeYaw, homePitch, homeRoll;   // degrees: resting gaze (from the reference art)
  float wander;                     // degrees of gaze wander around home (new target every few s)
  float drift;                      // degrees of slow, continuous floating on top of that
  float gazeSpeed;                  // 1 = default; lower = slower, silkier eye movement
  float followYaw, followPitch;     // degrees the eyes turn for a full-scale website look value
  uint32_t wipeMs;                  // how long the color swap takes
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
    face = toFace = c.face; eyes = toEyes = c.eyes; repaint = false;
    wiping = false; wipeK = 0; wipeDir = 1; prevWipeR = 0; ringOn = false;
    nextGaze = now + 1500;
    nextBlink = now + 1800;
    blinkStart = 0; blinksLeft = 0; blinkEvent = false; blinkEventTwice = false;
    quietUntil = now;
    t0 = now; last = now;
    act = ACT_NONE; actStart = now; physT = now; homeNext = false;
    baseYaw = yaw.value(); basePitch = pitch.value(); baseRoll = roll.value();
    baseX = cosf(basePitch) * sinf(baseYaw); baseY = -sinf(basePitch);
    velX = velY = 0; squash = squashV = 0; lean.x = lean.v = 0;
    hearts = false; heartK = heartV = 0; heartT0 = now;
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
  // Blink because the person watching just blinked (the web app's camera).
  // Its own random blinks pause while it's following yours, and come back if
  // you don't blink for a while (MIRROR_QUIET_MS).
  bool mirrorBlink(uint32_t now) {
    quietUntil = now + MIRROR_QUIET_MS;
    return blinkNow(now, false);
  }
  // New colors right away (the web app's color wheel). Same colors = nothing to do,
  // so an echo of what we already show never cuts a swap short.
  void setColors(uint32_t newFace, uint32_t newEyes) {
    newFace &= 0xFFFFFF; newEyes &= 0xFFFFFF;
    if (newFace == faceColor() && newEyes == eyeColor()) return;
    face = toFace = newFace; eyes = toEyes = newEyes;
    wiping = false; prevWipeR = 0;
    repaint = true;
  }
  // Swap the face and eye colors with a circular wipe (runs backwards if swapped again mid-way).
  void swapColors() {
    if (wiping) { wipeDir = -wipeDir; return; }
    toFace = eyes; toEyes = face;
    wiping = true; wipeK = 0; wipeDir = 1; prevWipeR = 0;
  }
  // A finger on the screen: swap the colors. Lifting it does nothing.
  void touch(bool down) { if (down) swapColors(); }

  // ---------------- moves ----------------
  enum { ACT_NONE = 0, ACT_FOLLOW = 1, ACT_DART = 2, ACT_SHAKE = 3 };
  // A finger at (x, y), in ball radii from the screen center (+x right, +y down):
  // the point between the eyes glides there. Call again whenever it moves.
  void follow(float x, float y, uint32_t now) {
    if (act != ACT_FOLLOW) { beginAct(ACT_FOLLOW, now); fol[0].reset(baseX); fol[1].reset(baseY); }
    clampReach(x, y);
    goalX = x; goalY = y;
  }
  // The finger lifted: the eyes stay where it left them for a moment, then carry on.
  void followEnd(uint32_t now) { if (act == ACT_FOLLOW) settle(now, FOLLOW_REST_MS, false); }
  // A quick glance to the screen's left (dir < 0) or right (dir > 0), then back.
  void dart(int dir, uint32_t now) {
    beginAct(ACT_DART, now);
    dartDir = dir < 0 ? -1.f : 1.f;
    dartY = baseY * 0.5f;
    dartW = dartV = 0.f;
  }
  // Shaken: the eyes bounce around the screen, come to rest, blink, float home.
  // The same seed gives the same bounces (the web app's preview uses the board's).
  void shake(uint32_t seed, uint32_t now) {
    beginAct(ACT_SHAKE, now);
    srng = seed ? seed : 0x2545F491u;
    bx = baseX; by = baseY; bvx = bvy = 0; kicks = 0;
    quietUntil = now + SHAKE_MAX_MS;   // no random blinks mid-shake
  }
  int action() const { return act; }

  // Heart eyes on / off. They pop in (a quick overshoot) and beat while on.
  void setHearts(bool on, uint32_t now) {
    if (on && !hearts) heartT0 = now;
    hearts = on;
  }
  bool heartsOn() const { return hearts; }

  // ---------------- per frame ----------------
  void update(uint32_t now) {
    float dt = (int32_t)(now - last) * 0.001f;
    if (dt < 0) dt = 0;
    if (dt > 0.1f) dt = 0.1f;
    last = now;
    float t = (now - t0) * 0.001f;

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
    float omega;
    if (linked) {
      omega = 11.f;
      float lx = lookX + jitX, ly = lookY + jitY;
      tYaw = lx * c.followYaw * DEG;
      tPitch = -ly * c.followPitch * DEG;
      tRoll = -lx * 5.f * DEG;
    } else {
      omega = 5.5f;   // idle: slow, gentle glides
      if ((int32_t)(now - nextGaze) >= 0) {
        if (homeNext) { goHome(); homeNext = false; } else pickIdleGaze();
        nextGaze = now + rand(1500, 4000);
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
    // ---- moves: the base pose (where the eyes point, before the slow float) ----
    stepMs(now);   // millisecond steps: the shake's bounces, the squash-and-stretch
    float sYaw = yaw.value(), sPitch = pitch.value();
    float w = 0.f, pYaw = sYaw, pPitch = sPitch;
    uint32_t at = now - actStart;
    if (act == ACT_FOLLOW) {
      fol[0].step(goalX, FOLLOW_OMEGA * c.gazeSpeed, dt);
      fol[1].step(goalY, FOLLOW_OMEGA * c.gazeSpeed, dt);
      pointToPose(fol[0].value(), fol[1].value(), pYaw, pPitch);
      w = 1.f;
    } else if (act == ACT_DART) {
      pointToPose(dartDir * DART_X, dartY, pYaw, pPitch);
      w = dartW;   // a springy 0 -> 1 -> 0, stepped every millisecond in stepMs()
      if (at > DART_HOLD_MS + 300 && fabsf(dartW) < 0.002f && fabsf(dartV) < 0.05f) { act = ACT_NONE; w = 0.f; }
    } else if (act == ACT_SHAKE) {
      pointToPose(bx, by, pYaw, pPitch);
      w = 1.f;
    }
    baseYaw = sYaw + (pYaw - sYaw) * w;
    basePitch = sPitch + (pPitch - sPitch) * w;
    // where that puts the eyes on screen, and how fast they're moving (ball radii / s)
    float nX = cosf(basePitch) * sinf(baseYaw), nY = -sinf(basePitch);
    if (dt > 0) { velX = (nX - baseX) / dt; velY = (nY - baseY) / dt; }
    baseX = nX; baseY = nY;
    lean.step(clampf(velX * 0.025f, -0.14f, 0.14f), 16.f, dt);   // lean into sideways motion
    baseRoll = roll.value() + lean.x;
    if (act == ACT_SHAKE && shakeDone(at)) {   // came to rest: a dazed double blink, then home
      settle(now, 700, true);
      if (!blinkStart) startBlink(now, 2);
      quietUntil = now + 2500;
    }

    // face radius only ever bulges outward, so the rim never shows
    br.step(c.faceRadius, 9.f, dt);
    if (br.x < c.faceRadius) { br.x = c.faceRadius; if (br.v < 0) br.v = 0; }

    // ---- blinking (both eyes together) ----
    if (autoBlink && blinkStart == 0 && (int32_t)(now - nextBlink) >= 0 && (int32_t)(now - quietUntil) >= 0) {
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

    // ---- color swap wipe (circle from the center; reverses if swapped again mid-way) ----
    const float endR = c.screenSize * 0.5f + 3.f;   // just past the visible edge
    float wipeR = 0.f;
    ringOn = false;
    if (wiping) {
      wipeK += wipeDir * dt * 1000.f / (float)c.wipeMs;
      if (wipeK >= 1.f) {                 // finished: the new colors everywhere
        wiping = false; face = toFace; eyes = toEyes;
        ringOn = true; ringR0 = prevWipeR - 2.f; ringR1 = 1e6f;
      } else if (wipeK <= 0.f && wipeDir < 0) {   // reversed all the way back
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
    s.yaw = baseYaw + fx;
    s.pitch = basePitch + fy + moodPitch.value();
    s.roll = baseRoll;
    s.blink = open;
    s.lidTop = lidTop.value();
    s.lidSlant = lidSlant.value();
    s.lidHappy = lidHappy.value();
    // renderer terms: fg = the ball (the face), bg = what shows through the eye holes
    s.fgA = hexColor(face);   s.bgA = hexColor(eyes);       // outside the wipe circle
    s.fgB = hexColor(wiping ? toFace : face);               // inside it
    s.bgB = hexColor(wiping ? toEyes : eyes);
    s.wipeX = half; s.wipeY = half;
    s.wipeR = wiping ? fmaxf(wipeR, 0.001f) : 0.f;
    // squash (+: wider and shorter) and stretch (-: narrower and taller), keeping the size about the same
    // heart eyes: the morph, plus the pop (overshoot) and the heartbeat swelling them
    float morph = heartK < 0.f ? 0.f : (heartK > 1.f ? 1.f : heartK);
    float pop = heartK > 1.f ? heartK - 1.f : 0.f;
    float beat = 0.f;
    if (hearts) {
      uint32_t bt = (now - heartT0) % HEARTBEAT_MS;
      beat = HEARTBEAT_LUB * bump(bt, 0, 140) + HEARTBEAT_DUB * bump(bt, 200, 140);
    }
    s.heart = morph;
    s.heartSize = 1.f + pop * 1.2f + beat * morph;
    s.eye = c.eye;
    s.eye.radius = c.eye.radius * (1.f + squash);
    s.eye.halfLen = fmaxf((c.eye.halfLen + c.eye.radius) * (1.f - 0.7f * squash) - s.eye.radius, 0.f);
  }

  // ---------------- output ----------------
  const Scene &scene() const { return s; }

  // The ring of the wipe that changed this frame (screen-centered), if any.
  bool wipeRing(float &r0, float &r1) const { r0 = ringR0; r1 = ringR1; return ringOn; }

  // The colors it shows (or is wiping toward).
  uint32_t faceColor() const { return wiping && wipeDir > 0 ? toFace : face; }
  uint32_t eyeColor() const { return wiping && wipeDir > 0 ? toEyes : eyes; }
  // True once after setColors(): the whole screen changed, redraw all of it.
  bool takeRepaint() { bool r = repaint; repaint = false; return r; }

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
  uint32_t face = 0, eyes = 0, toFace = 0, toEyes = 0;
  bool repaint = false, wiping = false, ringOn = false;
  float wipeK = 0, wipeDir = 1, prevWipeR = 0, ringR0 = 0, ringR1 = 0;
  uint32_t nextGaze = 0, nextBlink = 0, blinkStart = 0, quietUntil = 0;
  int blinksLeft = 0;
  bool blinkEvent = false, blinkEventTwice = false;
  uint32_t t0 = 0, last = 0;
  uint32_t rng = 1;

  uint32_t next() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
  uint32_t rand(uint32_t lo, uint32_t hi) { return lo + next() % (hi - lo + 1); }
  float frand(float lo, float hi) { return lo + (hi - lo) * (next() % 10001) / 10000.f; }
  static float ease(float x) { return x * x * (3.f - 2.f * x); }

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

  // ---- moves ----
  int act = ACT_NONE;
  uint32_t actStart = 0, physT = 0;
  bool homeNext = false;
  Smooth2 fol[2];                         // follow: the eyes' point, chasing the finger
  float goalX = 0, goalY = 0;
  float dartDir = 1, dartY = 0, dartW = 0, dartV = 0;
  float bx = 0, by = 0, bvx = 0, bvy = 0; // shake: position + velocity (ball radii, per s)
  int kicks = 0;
  uint32_t srng = 1;
  float baseYaw = 0, basePitch = 0, baseRoll = 0, baseX = 0, baseY = 0, velX = 0, velY = 0;
  float squash = 0, squashV = 0;
  Spring lean;
  bool hearts = false;
  float heartK = 0, heartV = 0;   // heart morph (springy; past 1 = popping)
  uint32_t heartT0 = 0;
  // 0 -> 1 -> 0 over [start, start + len) ms, smooth at both ends
  static float bump(uint32_t t, uint32_t start, uint32_t len) {
    if (t < start || t >= start + len) return 0.f;
    float u = sinf((t - start) * 3.14159265f / len);
    return u * u;
  }

  static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
  uint32_t snext() { srng ^= srng << 13; srng ^= srng >> 17; srng ^= srng << 5; return srng; }

  // A point on the screen (ball radii from the center) -> the yaw/pitch that puts the eyes there.
  static void pointToPose(float x, float y, float &yw, float &pt) {
    pt = asinf(clampf(-y, -0.97f, 0.97f));
    yw = asinf(clampf(x / cosf(pt), -0.97f, 0.97f));
  }
  static void clampReach(float &x, float &y) {
    float qx = x / REACH_X, qy = y / REACH_Y, q = qx * qx + qy * qy;
    if (q > 1.f) { float k = 1.f / sqrtf(q); x *= k; y *= k; }
  }
  // Start a move from wherever the eyes are right now (even mid-move), so nothing jumps.
  void beginAct(int kind, uint32_t now) {
    if (act != ACT_NONE) { yaw.reset(baseYaw); pitch.reset(basePitch); }
    act = kind; actStart = now;
  }
  // End a move: stay right here for restMs, then glide on (home first, if asked).
  void settle(uint32_t now, uint32_t restMs, bool home) {
    yaw.reset(baseYaw); pitch.reset(basePitch);
    tYaw = baseYaw; tPitch = basePitch;
    act = ACT_NONE; nextGaze = now + restMs; homeNext = home;
  }

  // Everything that runs in fixed 1 ms steps (so it's the same at any frame rate).
  void stepMs(uint32_t now) {
    int32_t n = (int32_t)(now - physT);
    if (n <= 0) return;
    if (n > 100) { physT = now - 100; n = 100; }
    // stretch along the motion: moving sideways = wider pills, up/down = taller
    float sp = sqrtf(velX * velX + velY * velY), tgt = 0.f;
    if (sp > 0.5f) tgt = (fabsf(velX) - fabsf(velY)) / sp * fminf(sp / 8.f, 1.f) * 0.16f;
    for (int32_t i = 0; i < n; i++) {
      physT++;
      int32_t at = (int32_t)(physT - actStart);   // (a move can start mid-frame)
      if (act == ACT_SHAKE && at >= 0) shakeStep((uint32_t)at);
      {   // heart eyes: a springy 0 -> 1 (pops past 1 for a moment) or a soft 1 -> 0
        float om = hearts ? 16.f : 14.f, z = hearts ? 0.42f : 1.f;
        float a = -om * om * (heartK - (hearts ? 1.f : 0.f)) - 2.f * z * om * heartV;
        heartV += a * 0.001f;
        heartK += heartV * 0.001f;
      }
      if (act == ACT_DART && at >= 0) {
        // over: a quick spring that overshoots a little; back: a soft one that doesn't
        bool out = at < (int32_t)DART_HOLD_MS;
        float om = out ? 24.f : 13.f, z = out ? 0.6f : 1.f;
        float a = -om * om * (dartW - (out ? 1.f : 0.f)) - 2.f * z * om * dartV;
        dartV += a * 0.001f;
        dartW += dartV * 0.001f;
      }
      // an underdamped spring, so a hit wobbles like jelly before it settles
      float a = -900.f * (squash - tgt) - 16.f * squashV;
      squashV += a * 0.001f;
      squash = clampf(squash + squashV * 0.001f, -0.35f, 0.45f);
    }
  }

  // One millisecond of being shaken: kicks, drag, bounces off the screen's edge.
  void shakeStep(uint32_t t) {
    if (kicks < SHAKE_KICKS && t >= SHAKE_KICK_MS[kicks]) {
      float ang = (snext() % 3600) * (6.2831853f / 3600.f);
      bvx = bvx * 0.4f + cosf(ang) * SHAKE_KICK_SPEED[kicks];
      bvy = bvy * 0.4f + sinf(ang) * SHAKE_KICK_SPEED[kicks];
      kicks++;
    }
    // light air drag while it's being shaken, then more and more so it comes to rest gently
    float drag = t < 1500 ? 0.9f : fminf(0.9f + (t - 1500) * (5.f / 700.f), 6.f);
    float k = 1.f - drag * 0.001f;
    bvx *= k; bvy *= k;
    bx += bvx * 0.001f; by += bvy * 0.001f;
    float qx = bx / REACH_X, qy = by / REACH_Y, q = qx * qx + qy * qy;
    if (q > 1.f) {                         // hit the edge: back onto it, and bounce
      float r = 1.f / sqrtf(q);
      bx *= r; by *= r;
      float nx = bx / (REACH_X * REACH_X), ny = by / (REACH_Y * REACH_Y);
      float nl = sqrtf(nx * nx + ny * ny);
      nx /= nl; ny /= nl;
      float vn = bvx * nx + bvy * ny;
      if (vn > 0.f) {
        bvx -= (1.f + SHAKE_BOUNCE) * vn * nx;
        bvy -= (1.f + SHAKE_BOUNCE) * vn * ny;
        squashV -= (nx * nx - ny * ny) * vn * 2.5f;   // side hit: squeezed narrow; top/bottom: squashed flat
      }
    }
  }
  bool shakeDone(uint32_t at) const {
    return at > SHAKE_MAX_MS || (kicks >= SHAKE_KICKS && at > 1200 && bvx * bvx + bvy * bvy < 0.0036f);
  }
};

}  // namespace eyes
