// node test/touch.test.mjs — the touch rule, on the same eye code the board runs (via roboeyes.js):
// a touch swaps the face and eye colors (circular wipe) and nothing else: the eyes keep the same
// shape and keep doing what they were doing. The swapped colors stay.
import assert from 'node:assert';
import { Animator, CONFIG } from '../roboeyes.js';

let n = 0;
const t = (name, fn) => { fn(); n++; console.log(`ok  ${name}`); };
const run = (a, from, to) => { for (let ms = from; ms <= to; ms += 16) a.update(ms); };
const WHITE = 0xFFFFFF, BLACK = 0x000000;
const pose = (s) => ({ lidTop: s.lidTop, lidSlant: s.lidSlant, lidHappy: s.lidHappy });

t('touch: colors swap (wipe) and stay swapped; the eyes stay the same pills', () => {
  const a = new Animator().begin(CONFIG, 1, 0);
  run(a, 0, 1000);
  assert.strictEqual(a.faceColor(), WHITE); assert.strictEqual(a.eyeColor(), BLACK);
  a.touch(true);
  assert.strictEqual(a.faceColor(), BLACK, 'heading to the swapped colors at once');
  for (let ms = 1000; ms <= 5000; ms += 16) {
    a.update(ms);
    assert.ok(a.scene().lidHappy < 0.001, `no smile at ${ms} ms`);
    if (ms === 1504) a.touch(false);
  }
  assert.strictEqual(a.wiping, false);
  assert.strictEqual(a.face, BLACK); assert.strictEqual(a.eyes, WHITE);
});

t('a touch does not change where the eyes look or their shape', () => {
  const a = new Animator().begin(CONFIG, 9, 0);
  const b = new Animator().begin(CONFIG, 9, 0);
  for (let ms = 0; ms <= 4000; ms += 16) {
    if (ms === 1000) { a.touch(true); }
    if (ms === 1200) { a.touch(false); }
    a.update(ms); b.update(ms);
    const sa = a.scene(), sb = b.scene();
    assert.deepStrictEqual(pose(sa), pose(sb));
    assert.strictEqual(sa.yaw, sb.yaw); assert.strictEqual(sa.pitch, sb.pitch); assert.strictEqual(sa.roll, sb.roll); assert.strictEqual(sa.ballR, sb.ballR); assert.strictEqual(sa.blink, sb.blink);
  }
});

t('touching again swaps back; touching mid-wipe runs the wipe backwards', () => {
  const a = new Animator().begin(CONFIG, 2, 0);
  a.touch(true); a.touch(false); run(a, 0, 1000);
  assert.strictEqual(a.faceColor(), BLACK);
  a.touch(true); a.touch(false); run(a, 1000, 1200);
  assert.ok(a.wiping);
  a.touch(true); a.touch(false);   // changed its mind: back to black
  assert.strictEqual(a.faceColor(), BLACK);
  run(a, 1200, 2500);
  assert.strictEqual(a.wiping, false);
  assert.strictEqual(a.face, BLACK); assert.strictEqual(a.eyes, WHITE);
});

t('colors from the app apply at once; the same colors again change nothing', () => {
  const a = new Animator().begin(CONFIG, 3, 0);
  run(a, 0, 100);
  a.setColors(0x14213D, 0xFF5FA2);
  assert.ok(a.takeRepaint(), 'whole screen redrawn');
  assert.strictEqual(a.face, 0x14213D);
  a.touch(true);                             // swap starts...
  run(a, 200, 400);
  a.setColors(0xFF5FA2, 0x14213D);           // ...the app echoes the colors it's swapping to
  assert.ok(a.wiping, 'echo does not cut the wipe short');
  assert.ok(!a.takeRepaint());
});

t('a touch while the app drives the eyes (E lines) keeps the mood it was given', () => {
  const a = new Animator().begin(CONFIG, 4, 0);
  a.setLinked(true); a.setMood('side');
  run(a, 0, 600);
  assert.ok(a.scene().lidTop > 0.4);
  a.touch(true); a.touch(false);
  run(a, 600, 1400);
  assert.ok(a.scene().lidTop > 0.4 && a.scene().lidHappy < 0.001);
  assert.strictEqual(a.faceColor(), BLACK);
});
console.log(`\n${n} passed`);
