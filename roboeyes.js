// roboeyes.js — line-for-line port of the RoboEyes firmware's eye code
// (firmware/RoboEyes/eyes_render.h + eyes_anim.h), so the web app's preview shows what the round
// AMOLED shows.
//
// The character: the round screen is a 3D ball (the face), the eyes are two pill-shaped holes in
// it. The ball turns to look around, so the pills foreshorten near the rim. A touch swaps the face
// and eye colors with a circular wipe; the eyes themselves don't change.
//
// If you change shapes, moods, colors or timings in the firmware, change them here too and run
// the pixel test (see the README): test/roboeyes_port.test.mjs.

export const MOODS = ['neutral', 'happy', 'side', 'sad'];
const NEUTRAL = 0;
export const SCREEN = 466;
const DEG = Math.PI / 180;
// after a blink copied from the person watching, no random blinks for this long (ms)
const MIRROR_QUIET_MS = 9000;

// ---- moves (same constants as eyes_anim.h) ----
const REACH_X = 0.76, REACH_Y = 0.84;
const FOLLOW_OMEGA = 32;
const FOLLOW_REST_MS = 900;
const DART_X = 0.70;
const DART_HOLD_MS = 850;
const SHAKE_BOUNCE = 0.85;
const SHAKE_MAX_MS = 3800;
const SHAKE_KICK_MS = [0, 170, 340, 510, 680, 850];
const SHAKE_KICK_SPEED = [8.0, 7.5, 7.0, 6.5, 5.5, 4.5];
// heart eyes (same as eyes_anim.h)
const HEARTBEAT_MS = 1000, HEARTBEAT_LUB = 0.10, HEARTBEAT_DUB = 0.06;
const bump = (t, start, len) => { if (t < start || t >= start + len) return 0; const u = Math.sin((t - start) * Math.PI / len); return u * u; };
export const ACT = { NONE: 0, FOLLOW: 1, DART: 2, SHAKE: 3 };
const clampf = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
function pointToPose(x, y) {
  const pt = Math.asin(clampf(-y, -0.97, 0.97));
  return [Math.asin(clampf(x / Math.cos(pt), -0.97, 0.97)), pt];
}
function clampReach(x, y) {
  const qx = x / REACH_X, qy = y / REACH_Y, q = qx * qx + qy * qy;
  if (q > 1) { const k = 1 / Math.sqrt(q); return [x * k, y * k]; }
  return [x, y];
}

// ---- same defaults as the settings at the top of RoboEyes.ino ----
export const CONFIG = {
  face: 0xFFFFFF, eyes: 0x000000,   // white face, black eyes (a touch swaps them)
  faceRadius: 236,
  homeYaw: 28, homePitch: 27, homeRoll: -10,
  wander: 10, drift: 4, gazeSpeed: 1.0,
  followYaw: 40, followPitch: 30,
  wipeMs: 650,
  eye: { spacing: 0.25, height: 0.0, radius: 0.10, halfLen: 0.11, tilt: 0 },
  screenSize: SCREEN,
};

// lidTop, lidSlant, lidHappy, pitch (deg) — same table as eyes_anim.h
const MOOD_STYLES = [
  [0.00, 0.00, 0.00, 0],    // neutral: the reference pills
  [0.00, 0.00, 1.00, 4],    // happy ("welcome back"): arches, ^ ^
  [0.46, 0.00, 0.00, 0],    // side-eye: flat, half-closed upper lids
  [0.34, 0.85, 0.00, -8],   // sad ("disappointed"): droopy outer corners, head down
];

const clamp01 = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);
const hexColor = (c) => ({ r: (c >> 16) & 0xFF, g: (c >> 8) & 0xFF, b: c & 0xFF });

// RGB565 (not byte-swapped: the board swaps for its panel, the canvas doesn't need to)
function pack565(r, g, b) {
  let ir = Math.trunc(r + 0.5), ig = Math.trunc(g + 0.5), ib = Math.trunc(b + 0.5);
  ir = ir < 0 ? 0 : ir > 255 ? 255 : ir;
  ig = ig < 0 ? 0 : ig > 255 ? 255 : ig;
  ib = ib < 0 ? 0 : ib > 255 ? 255 : ib;
  return ((ir & 0xF8) << 8) | ((ig & 0xFC) << 3) | (ib >> 3);
}

// ===================================================================== renderer (eyes_render.h)

function faceToScreen(p, fu, fv) {
  const u = fu * p.cr - fv * p.sr;
  const v = fu * p.sr + fv * p.cr;
  const w = Math.sqrt(Math.max(0, 1 - u * u - v * v));
  return [p.m00 * u + p.m10 * v + p.m20 * w, p.m11 * v + p.m21 * w, p.m02 * u + p.m12 * v + p.m22 * w];
}

