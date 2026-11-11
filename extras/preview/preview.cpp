// PC preview: runs the real animation + renderer and writes a contact sheet.
//   clang++ -O2 -std=c++17 -o preview extras/preview/preview.cpp
//   ./preview sheet.ppm [tap_ms...]    (frames every 50 ms, default tap at 2000 ms)
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../../eyes_render.h"
#include "../../eyes_anim.h"
using namespace eyes;

int main(int argc, char **argv) {
  const int N = 466, COLS = 8, ROWS = 6, S = 2;     // each tile = 233 px (half size)
  const int T = N / S, STEP_MS = 50, FIRST_MS = 1700;
  std::vector<uint32_t> taps;
  for (int i = 2; i < argc; i++) taps.push_back(atoi(argv[i]));
  if (taps.empty()) taps.push_back(2000);

  AnimConfig c;
  // keep in sync with the settings at the top of RoboEyes.ino
  c.idleFace = 0xFFFFFF; c.idleEyes = 0x000000; c.tapFace = 0x0B0B0E; c.tapEyes = 0xF4F4F4;
  c.faceRadius = 236; c.homeYaw = 28; c.homePitch = 27; c.homeRoll = -10;
  c.wander = 10; c.drift = 4; c.wipeMs = 550; c.autoReturnMs = 0;
  c.eye = { 0.25f, 0.0f, 0.10f, 0.11f, 0.0f }; c.screenSize = N;
  Animator a; a.begin(c, 12345, 1);

  std::vector<uint16_t> frame(N * N);
  std::vector<unsigned char> sheet(COLS * T * ROWS * T * 3, 110);
  Xform xf = makeXform(0, N);
  int tile = 0;
  for (uint32_t t = 1; tile < COLS * ROWS; t += 10) {     // simulate at 100 fps
    bool tap = false;
    for (uint32_t tp : taps) if (t <= tp && tp < t + 10) tap = true;
    a.update(t, tap);
    if (t >= FIRST_MS && (t - FIRST_MS) % STEP_MS < 10) {
      Prepared p; prepare(a.scene(), p);
      renderRect(a.scene(), p, xf, frame.data(), 0, 0, N, N);
      int ox = (tile % COLS) * T, oy = (tile / COLS) * T;
      for (int y = 0; y < T; y++) for (int x = 0; x < T; x++) {
        float r = 0, g = 0, b = 0;
        for (int sy = 0; sy < S; sy++) for (int sx = 0; sx < S; sx++) {
          uint16_t v = frame[(y * S + sy) * N + x * S + sx]; v = (v >> 8) | (v << 8);
          r += (v >> 11) * 255.f / 31; g += ((v >> 5) & 63) * 255.f / 63; b += (v & 31) * 255.f / 31;
        }
        float dx = x + 0.5f - T / 2.f, dy = y + 0.5f - T / 2.f;
        bool in = dx * dx + dy * dy <= (T / 2.f) * (T / 2.f);
        unsigned char *o = &sheet[((oy + y) * COLS * T + ox + x) * 3];
        o[0] = in ? r / (S * S) : 110; o[1] = in ? g / (S * S) : 110; o[2] = in ? b / (S * S) : 110;
      }
      tile++;
    }
  }
  FILE *f = fopen(argv[1], "wb");
  fprintf(f, "P6\n%d %d\n255\n", COLS * T, ROWS * T);
  fwrite(sheet.data(), 1, sheet.size(), f);
  fclose(f);
  return 0;
}
