// node test/blink.test.mjs — "blink with me": the camera blink detector (blink.js) and the board's
// mirrored blink (same eye code as the firmware, via roboeyes.js).
import assert from 'node:assert';
import { BlinkDetector, DEFAULT_SENSITIVITY, sensitivityLabel } from '../blink.js';
import { Animator, CONFIG } from '../roboeyes.js';

let n = 0;
const t = (name, fn) => { fn(); n++; console.log(`ok  ${name}`); };

// A camera at 30 fps. `shape(ms)` gives the eye-closed score; returns the times blinks were seen.
function feed(det, shape, from, to, fps = 30, face = () => true) {
  const seen = [];
  let seed = 7;
  const noise = () => { seed = (seed * 1103515245 + 12345) >>> 0; return ((seed >>> 8) / 2 ** 24 - 0.5) * 0.04; };   // ±0.02
  for (let ms = from; ms <= to; ms += 1000 / fps) {
    if (det.update(face(ms), Math.max(0, Math.min(1, shape(ms) + noise())), ms)) seen.push(Math.round(ms));
  }
  return seen;
}
// A blink: quick close, short hold, open. `depth` = how closed (score) at its deepest.
const blinkAt = (at, depth, open = 0.1, ms = 150) => (x) => {
  const d = x - at;
  if (d < 0 || d > ms) return open;
  return open + (depth - open) * Math.sin((d / ms) * Math.PI);
};
const sum = (...fs) => (x) => fs.reduce((v, f) => Math.max(v, f(x)), 0);

t('default is extra sensitive', () => {
  assert.strictEqual(sensitivityLabel(DEFAULT_SENSITIVITY), 'extra sensitive');
  assert.ok(new BlinkDetector().margin < 0.13);
});

t('open eyes with camera noise: no blinks', () => {
  assert.deepStrictEqual(feed(new BlinkDetector(), () => 0.1, 0, 20000), []);
});

t('a full blink counts once', () => {
  const seen = feed(new BlinkDetector(), blinkAt(3000, 0.85), 0, 5000);
  assert.strictEqual(seen.length, 1);
  assert.ok(seen[0] >= 3000 && seen[0] < 3080, `seen fast (at ${seen[0]} ms)`);
});

t('extra sensitive catches a small, quick blink that "relaxed" ignores', () => {
  const small = blinkAt(3000, 0.32, 0.1, 110);
  assert.strictEqual(feed(new BlinkDetector(), small, 0, 5000).length, 1);
  assert.strictEqual(feed(new BlinkDetector(0.1), small, 0, 5000).length, 0);
});

t('blinks in quick succession all count', () => {
  const f = sum(blinkAt(2000, 0.8), blinkAt(2350, 0.8), blinkAt(2700, 0.8));
  assert.strictEqual(feed(new BlinkDetector(), f, 0, 4000).length, 3);
});

t('slowly looking down / squinting is not a blink', () => {
  // score creeps from 0.1 to 0.45 over 4 s (looking down at a phone), stays there, then a real blink
  const f = (x) => x < 2000 ? 0.1 : x < 6000 ? 0.1 + 0.35 * (x - 2000) / 4000 : 0.45;
  assert.deepStrictEqual(feed(new BlinkDetector(), f, 0, 10000), []);
  const g = (x) => Math.max(f(x), blinkAt(10000, 0.9, 0.45)(x));
  assert.strictEqual(feed(new BlinkDetector(), g, 0, 11000).length, 1, 'a blink from there still counts');
});

t('eyes held shut is one blink, then becomes the new normal', () => {
  const f = (x) => (x >= 2000 && x < 6000 ? 0.85 : 0.1);
  const seen = feed(new BlinkDetector(), f, 0, 9000);
  assert.strictEqual(seen.length, 1);
});

t('losing the face never makes a blink', () => {
  const face = (ms) => !(ms > 3000 && ms < 3300);
  const f = (x) => (x > 3000 && x < 3300 ? 0.9 : 0.1);
  assert.deepStrictEqual(feed(new BlinkDetector(), f, 0, 6000, 30, face), []);
});

t('board: a mirrored blink happens at once and pauses its own random blinks for 9 s', () => {
  const a = new Animator().begin(CONFIG, 5, 0);
  for (let ms = 0; ms <= 1000; ms += 16) a.update(ms);
  assert.ok(a.mirrorBlink(1000));
  a.update(1060);
  assert.ok(a.scene().blink < 0.6, 'eyes closing right away');
  let autoBlinks = 0;
  for (let ms = 1076; ms <= 9900; ms += 16) { a.update(ms); if (a.takeBlinkEvent()) autoBlinks++; }
  assert.strictEqual(autoBlinks, 0, 'no random blinks while it follows yours');
  for (let ms = 9916; ms <= 20000; ms += 16) { a.update(ms); if (a.takeBlinkEvent()) autoBlinks++; }
  assert.ok(autoBlinks > 0, 'random blinks come back after you stop blinking');
});
console.log(`\n${n} passed`);
