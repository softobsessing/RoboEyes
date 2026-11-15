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
  c.wipeMs = 650; c.smileMs = 2500;
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
  const uint32_t shots[] = { 1400, 1890, 2700, 2900, 3420, 3600, 4330, 4420, 4700, 5300, 5800, 6200, 6800, 7700, 8400, 8990 };
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
      if (t == 4200) a.touch(true, t);
      if (t == 4296) a.touch(false, t);
      if (t == 4504) a.touch(true, t);    // touched again mid-wipe: the swap runs backwards
      if (t == 4600) a.touch(false, t);
      if (t == 5000) a.setColors(0x14213D, 0xFF5FA2);
      if (t == 7496) a.touch(true, t);
      if (t == 7608) a.touch(false, t);
      a.update(t);
      for (uint32_t s : shots) if (s >= t && s < t + 16) dump(a, dir + "/linked_" + std::to_string(k++) + ".bin");
    }
  }
  // ---- standalone: blinks, glances, floats; a touch swaps colors + smiles, then back to pills ----
  {
    Animator a; a.begin(config(), 7, 1000);
    int k = 0;
    for (uint32_t t = 1000; t <= 9000; t += 16) {
      if (t == 3000) a.touch(true, t);
      if (t == 3208) a.touch(false, t);
      if (t == 6008) a.setColors(0xFFE8D6, 0x2A9D8F);
      if (t == 7000) a.touch(true, t);
      if (t == 7000) a.touch(false, t);   // the web app's TAP: down + up at once
      a.update(t);
      for (uint32_t s : shots) if (s >= t && s < t + 16) dump(a, dir + "/solo_" + std::to_string(k++) + ".bin");
    }
  }
  printf("wrote %zu + %zu frames to %s\n", sizeof shots / sizeof *shots, sizeof shots / sizeof *shots, dir.c_str());
  return 0;
}