// heart size in face units per heartSize (same as HEART_K in eyes_render.h)
const HEART_K = 0.30;
// Inigo Quilez's exact heart SDF: tip at (0, 0), lobes on top (+y up), ~1.2 wide, 1.1 tall
function sdHeart(x, y) {
  x = Math.abs(x);
  if (y + x > 1) { const dx = x - 0.25, dy = y - 0.75; return Math.sqrt(dx * dx + dy * dy) - 0.35355339; }
  const ay = y - 1, d1 = x * x + ay * ay;
  const m = 0.5 * Math.max(x + y, 0), bx = x - m, by = y - m, d2 = bx * bx + by * by;
  return Math.sqrt(Math.min(d1, d2)) * (x > y ? 1 : -1);
}

export function prepare(s) {
  const p = {};
  const cy = Math.cos(s.yaw), sy = Math.sin(s.yaw);
  const cp = Math.cos(s.pitch), sp = Math.sin(s.pitch);
  p.m00 = cy; p.m02 = -sy;
  p.m10 = sp * sy; p.m11 = cp; p.m12 = sp * cy;
  p.m20 = cp * sy; p.m21 = -sp; p.m22 = cp * cy;
  p.cr = Math.cos(s.roll); p.sr = Math.sin(s.roll);
  p.ct = Math.cos(s.eye.tilt); p.st = Math.sin(s.eye.tilt);
  p.invR = 1 / s.ballR;
  p.r2in = (s.ballR - 1) * (s.ballR - 1);
  p.r2out = (s.ballR + 1) * (s.ballR + 1);

  const h = clamp01(s.lidHappy);
  const B = s.eye.radius * (1 + 0.6 * h), hl = s.eye.halfLen;
  const L = hl + B;
  p.L = L; p.B = B; p.hl = hl;
  p.topC = -L + 2 * L * clamp01(s.lidTop);
  p.topK = s.lidSlant * L / B;
  p.topNorm = 1 / Math.sqrt(1 + p.topK * p.topK);
  p.happyOn = h > 0.001;
  p.heart = clamp01(s.heart || 0);
  p.heartOn = p.heart > 0.001;
  p.heartK = HEART_K * (s.heartSize ?? 1);
  p.holeA = -hl + 0.1 * B;
  p.holeR = h * 0.55 * B;
  p.cutA = L + ((-hl + 0.3 * B) - L) * h;
  const aTop = Math.max(-L, p.topC);
  let aBot = p.cutA;
  if (aBot < aTop + 0.02) aBot = aTop + 0.02;
  const openMid = 0.5 * (aTop + aBot);
  const closeMid = openMid + (0.5 * (aTop + Math.max(aTop, p.holeA - p.holeR)) - openMid) * h;
  const bl = clamp01(s.blink);
  p.aMid = closeMid + (openMid - closeMid) * bl;
  p.openHalf = Math.max(bl * 0.5 * (aBot - aTop), 2.4 / s.ballR);

  let BB = B, LL = L;
  if (p.heartOn) { BB = Math.max(B, 0.61 * p.heartK); LL = Math.max(L, 0.60 * p.heartK); }
  const A0 = Math.max(-LL, p.aMid - p.openHalf), A1 = Math.min(LL, p.aMid + p.openHalf);
  const K = 6;
  p.eyeOn = [false, false];
  p.eyeBox = [null, null];
  for (let e = 0; e < 2; e++) {
    const side = e === 0 ? -1 : 1, st = p.st * side;
    let x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9, zmax = -2;
    for (let k = 0; k <= K; k++) {
      const t = k / K;
      const ta = A0 + (A1 - A0) * t, tb = -BB + 2 * BB * t;
      const pts = [[-BB, ta], [BB, ta], [tb, A0], [tb, A1]];
      for (const [b, a] of pts) {
        const fu = b * p.ct + a * st + side * s.eye.spacing;
        const fv = -b * st + a * p.ct + s.eye.height;
        const [x, y, z] = faceToScreen(p, fu, fv);
        x0 = Math.min(x0, x); x1 = Math.max(x1, x);
        y0 = Math.min(y0, y); y1 = Math.max(y1, y);
        zmax = Math.max(zmax, z);
      }
    }
    const m = 3;
    p.eyeOn[e] = zmax > 0;
    p.eyeBox[e] = [s.ballX + x0 * s.ballR - m, s.ballY + y0 * s.ballR - m, s.ballX + x1 * s.ballR + m, s.ballY + y1 * s.ballR + m];
  }
  p.bgA565 = pack565(s.bgA.r, s.bgA.g, s.bgA.b);
  p.fgA565 = pack565(s.fgA.r, s.fgA.g, s.fgA.b);
  p.bgB565 = pack565(s.bgB.r, s.bgB.g, s.bgB.b);
  p.fgB565 = pack565(s.fgB.r, s.fgB.g, s.fgB.b);
  return p;
}

