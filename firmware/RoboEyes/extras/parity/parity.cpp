// Renders a scripted scenario with the board's eye code, for the web app's port test.
// From the repo root:
//   clang++ -O2 -std=c++17 -o /tmp/robo_parity firmware/RoboEyes/extras/parity/parity.cpp
//   mkdir -p /tmp/robo_parity_out && /tmp/robo_parity /tmp/robo_parity_out
//   node test/roboeyes_port.test.mjs /tmp/robo_parity_out
// The same script lives in test/roboeyes_port.test.mjs. Keep the two in sync.
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>
#include "../../eyes_render.h"
#include "../../eyes_anim.h"
using namespace eyes;

static const int N = 466;

static AnimConfig config() {   // same as the defaults at the top of RoboEyes.ino
  AnimConfig c;
  c.face = 0xFFFFFF; c.eyes = 0x000000;
  c.faceRadius = 236; c.homeYaw = 28; c.homePitch = 27; c.homeRoll = -10;
  c.wander = 10; c.drift = 4; c.gazeSpeed = 1.0f; c.followYaw = 40; c.followPitch = 30;
  c.wipeMs = 650;
  c.eye = { 0.25f, 0.0f, 0.10f, 0.11f, 0.0f }; c.screenSize = N;
  return c;
}

static void dump(const Animator &a, const std::string &path) {
  Prepared p; prepare(a.scene(), p);
  std::vector<uint16_t> f(N * N);
  renderRect(a.scene(), p, makeXform(0, N), f.data(), 0, 0, N, N);
  for (auto &v : f) v = (uint16_t)((v >> 8) | (v << 8));   // un-swap: plain RGB565, little-endian file
  FILE *fp = fopen(path.c_str(), "wb"); fwrite(f.data(), 2, f.size(), fp); fclose(fp);
}

int main(int argc, char **argv) {
  std::string dir = argc > 1 ? argv[1] : "parity_out";
  const uint32_t shots[] = { 1400, 1890, 2700, 2900, 3420, 3600, 4330, 4420, 4700, 5090, 5800, 6200, 6800, 7700, 8400, 8990 };
  // ---- linked (E lines): mood, look, blinks; touches swap colors mid-way; colors set from the app ----
  {
    Animator a; a.begin(config(), 42, 1000);
    int k = 0;
    for (uint32_t t = 1000; t <= 9000; t += 16) {
      a.setLinked(true); a.setAutoBlink(false);
      a.setMood(t < 2500 ? NEUTRAL : t < 4000 ? HAPPY : t < 5500 ? SIDE : t < 7000 ? SAD : NEUTRAL);
      a.setLook((float)(0.6 * sin(t / 700.0)), (float)(0.4 * cos(t / 900.0)));
      if (t == 1800 || t == 6096) a.blinkNow(t, false);
      if (t == 3304) a.blinkNow(t, true);
      if (t == 4200) a.touch(true);
      if (t == 4296) a.touch(false);
      if (t == 4504) a.touch(true);    // touched again mid-wipe: the swap runs backwards
      if (t == 4600) a.touch(false);
      if (t == 5000) a.setColors(0x14213D, 0xFF5FA2);
      if (t == 7496) a.touch(true);
      if (t == 7608) a.touch(false);
      a.update(t);
      for (uint32_t s : shots) if (s >= t && s < t + 16) dump(a, dir + "/linked_" + std::to_string(k++) + ".bin");
    }
  }
  // ---- standalone: blinks, glances, floats; a touch swaps the colors ----
  {
    Animator a; a.begin(config(), 7, 1000);
    int k = 0;
    for (uint32_t t = 1000; t <= 9000; t += 16) {
      if (t == 3000) a.touch(true);
      if (t == 3208) a.touch(false);
      if (t == 5000) a.mirrorBlink(t);   // the web app saw you blink
      if (t == 6008) a.setColors(0xFFE8D6, 0x2A9D8F);
      if (t == 7000) a.touch(true);
      if (t == 7000) a.touch(false);   // the web app's TAP: down + up at once
      a.update(t);
      for (uint32_t s : shots) if (s >= t && s < t + 16) dump(a, dir + "/solo_" + std::to_string(k++) + ".bin");
    }
  }
  // ---- moves: a finger drags the eyes around, darts (one cut short by the other), a shake ----
  const uint32_t mshots[] = { 1500, 1700, 2000, 2400, 2800, 3450, 3600, 3900, 4300, 5100, 5300, 5600, 6000, 6400, 7200, 8500 };
  {
    Animator a; a.begin(config(), 99, 1000);
    int k = 0;
    for (uint32_t t = 1000; t <= 9000; t += 16) {
      if (t >= 1400 && t < 2600) a.follow((float)(0.6 * cos((t - 1400) / 300.0)), (float)(0.5 * sin((t - 1400) / 300.0)), t);
      if (t >= 2600 && t < 2616) a.followEnd(t);
      if (t >= 3400 && t < 3416) a.dart(-1, t);
      if (t >= 3800 && t < 3816) a.dart(1, t);   // mid-dart: straight into the other one
      if (t >= 5000 && t < 5016) a.shake(1234567, t);
      a.update(t);
      for (uint32_t s : mshots) if (s >= t && s < t + 16) dump(a, dir + "/moves_" + std::to_string(k++) + ".bin");
    }
  }
  // ---- heart eyes: pop in, beat, blink, dart and shake as hearts, melt back ----
  const uint32_t hshots[] = { 1530, 1580, 1650, 1800, 2050, 2600, 2700, 3150, 3600, 4300, 4600, 4700, 4850, 5300 };
  {
    Animator a; a.begin(config(), 5, 1000);
    int k = 0;
    for (uint32_t t = 1000; t <= 6000; t += 16) {
      if (t >= 1500 && t < 1516) a.setHearts(true, t);
      if (t >= 2600 && t < 2616) a.blinkNow(t, false);
      if (t >= 3000 && t < 3016) a.dart(1, t);
      if (t >= 3500 && t < 3516) a.shake(777, t);
      if (t >= 4500 && t < 4516) a.setHearts(false, t);
      a.update(t);
      for (uint32_t s : hshots) if (s >= t && s < t + 16) dump(a, dir + "/hearts_" + std::to_string(k++) + ".bin");
    }
  }
  printf("wrote %zu + %zu + %zu + %zu frames to %s\n", sizeof shots / sizeof *shots, sizeof shots / sizeof *shots, sizeof mshots / sizeof *mshots, sizeof hshots / sizeof *hshots, dir.c_str());
  return 0;
}
