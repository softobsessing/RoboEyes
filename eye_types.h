#pragma once
#include <stdbool.h>

// Per-frame geometry of one eye, computed in buildEyes()
struct EyeDraw {
  float cx, cy, hw, hh, r;   // center, half-size, corner radius
  float lidY, lidSlant;      // upper lid line: y at center, slope toward nose
  float smileCx, smileCy, smileR;   // circle that cuts the eye from below
  bool hasSmile;
  int x0, y0, x1, y1;        // bounding box (screen coords)
};