function pillDist(s, p, fu, fv, side) {
  const x = fu - side * s.eye.spacing;
  const y = fv - s.eye.height;
  const st = p.st * side;
  const b = x * p.ct - y * st;
  const a = x * st + y * p.ct;
  const aa = Math.abs(a);
  let ex = aa - p.hl; ex = ex > 0 ? ex : 0;
  let d = Math.sqrt(b * b + ex * ex) - p.B;
  if (p.heartOn) {
    const k = p.heartK;
    const dh = sdHeart(b / k, 0.52 - a / k) * k;
    d += (dh - d) * p.heart;
  }
  const outer = b * side;
  const top = (p.topC + p.topK * outer - a) * p.topNorm;
  if (top > d) d = top;
  if (p.happyOn) {
    const cut = a - p.cutA;
    if (cut > d) d = cut;
    const da = a - p.holeA;
    const hole = p.holeR - Math.sqrt(b * b + da * da);
    if (hole > d) d = hole;
  }
  const blink = Math.abs(a - p.aMid) - p.openHalf;
  return d > blink ? d : blink;
}

function eyeHoleCoverage(s, p, dx, dy, mask) {
  const nx = dx * p.invR, ny = dy * p.invR;
  const nz2 = 1 - nx * nx - ny * ny;
  const nz = nz2 > 0 ? Math.sqrt(nz2) : 0;
  const lx = p.m00 * nx + p.m02 * nz;
  const ly = p.m10 * nx + p.m11 * ny + p.m12 * nz;
  const lz = p.m20 * nx + p.m21 * ny + p.m22 * nz;
  if (lz <= 0.02) return 0;
  const fu = lx * p.cr + ly * p.sr;
  const fv = -lx * p.sr + ly * p.cr;
  let d = 1e9;
  if (mask & 1) d = pillDist(s, p, fu, fv, -1);
  if (mask & 2) d = Math.min(d, pillDist(s, p, fu, fv, 1));
  let k = nz / lz; k = k < 0.15 ? 0.15 : k > 1 ? 1 : k;
  return clamp01(0.5 - d * s.ballR * k);
}

// RGB565 of logical pixel (x, y), exactly like renderRect on the board
export function shade(s, p, x, y) {
  const px = x + 0.5, py = y + 0.5;
  let t = 0;
  if (s.wipeR > 0) {
    const wIn = s.wipeR > 1 ? (s.wipeR - 1) * (s.wipeR - 1) : 0;
    const wOut = (s.wipeR + 1) * (s.wipeR + 1);
    const wx = px - s.wipeX, wy = py - s.wipeY, w2 = wx * wx + wy * wy;
    if (w2 <= wIn) t = 1;
    else if (w2 < wOut) t = clamp01(s.wipeR - Math.sqrt(w2) + 0.5);
  }
  const dx = px - s.ballX, dy = py - s.ballY, d2 = dx * dx + dy * dy;
  let alpha;
  if (d2 >= p.r2out) alpha = 0;
  else {
    alpha = d2 <= p.r2in ? 1 : clamp01(s.ballR - Math.sqrt(d2) + 0.5);
    let mask = 0;
    const b0 = p.eyeBox[0], b1 = p.eyeBox[1];
    if (p.eyeOn[0] && px >= b0[0] && px <= b0[2] && py >= b0[1] && py <= b0[3]) mask |= 1;
    if (p.eyeOn[1] && px >= b1[0] && px <= b1[2] && py >= b1[1] && py <= b1[3]) mask |= 2;
    if (mask) alpha *= 1 - eyeHoleCoverage(s, p, dx, dy, mask);
  }
  if (t === 0) {
    if (alpha === 0) return p.bgA565;
    if (alpha === 1) return p.fgA565;
  } else if (t === 1) {
    if (alpha === 0) return p.bgB565;
    if (alpha === 1) return p.fgB565;
  }
  const bgr = s.bgA.r + (s.bgB.r - s.bgA.r) * t, bgg = s.bgA.g + (s.bgB.g - s.bgA.g) * t, bgb = s.bgA.b + (s.bgB.b - s.bgA.b) * t;
  const fgr = s.fgA.r + (s.fgB.r - s.fgA.r) * t, fgg = s.fgA.g + (s.fgB.g - s.fgA.g) * t, fgb = s.fgA.b + (s.fgB.b - s.fgA.b) * t;
  return pack565(bgr + (fgr - bgr) * alpha, bgg + (fgg - bgg) * alpha, bgb + (fgb - bgb) * alpha);
}

// ===================================================================== animation (eyes_anim.h)

class Spring {
  constructor() { this.x = 0; this.v = 0; }
  step(target, omega, dt) {
    const d = this.x - target;
    const tmp = (this.v + omega * d) * dt;
    const e = Math.exp(-omega * dt);
    this.x = target + (d + tmp) * e;
    this.v = (this.v - omega * tmp) * e;
  }
}

