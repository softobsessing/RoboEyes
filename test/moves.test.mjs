// node test/moves.test.mjs — follow the finger, dart left / right, shake: on the same eye code the
// board runs (via roboeyes.js).
import assert from 'node:assert';
import { Animator, CONFIG, ACT } from '../roboeyes.js';

let n = 0;
const t = (name, fn) => { fn(); n++; console.log(`ok  ${name}`); };
const fresh = (seed = 1) => { const a = new Animator().begin(CONFIG, seed, 0); for (let ms = 0; ms <= 1000; ms += 16) a.update(ms); return a; };
// Run frames; `each(a, ms)` before every update. Returns the biggest frame-to-frame jump of the eyes'
// point on screen (ball radii), to check that nothing ever teleports.
function run(a, from, to, step = 16, each = () => {}) {
  let maxJump = 0, px = a.baseX, py = a.baseY;
  for (let ms = from; ms <= to; ms += step) {
    each(a, ms);
    a.update(ms);
    maxJump = Math.max(maxJump, Math.hypot(a.baseX - px, a.baseY - py));
    px = a.baseX; py = a.baseY;
  }
  return maxJump;
}
const inReach = (x, y) => (x / 0.76) ** 2 + (y / 0.84) ** 2 <= 1 + 1e-6;

t('follow: the eyes glide to the finger and track it as it drags', () => {
  const a = fresh();
  a.follow(-0.5, 0.4, 1016);
  const jump = run(a, 1016, 1400);
  assert.ok(Math.hypot(a.baseX + 0.5, a.baseY - 0.4) < 0.02, `at the finger (${a.baseX.toFixed(3)}, ${a.baseY.toFixed(3)})`);
  assert.ok(jump < 0.2, `glides, no jump (${jump.toFixed(3)})`);
  run(a, 1416, 2000, 16, (a, ms) => a.follow(-0.5 + (ms - 1416) / 1000, 0.4, ms));   // drag right
  assert.ok(Math.abs(a.baseX - (-0.5 + 0.584)) < 0.13, 'keeps up with a drag (about 1/8 s behind)');
});

t('follow: a finger past the edge keeps the eyes on the glass', () => {
  const a = fresh();
  a.follow(1, 1, 1016);
  run(a, 1016, 1600);
  assert.ok(inReach(a.baseX, a.baseY) && Math.hypot(a.baseX, a.baseY) > 0.7);
});

t('follow: after the finger lifts the eyes stay put, then carry on', () => {
  const a = fresh();
  a.follow(-0.4, 0.3, 1016); run(a, 1016, 1500);
  a.followEnd(1500);
  assert.strictEqual(a.act, ACT.NONE);
  const x = a.baseX, y = a.baseY;
  run(a, 1516, 2300);
  assert.ok(Math.hypot(a.baseX - x, a.baseY - y) < 0.03, 'still there 0.8 s later');
  run(a, 2316, 6000);
  assert.ok(Math.hypot(a.baseX - x, a.baseY - y) > 0.05, 'wandering again');
});

for (const [dir, name] of [[-1, 'left'], [1, 'right']]) {
  t(`dart ${name}: snaps over fast, holds, comes back`, () => {
    const a = fresh();
    const x0 = a.baseX;
    a.dart(dir, 1016);
    const jump = run(a, 1016, 1176);
    assert.ok(dir * a.baseX > 0.6, `over at the ${name} in 160 ms (${a.baseX.toFixed(2)})`);
    assert.ok(jump < 0.3, 'fast, but no teleport');
    run(a, 1192, 1800);
    assert.ok(dir * a.baseX > 0.6, 'holds there');
    run(a, 1816, 2900);
    assert.strictEqual(a.act, ACT.NONE);
    assert.ok(Math.abs(a.baseX - x0) < 0.2, 'back where it was looking');
  });
}

