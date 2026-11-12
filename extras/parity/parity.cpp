// Renders a scripted scenario with the board's eye code, for the website's port test.
//   clang++ -O2 -std=c++17 -o parity extras/parity/parity.cpp && ./parity <out_dir>
// then, in the website repo:  node test/eyes_port.test.mjs <out_dir>
// The same script lives in test/eyes_port.test.mjs. Keep the two in sync.
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
  c.idleFace = 0xFFFFFF; c.idleEyes = 0x000000; c.tapFace = 0x0B0B0E; c.tapEyes = 0xF4F4F4;
  c.faceRadius = 236; c.homeYaw = 28; c.homePitch = 27; c.homeRoll = -10;
  c.wander = 10; c.drift = 4; c.gazeSpeed = 1.0f; c.followYaw = 40; c.followPitch = 30;
  c.wipeMs = 650; c.autoReturnMs = 0;
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
  // ---- linked: the website drives mood, look, blinks; the board's tap + colors mirrored ----
  {
    Animator a; a.begin(config(), 42, 1000);
    int k = 0;
    for (uint32_t t = 1000; t <= 9000; t += 16) {
      a.setLinked(true); a.setAutoBlink(false);
      a.setMood(t < 2500 ? NEUTRAL : t < 4000 ? HAPPY : t < 5500 ? SIDE : t < 7000 ? SAD : NEUTRAL);
      a.setLook((float)(0.6 * sin(t / 700.0)), (float)(0.4 * cos(t / 900.0)));
      if (t == 1800 || t == 6096) a.blinkNow(t, false);
      if (t == 3304) a.blinkNow(t, true);
      if (t == 4200) a.setInverted(true, t);
      if (t == 4504) a.setInverted(false, t);   // changed mind mid-wipe
      if (t == 5000) a.lookAround(t);
      if (t == 7496) a.setInverted(true, t);
      a.update(t);
      for (uint32_t s : shots) if (s >= t && s < t + 16) dump(a, dir + "/linked_" + std::to_string(k++) + ".bin");
    }
  }
  // ---- standalone: blinks, glances and floats on its own (same random seed) ----
  {
    Animator a; a.begin(config(), 7, 1000);
    int k = 0;
    for (uint32_t t = 1000; t <= 9000; t += 16) {
      if (t == 3000) a.tap(t);
      a.update(t);
      for (uint32_t s : shots) if (s >= t && s < t + 16) dump(a, dir + "/solo_" + std::to_string(k++) + ".bin");
    }
  }
  printf("wrote %zu + %zu frames to %s\n", sizeof shots / sizeof *shots, sizeof shots / sizeof *shots, dir.c_str());
  return 0;
}