class Smooth2 {
  constructor() { this.a = new Spring(); this.b = new Spring(); }
  reset(x) { this.a.x = this.b.x = x; this.a.v = this.b.v = 0; }
  step(target, omega, dt) {
    const n = dt > 0.02 ? Math.trunc(dt / 0.02) + 1 : 1;
    const h = dt / n;
    for (let i = 0; i < n; i++) { this.a.step(target, omega, h); this.b.step(this.a.x, omega, h); }
  }
  value() { return this.b.x; }
}

const BLINK_CLOSE = 90, BLINK_HOLD = 40, BLINK_OPEN = 150;
const ease = (x) => x * x * (3 - 2 * x);
const reached = (now, when) => ((now - when) | 0) >= 0;

export class Animator {
  begin(cfg, seed, now) {
    this.c = cfg;
    this.rng = (seed >>> 0) || 0x9E3779B9;
    this.br = new Spring(); this.br.x = cfg.faceRadius;
    this.yaw = new Smooth2(); this.pitch = new Smooth2(); this.roll = new Smooth2();
    this.lidTop = new Smooth2(); this.lidSlant = new Smooth2(); this.lidHappy = new Smooth2(); this.moodPitch = new Smooth2();
    this.goHome();
    this.yaw.reset(this.tYaw); this.pitch.reset(this.tPitch); this.roll.reset(this.tRoll);
    this.mood = NEUTRAL; this.linked = false; this.autoBlink = true;
    this.lookX = this.lookY = 0; this.jitX = this.jitY = 0; this.nextJitter = now;
    this.face = this.toFace = cfg.face; this.eyes = this.toEyes = cfg.eyes; this.repaint = false;
    this.wiping = false; this.wipeK = 0; this.wipeDir = 1; this.prevWipeR = 0;
    this.ringOn = false; this.ringR0 = 0; this.ringR1 = 0;
    this.nextGaze = (now + 1500) >>> 0;
    this.nextBlink = (now + 1800) >>> 0;
    this.blinkStart = 0; this.blinksLeft = 0; this.blinkEvent = false; this.blinkEventTwice = false;
    this.quietUntil = now;
    this.t0 = now; this.last = now;
    this.act = ACT.NONE; this.actStart = now; this.physT = now; this.homeNext = false;
    this.fol = [new Smooth2(), new Smooth2()]; this.goalX = 0; this.goalY = 0;
    this.dartDir = 1; this.dartY = 0; this.dartW = 0; this.dartV = 0;
    this.bx = 0; this.by = 0; this.bvx = 0; this.bvy = 0; this.kicks = 0; this.srng = 1;
    this.baseYaw = this.yaw.value(); this.basePitch = this.pitch.value(); this.baseRoll = this.roll.value();
    this.baseX = Math.cos(this.basePitch) * Math.sin(this.baseYaw); this.baseY = -Math.sin(this.basePitch);
    this.velX = 0; this.velY = 0; this.squash = 0; this.squashV = 0; this.lean = new Spring();
    this.hearts = false; this.heartK = 0; this.heartV = 0; this.heartT0 = now;
    this.s = {};
    return this;
  }

  // ---------------- control ----------------
  setLinked(on) { if (this.linked && !on) this.nextGaze = this.last; this.linked = on; }
  setMood(m) { const i = typeof m === 'string' ? MOODS.indexOf(m) : m; if (i >= 0) this.mood = i; }
  setLook(x, y) { this.lookX = x < -1 ? -1 : x > 1 ? 1 : x; this.lookY = y < -1 ? -1 : y > 1 ? 1 : y; }
  setAutoBlink(on) { this.autoBlink = on; }
  blinkNow(now, twice) { if (this.blinkStart) return false; this.startBlink(now, twice ? 2 : 1); return true; }
  mirrorBlink(now) { this.quietUntil = (now + MIRROR_QUIET_MS) >>> 0; return this.blinkNow(now, false); }
  setColors(face, eyes) {
    face &= 0xFFFFFF; eyes &= 0xFFFFFF;
    if (face === this.faceColor() && eyes === this.eyeColor()) return;
    this.face = this.toFace = face; this.eyes = this.toEyes = eyes;
    this.wiping = false; this.prevWipeR = 0;
    this.repaint = true;
  }
  swapColors() {
    if (this.wiping) { this.wipeDir = -this.wipeDir; return; }
    this.toFace = this.eyes; this.toEyes = this.face;
    this.wiping = true; this.wipeK = 0; this.wipeDir = 1; this.prevWipeR = 0;
  }
  touch(down) { if (down) this.swapColors(); }