t('shake: bounces all around, stays on the glass, squashes on impact, settles, blinks', () => {
  const a = fresh();
  a.shake(1234567, 1016);
  let minX = 9, maxX = -9, minY = 9, maxY = -9, minSq = 0, maxSq = 0, blinked = false, end = 0;
  const jump = run(a, 1016, 6000, 16, (a, ms) => {
    if (a.act === ACT.SHAKE) {
      assert.ok(inReach(a.bx, a.by), 'inside the screen edge');
      minX = Math.min(minX, a.bx); maxX = Math.max(maxX, a.bx); minY = Math.min(minY, a.by); maxY = Math.max(maxY, a.by);
      minSq = Math.min(minSq, a.squash); maxSq = Math.max(maxSq, a.squash);
    } else if (!end) end = ms;
    if (end && a.blinkStart) blinked = true;
  });
  assert.ok(maxX - minX > 1.0 && maxY - minY > 1.0, `covers the screen (x ${minX.toFixed(2)}..${maxX.toFixed(2)}, y ${minY.toFixed(2)}..${maxY.toFixed(2)})`);
  assert.ok(maxSq > 0.12 && minSq < -0.12, `squashes and stretches (${minSq.toFixed(2)}..${maxSq.toFixed(2)})`);
  assert.ok(end > 2000 && end < 1016 + 3800 + 32, `comes to rest (${end - 1016} ms)`);
  assert.ok(blinked, 'a dazed blink after');
  assert.ok(jump < 0.2, `smooth the whole way (${jump.toFixed(3)} per frame)`);
});

t('shake: same seed = same bounces (board and preview agree), at any frame rate', () => {
  const a = fresh(), b = fresh();
  a.shake(42, 1016); b.shake(42, 1016);
  run(a, 1016, 2216, 16); run(b, 1016, 2216, 4);   // 60 fps vs 250 fps
  assert.ok(Math.abs(a.bx - b.bx) < 1e-9 && Math.abs(a.by - b.by) < 1e-9, 'physics independent of frame rate');
  const c = fresh(); c.shake(43, 1016); run(c, 1016, 2216);
  assert.ok(Math.hypot(a.bx - c.bx, a.by - c.by) > 0.01, 'another seed bounces differently');
});

t('moves interrupting moves never jump', () => {
  const a = fresh();
  const jump = run(a, 1016, 7000, 16, (a, ms) => {
    if (ms === 1200) a.dart(-1, ms);
    if (ms === 1296) a.shake(7, ms);           // mid-dart
    if (ms === 1808) a.dart(1, ms);            // mid-shake
    if (ms >= 2000 && ms < 2600) a.follow(0.3, 0.5, ms);   // mid-dart
    if (ms === 2600) a.followEnd(ms);
    if (ms === 2700) a.shake(8, ms);
  });
  assert.ok(jump < 0.2, `no teleports (${jump.toFixed(3)} per frame)`);
});
t('heart eyes: pop in, beat, still blink, melt back to pills', () => {
  let maxSize = 0, minSizeLater = 9, maxSizeLater = 0, closed = false;
  const b = fresh();
  b.setHearts(true, 1016);
  for (let ms = 1016; ms <= 4000; ms += 16) {
    if (ms === 2504) b.blinkNow(ms, false);
    b.update(ms);
    const s = b.scene();
    if (ms < 1400) maxSize = Math.max(maxSize, s.heartSize);
    if (ms > 1800) { minSizeLater = Math.min(minSizeLater, s.heartSize); maxSizeLater = Math.max(maxSizeLater, s.heartSize); }
    if (ms > 2504 && ms < 2700 && s.blink < 0.05) closed = true;
  }
  assert.ok(b.scene().heart > 0.99, 'all heart');
  assert.ok(maxSize > 1.08, `pops in a bit big (${maxSize.toFixed(2)})`);
  assert.ok(minSizeLater < 1.01 && maxSizeLater > 1.07, `beats (${minSizeLater.toFixed(2)}..${maxSizeLater.toFixed(2)})`);
  assert.ok(closed, 'hearts still blink');
  b.setHearts(false, 4016);
  for (let ms = 4016; ms <= 5000; ms += 16) b.update(ms);
  assert.ok(b.scene().heart < 0.01 && Math.abs(b.scene().heartSize - 1) < 0.01, 'back to pills');
});
console.log(`\n${n} passed`);
