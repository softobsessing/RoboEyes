// node test/touch.test.mjs — the touch rule, on the same eye code the board runs (via roboeyes.js):
// a touch swaps the face and eye colors and smiles while the finger is down plus 2.5 s after it
// lifts; then the usual pill eyes come back and the swapped colors stay.
import assert from 'node:assert';
import { Animator, CONFIG } from '../roboeyes.js';

let n = 0;
const t = (name, fn) => { fn(); n++; console.log(`ok  ${name}`); };
const run = (a, from, to) => { for (let ms = from; ms <= to; ms += 16) a.update(ms); };
const WHITE = 0xFFFFFF, BLACK = 0x000000;

t('touch: colors swap (wipe), smile while held + 2.5 s, then pills with the swapped colors', () => {
  const a = new Animator().begin(CONFIG, 1, 0);
  run(a, 0, 1000);
  assert.strictEqual(a.faceColor(), WHITE); assert.strictEqual(a.eyeColor(), BLACK);
  a.touch(true, 1000);
  assert.strictEqual(a.faceColor(), BLACK, 'heading to the swapped colors at once');
  assert.ok(a.smiling(1000));
  run(a, 1000, 2000);                      // finger held for 1 s; the 650 ms wipe is done
  assert.strictEqual(a.wiping, false);
  assert.strictEqual(a.face, BLACK); assert.strictEqual(a.eyes, WHITE);
  assert.ok(a.scene().lidHappy > 0.95, 'eyes are smiling arches');
  a.touch(false, 2000);
  run(a, 2000, 4480);
  assert.ok(a.smiling(4480), 'still smiling 2.48 s after lifting');
  run(a, 4480, 4520);
  assert.ok(!a.smiling(4520), 'smile over 2.5 s after lifting');
  run(a, 4520, 5600);
  assert.ok(a.scene().lidHappy < 0.02, 'back to the usual pills');
  assert.strictEqual(a.faceColor(), BLACK); assert.strictEqual(a.eyeColor(), WHITE);
});

t('touching again swaps back; touching mid-wipe runs the wipe backwards', () => {
  const a = new Animator().begin(CONFIG, 2, 0);
  a.touch(true, 0); a.touch(false, 50); run(a, 0, 1000);
  assert.strictEqual(a.faceColor(), BLACK);
  a.touch(true, 1000); a.touch(false, 1050); run(a, 1000, 1200);
  assert.ok(a.wiping);
  a.touch(true, 1200); a.touch(false, 1250);   // changed its mind: back to black
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
  a.touch(true, 200);                        // swap starts...
  run(a, 200, 400);
  a.setColors(0xFF5FA2, 0x14213D);           // ...the app echoes the colors it's swapping to
  assert.ok(a.wiping, 'echo does not cut the wipe short');
  assert.ok(!a.takeRepaint());
});

t('a touch while the app drives the eyes (E lines) still smiles', () => {
  const a = new Animator().begin(CONFIG, 4, 0);
  a.setLinked(true); a.setMood('side');
  run(a, 0, 600);
  assert.ok(a.scene().lidTop > 0.4);
  a.touch(true, 600); a.touch(false, 600);
  run(a, 600, 1400);
  assert.ok(a.scene().lidHappy > 0.95 && a.scene().lidTop < 0.05);
  run(a, 1400, 3800);
  assert.ok(a.scene().lidTop > 0.4, 'back to the mood it was given');
});
console.log(`\n${n} passed`);