  // ---------------- moves ----------------
  follow(x, y, now) {
    now >>>= 0;
    if (this.act !== ACT.FOLLOW) { this.beginAct(ACT.FOLLOW, now); this.fol[0].reset(this.baseX); this.fol[1].reset(this.baseY); }
    [this.goalX, this.goalY] = clampReach(x, y);
  }
  followEnd(now) { if (this.act === ACT.FOLLOW) this.settle(now >>> 0, FOLLOW_REST_MS, false); }
  dart(dir, now) {
    this.beginAct(ACT.DART, now >>> 0);
    this.dartDir = dir < 0 ? -1 : 1;
    this.dartY = this.baseY * 0.5;
    this.dartW = this.dartV = 0;
  }
  shake(seed, now) {
    now >>>= 0;
    this.beginAct(ACT.SHAKE, now);
    this.srng = (seed >>> 0) || 0x2545F491;
    this.bx = this.baseX; this.by = this.baseY; this.bvx = this.bvy = 0; this.kicks = 0;
    this.quietUntil = (now + SHAKE_MAX_MS) >>> 0;
  }
  action() { return this.act; }
  setHearts(on, now) {
    if (on && !this.hearts) this.heartT0 = now >>> 0;
    this.hearts = !!on;
  }

  // ---------------- per frame ----------------
  update(now) {
    now >>>= 0;
    const c = this.c;
    let dt = ((now - this.last) | 0) * 0.001;
    if (dt < 0) dt = 0;
    if (dt > 0.1) dt = 0.1;
    this.last = now;
    const t = ((now - this.t0) >>> 0) * 0.001;

    if (this.linked && this.autoBlink) {
      if (reached(now, this.nextJitter)) {
        this.jitX = this.frand(-0.05, 0.05);
        this.jitY = this.frand(-0.04, 0.04);
        this.nextJitter = (now + this.rand(400, 1600)) >>> 0;
      }
    } else {
      this.jitX = this.jitY = 0;
    }

    let omega;
    if (this.linked) {
      omega = 11;
      const lx = this.lookX + this.jitX, ly = this.lookY + this.jitY;
      this.tYaw = lx * c.followYaw * DEG;
      this.tPitch = -ly * c.followPitch * DEG;
      this.tRoll = -lx * 5 * DEG;
    } else {
      omega = 5.5;
      if (reached(now, this.nextGaze)) {
        if (this.homeNext) { this.goHome(); this.homeNext = false; } else this.pickIdleGaze();
        this.nextGaze = (now + this.rand(1500, 4000)) >>> 0;
      }
    }
    omega *= c.gazeSpeed;
    this.yaw.step(this.tYaw, omega, dt);
    this.pitch.step(this.tPitch, omega, dt);
    this.roll.step(this.tRoll, omega * 0.8, dt);

    const ms = MOOD_STYLES[this.mood];
    this.lidTop.step(ms[0], 11, dt);
    this.lidSlant.step(ms[1], 11, dt);
    this.lidHappy.step(ms[2], 11, dt);
    this.moodPitch.step(ms[3] * DEG, 8, dt);

    let fx = 0, fy = 0;
    if (!this.linked) {
      fx = c.drift * DEG * (Math.sin(t * 0.63 + 0.7) * 0.7 + Math.sin(t * 1.17) * 0.3);
      fy = c.drift * DEG * (Math.sin(t * 0.51) * 0.6 + Math.sin(t * 0.93 + 2.1) * 0.4);
    }
    // ---- moves: the base pose (where the eyes point, before the slow float) ----
    this.stepMs(now);
    const sYaw = this.yaw.value(), sPitch = this.pitch.value();
    let w = 0, pYaw = sYaw, pPitch = sPitch;
    const at = (now - this.actStart) >>> 0;
    if (this.act === ACT.FOLLOW) {
      this.fol[0].step(this.goalX, FOLLOW_OMEGA * c.gazeSpeed, dt);
      this.fol[1].step(this.goalY, FOLLOW_OMEGA * c.gazeSpeed, dt);
      [pYaw, pPitch] = pointToPose(this.fol[0].value(), this.fol[1].value());
      w = 1;
    } else if (this.act === ACT.DART) {
      [pYaw, pPitch] = pointToPose(this.dartDir * DART_X, this.dartY);
      w = this.dartW;
      if (at > DART_HOLD_MS + 300 && Math.abs(this.dartW) < 0.002 && Math.abs(this.dartV) < 0.05) { this.act = ACT.NONE; w = 0; }
    } else if (this.act === ACT.SHAKE) {
      [pYaw, pPitch] = pointToPose(this.bx, this.by);
      w = 1;
    }
    this.baseYaw = sYaw + (pYaw - sYaw) * w;
    this.basePitch = sPitch + (pPitch - sPitch) * w;
    const nX = Math.cos(this.basePitch) * Math.sin(this.baseYaw), nY = -Math.sin(this.basePitch);
    if (dt > 0) { this.velX = (nX - this.baseX) / dt; this.velY = (nY - this.baseY) / dt; }
    this.baseX = nX; this.baseY = nY;
    this.lean.step(clampf(this.velX * 0.025, -0.14, 0.14), 16, dt);
    this.baseRoll = this.roll.value() + this.lean.x;
    if (this.act === ACT.SHAKE && this.shakeDone(at)) {
      this.settle(now, 700, true);
      if (!this.blinkStart) this.startBlink(now, 2);
      this.quietUntil = (now + 2500) >>> 0;
    }

    this.br.step(c.faceRadius, 9, dt);
    if (this.br.x < c.faceRadius) { this.br.x = c.faceRadius; if (this.br.v < 0) this.br.v = 0; }

    if (this.autoBlink && this.blinkStart === 0 && reached(now, this.nextBlink) && reached(now, this.quietUntil)) {
      const twice = this.next() % 100 < 20;
      this.startBlink(now, twice ? 2 : 1);
      this.blinkEvent = true; this.blinkEventTwice = twice;
    }
    let open = 1;
    if (this.blinkStart && reached(now, this.blinkStart)) {
      const bt = (now - this.blinkStart) >>> 0;
      if (bt < BLINK_CLOSE) open = 1 - ease(bt / BLINK_CLOSE);
      else if (bt < BLINK_CLOSE + BLINK_HOLD) open = 0;
      else if (bt < BLINK_CLOSE + BLINK_HOLD + BLINK_OPEN) open = ease((bt - BLINK_CLOSE - BLINK_HOLD) / BLINK_OPEN);
      else if (--this.blinksLeft > 0) this.blinkStart = (now + 110) >>> 0;
      else { this.blinkStart = 0; this.nextBlink = (now + this.rand(2500, 6000)) >>> 0; }
    }

    const endR = c.screenSize * 0.5 + 3;
    let wipeR = 0;
    this.ringOn = false;
    if (this.wiping) {
      this.wipeK += this.wipeDir * dt * 1000 / c.wipeMs;
      if (this.wipeK >= 1) {
        this.wiping = false; this.face = this.toFace; this.eyes = this.toEyes;
        this.ringOn = true; this.ringR0 = this.prevWipeR - 2; this.ringR1 = 1e6;
      } else if (this.wipeK <= 0 && this.wipeDir < 0) {
        this.wiping = false;
        this.ringOn = true; this.ringR0 = 0; this.ringR1 = this.prevWipeR + 2;
      } else {
        wipeR = endR * (0.5 - 0.5 * Math.cos(this.wipeK * Math.PI));
        this.ringOn = true;
        this.ringR0 = Math.min(this.prevWipeR, wipeR) - 2;
        this.ringR1 = Math.max(this.prevWipeR, wipeR) + 2;
        this.prevWipeR = wipeR;
      }
      if (!this.wiping) this.prevWipeR = 0;
    }

    const half = c.screenSize * 0.5, s = this.s;
    s.ballX = half; s.ballY = half; s.ballR = this.br.x;
    s.yaw = this.baseYaw + fx;
    s.pitch = this.basePitch + fy + this.moodPitch.value();
    s.roll = this.baseRoll;
    s.blink = open;
    s.lidTop = this.lidTop.value();
    s.lidSlant = this.lidSlant.value();
    s.lidHappy = this.lidHappy.value();
    s.fgA = hexColor(this.face); s.bgA = hexColor(this.eyes);
    s.fgB = hexColor(this.wiping ? this.toFace : this.face);
    s.bgB = hexColor(this.wiping ? this.toEyes : this.eyes);
    s.wipeX = half; s.wipeY = half;
    s.wipeR = this.wiping ? Math.max(wipeR, 0.001) : 0;
    const morph = this.heartK < 0 ? 0 : this.heartK > 1 ? 1 : this.heartK;
    const pop = this.heartK > 1 ? this.heartK - 1 : 0;
    let beat = 0;
    if (this.hearts) {
      const bt = ((now - this.heartT0) >>> 0) % HEARTBEAT_MS;
      beat = HEARTBEAT_LUB * bump(bt, 0, 140) + HEARTBEAT_DUB * bump(bt, 200, 140);
    }
    s.heart = morph;
    s.heartSize = 1 + pop * 1.2 + beat * morph;
    const radius = c.eye.radius * (1 + this.squash);
    s.eye = { ...c.eye, radius, halfLen: Math.max((c.eye.halfLen + c.eye.radius) * (1 - 0.7 * this.squash) - radius, 0) };
  }

  // ---------------- output ----------------
  scene() { return this.s; }
  faceColor() { return this.wiping && this.wipeDir > 0 ? this.toFace : this.face; }
  eyeColor() { return this.wiping && this.wipeDir > 0 ? this.toEyes : this.eyes; }
  takeRepaint() { const r = this.repaint; this.repaint = false; return r; }
  takeBlinkEvent() {
    if (!this.blinkEvent) return null;
    this.blinkEvent = false;
    return { twice: this.blinkEventTwice };
  }

  next() {
    let r = this.rng;
    r ^= r << 13; r >>>= 0;
    r ^= r >>> 17; r >>>= 0;
    r ^= r << 5; r >>>= 0;
    return (this.rng = r);
  }
  rand(lo, hi) { return lo + this.next() % (hi - lo + 1); }
  frand(lo, hi) { return lo + (hi - lo) * (this.next() % 10001) / 10000; }
  goHome() { const c = this.c; this.tYaw = c.homeYaw * DEG; this.tPitch = c.homePitch * DEG; this.tRoll = c.homeRoll * DEG; }
  pickIdleGaze() {
    const c = this.c, r = this.next() % 100;
    if (r < 55) {
      this.tYaw = (c.homeYaw + this.frand(-c.wander, c.wander)) * DEG;
      this.tPitch = (c.homePitch + this.frand(-c.wander * 0.7, c.wander * 0.7)) * DEG;
      this.tRoll = (c.homeRoll + this.frand(-3, 3)) * DEG;
    } else if (r < 75) {
      this.goHome();
    } else if (r < 88) {
      this.tYaw = this.frand(-6, 6) * DEG; this.tPitch = this.frand(-4, 6) * DEG; this.tRoll = this.frand(-3, 3) * DEG;
    } else {
      this.tYaw = this.frand(-30, -15) * DEG; this.tPitch = this.frand(5, 22) * DEG; this.tRoll = this.frand(2, 8) * DEG;
    }
  }
  startBlink(now, count) { this.blinkStart = now ? now >>> 0 : 1; this.blinksLeft = count; }

  // ---- moves ----
  snext() {
    let r = this.srng;
    r ^= r << 13; r >>>= 0;
    r ^= r >>> 17; r >>>= 0;
    r ^= r << 5; r >>>= 0;
    return (this.srng = r);
  }
  beginAct(kind, now) {
    if (this.act !== ACT.NONE) { this.yaw.reset(this.baseYaw); this.pitch.reset(this.basePitch); }
    this.act = kind; this.actStart = now;
  }
  settle(now, restMs, home) {
    this.yaw.reset(this.baseYaw); this.pitch.reset(this.basePitch);
    this.tYaw = this.baseYaw; this.tPitch = this.basePitch;
    this.act = ACT.NONE; this.nextGaze = (now + restMs) >>> 0; this.homeNext = home;
  }
  stepMs(now) {
    let n = (now - this.physT) | 0;
    if (n <= 0) return;
    if (n > 100) { this.physT = (now - 100) >>> 0; n = 100; }
    const sp = Math.sqrt(this.velX * this.velX + this.velY * this.velY);
    let tgt = 0;
    if (sp > 0.5) tgt = (Math.abs(this.velX) - Math.abs(this.velY)) / sp * Math.min(sp / 8, 1) * 0.16;
    for (let i = 0; i < n; i++) {
      this.physT = (this.physT + 1) >>> 0;
      const at = (this.physT - this.actStart) | 0;
      if (this.act === ACT.SHAKE && at >= 0) this.shakeStep(at);
      {
        const om = this.hearts ? 16 : 14, z = this.hearts ? 0.42 : 1;
        const a = -om * om * (this.heartK - (this.hearts ? 1 : 0)) - 2 * z * om * this.heartV;
        this.heartV += a * 0.001;
        this.heartK += this.heartV * 0.001;
      }
      if (this.act === ACT.DART && at >= 0) {
        const out = at < DART_HOLD_MS;
        const om = out ? 24 : 13, z = out ? 0.6 : 1;
        const a = -om * om * (this.dartW - (out ? 1 : 0)) - 2 * z * om * this.dartV;
        this.dartV += a * 0.001;
        this.dartW += this.dartV * 0.001;
      }
      const a = -900 * (this.squash - tgt) - 16 * this.squashV;
      this.squashV += a * 0.001;
      this.squash = clampf(this.squash + this.squashV * 0.001, -0.35, 0.45);
    }
  }
  shakeStep(t) {
    if (this.kicks < SHAKE_KICK_MS.length && t >= SHAKE_KICK_MS[this.kicks]) {
      const ang = (this.snext() % 3600) * (6.2831853 / 3600);
      this.bvx = this.bvx * 0.4 + Math.cos(ang) * SHAKE_KICK_SPEED[this.kicks];
      this.bvy = this.bvy * 0.4 + Math.sin(ang) * SHAKE_KICK_SPEED[this.kicks];
      this.kicks++;
    }
    const drag = t < 1500 ? 0.9 : Math.min(0.9 + (t - 1500) * (5 / 700), 6);
    const k = 1 - drag * 0.001;
    this.bvx *= k; this.bvy *= k;
    this.bx += this.bvx * 0.001; this.by += this.bvy * 0.001;
    const qx = this.bx / REACH_X, qy = this.by / REACH_Y, q = qx * qx + qy * qy;
    if (q > 1) {
      const r = 1 / Math.sqrt(q);
      this.bx *= r; this.by *= r;
      let nx = this.bx / (REACH_X * REACH_X), ny = this.by / (REACH_Y * REACH_Y);
      const nl = Math.sqrt(nx * nx + ny * ny);
      nx /= nl; ny /= nl;
      const vn = this.bvx * nx + this.bvy * ny;
      if (vn > 0) {
        this.bvx -= (1 + SHAKE_BOUNCE) * vn * nx;
        this.bvy -= (1 + SHAKE_BOUNCE) * vn * ny;
        this.squashV -= (nx * nx - ny * ny) * vn * 2.5;
      }
    }
  }
  shakeDone(at) {
    return at > SHAKE_MAX_MS || (this.kicks >= SHAKE_KICK_MS.length && at > 1200 && this.bvx * this.bvx + this.bvy * this.bvy < 0.0036);
  }
}

// ===================================================================== what app.js uses

// The preview engine: an Animator with the board's settings. Calls made before the first
// update() are queued and applied right before it (the animator starts on the first frame).
export class Engine {
  constructor(seed = 1, cfg = CONFIG) {
    this.anim = new Animator();
    this.seed = seed; this.cfg = { ...cfg };
    this.started = false;
    this.p = null;
    this._queue = [];
  }
  _start(now) {
    if (this.started) return;
    this.started = true;
    this.anim.begin(this.cfg, this.seed, now >>> 0);
    for (const [fn, args] of this._queue) this.anim[fn](...args);
    this._queue = [];
  }
  _call(fn, ...args) { if (this.started) this.anim[fn](...args); else this._queue.push([fn, args]); }
  setMood(m) { this._call('setMood', m); }
  setLook(x, y) { this._call('setLook', x, y); }
  setLinked(on) { this._call('setLinked', on); }
  setAutoBlink(on) { this._call('setAutoBlink', on); }
  setColors(face, eyes) { if (this.started) this.anim.setColors(face, eyes); else { this.cfg.face = face & 0xFFFFFF; this.cfg.eyes = eyes & 0xFFFFFF; } }
  blinkNow(now, twice) { this._start(now); return this.anim.blinkNow(now >>> 0, twice); }
  mirrorBlink(now) { this._start(now); return this.anim.mirrorBlink(now >>> 0); }
  touch(down, now) { this._start(now); this.anim.touch(down); }
  follow(x, y, now) { this._start(now); this.anim.follow(x, y, now >>> 0); }
  followEnd(now) { this._start(now); this.anim.followEnd(now >>> 0); }
  dart(dir, now) { this._start(now); this.anim.dart(dir, now >>> 0); }
  shake(seed, now) { this._start(now); this.anim.shake(seed, now >>> 0); }
  get action() { return this.started ? this.anim.act : ACT.NONE; }
  setHearts(on, now) { this._start(now); this.anim.setHearts(on, now >>> 0); }
  get heartsOn() { return this.started && this.anim.hearts; }
  get faceColor() { return this.started ? this.anim.faceColor() : this.cfg.face; }
  get eyeColor() { return this.started ? this.anim.eyeColor() : this.cfg.eyes; }
  get moodName() { return MOODS[this.started ? this.anim.mood : 0]; }
  update(now) {
    this._start(now);
    this.anim.update(now >>> 0);
    this.p = prepare(this.anim.scene());
  }
  scene() { return this.anim.scene(); }
  shade(x, y) { return shade(this.anim.scene(), this.p, x, y); }
}

// Draws the engine into a 466x466 canvas. Normally only the boxes around the eyes change;
// during a color swap, after new colors, or on the first frame the whole screen is redrawn.
export class ScreenPreview {
  constructor(canvas) {
    canvas.width = canvas.height = SCREEN;
    this.ctx = canvas.getContext('2d');
    this.img = this.ctx.createImageData(SCREEN, SCREEN);
    this.px = new Uint32Array(this.img.data.buffer);
    this.prev = null;
    this.lut = new Uint32Array(65536);
    for (let c = 0; c < 65536; c++) {
      const r = (((c >> 11) & 31) * 255 / 31) | 0, g = (((c >> 5) & 63) * 255 / 63) | 0, b = ((c & 31) * 255 / 31) | 0;
      this.lut[c] = 0xFF000000 | (b << 16) | (g << 8) | r;
    }
  }
  draw(engine) {
    if (!engine.p) return;
    const p = engine.p, a = engine.anim;
    const boxes = [0, 1].filter((e) => p.eyeOn[e]).map((e) => p.eyeBox[e].map((v, i) => (i < 2 ? Math.floor(v) : Math.ceil(v))));
    const all = a.takeRepaint() || !this.prev || a.ringOn || a.wiping;
    const rects = all ? [[0, 0, SCREEN - 1, SCREEN - 1]] : this.prev.concat(boxes);
    for (const [x0, y0, x1, y1] of rects) {
      const ax = Math.max(0, x0), ay = Math.max(0, y0), bx = Math.min(SCREEN - 1, x1), by = Math.min(SCREEN - 1, y1);
      for (let y = ay; y <= by; y++) {
        let i = y * SCREEN + ax;
        for (let x = ax; x <= bx; x++) this.px[i++] = this.lut[engine.shade(x, y)];
      }
    }
    this.prev = boxes;
    this.ctx.putImageData(this.img, 0, 0);
  }
}
