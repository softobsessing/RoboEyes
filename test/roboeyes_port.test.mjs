// Compares roboeyes.js (the web app's preview) with the board's firmware, pixel by pixel.
// From the repo root:
//   clang++ -O2 -std=c++17 -o /tmp/robo_parity firmware/RoboEyes/extras/parity/parity.cpp
//   mkdir -p /tmp/robo_parity_out && /tmp/robo_parity /tmp/robo_parity_out
//   node test/roboeyes_port.test.mjs /tmp/robo_parity_out
// The scripted scenario below must match firmware/RoboEyes/extras/parity/parity.cpp.
import { readFileSync, existsSync } from 'node:fs';
import assert from 'node:assert';
import { Animator, CONFIG, prepare, shade, SCREEN } from '../roboeyes.js';

const dir = process.argv[2] || '/tmp/robo_parity_out';
if (!existsSync(dir)) { console.log(`skip: no firmware frames in ${dir}`); process.exit(0); }

const N = SCREEN;
const SHOTS = [1400, 1890, 2700, 2900, 3420, 3600, 4330, 4420, 4700, 5300, 5800, 6200, 6800, 7700, 8400, 8990];
const expand = (c) => [((c >> 11) & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31];

function render(a) {
  const s = a.scene(), p = prepare(s), f = new Uint16Array(N * N);
  for (let y = 0; y < N; y++) for (let x = 0; x < N; x++) f[y * N + x] = shade(s, p, x, y);
  return f;
}

let worstFrame = 0, totalBig = 0, frames = 0;
function compare(name, js) {
  const buf = readFileSync(`${dir}/${name}.bin`);
  const ref = new Uint16Array(buf.buffer, buf.byteOffset, N * N);
  let big = 0, diff = 0;
  for (let y = 0; y < N; y++) for (let x = 0; x < N; x++) {
    const dx = x + 0.5 - N / 2, dy = y + 0.5 - N / 2;
    if (dx * dx + dy * dy > (N / 2) ** 2) continue;   // outside the round glass
    const i = y * N + x;
    if (js[i] === ref[i]) continue;
    diff++;
    const a = expand(js[i]), b = expand(ref[i]);
    // more than one RGB565 step apart = a real difference (one step = float rounding on an edge)
    if (Math.abs(a[0] - b[0]) > 9 || Math.abs(a[1] - b[1]) > 5 || Math.abs(a[2] - b[2]) > 9) big++;
  }
  worstFrame = Math.max(worstFrame, big); totalBig += big; frames++;
  console.log(`${name.padEnd(10)} ${diff ? `${diff} px differ by a rounding step, ${big} by more` : 'identical'}`);
}

// ---- linked (E lines): mood, look, blinks; touches swap colors mid-way; colors set from the app ----
{
  const a = new Animator().begin(CONFIG, 42, 1000);
  let k = 0;
  for (let t = 1000; t <= 9000; t += 16) {
    a.setLinked(true); a.setAutoBlink(false);
    a.setMood(t < 2500 ? 0 : t < 4000 ? 1 : t < 5500 ? 2 : t < 7000 ? 3 : 0);
    a.setLook(Math.fround(0.6 * Math.sin(t / 700)), Math.fround(0.4 * Math.cos(t / 900)));
    if (t === 1800 || t === 6096) a.blinkNow(t, false);
    if (t === 3304) a.blinkNow(t, true);
    if (t === 4200) a.touch(true, t);
    if (t === 4296) a.touch(false, t);
    if (t === 4504) a.touch(true, t);    // touched again mid-wipe: the swap runs backwards
    if (t === 4600) a.touch(false, t);
    if (t === 5000) a.setColors(0x14213D, 0xFF5FA2);
    if (t === 7496) a.touch(true, t);
    if (t === 7608) a.touch(false, t);
    a.update(t);
    for (const s of SHOTS) if (s >= t && s < t + 16) compare(`linked_${k++}`, render(a));
  }
}
// ---- standalone: blinks, glances, floats; a touch swaps colors + smiles, then back to pills ----
{
  const a = new Animator().begin(CONFIG, 7, 1000);
  let k = 0;
  for (let t = 1000; t <= 9000; t += 16) {
    if (t === 3000) a.touch(true, t);
    if (t === 3208) a.touch(false, t);
    if (t === 6008) a.setColors(0xFFE8D6, 0x2A9D8F);
    if (t === 7000) a.touch(true, t);
    if (t === 7000) a.touch(false, t);   // the web app's TAP: down + up at once
    a.update(t);
    for (const s of SHOTS) if (s >= t && s < t + 16) compare(`solo_${k++}`, render(a));
  }
}

console.log(`\n${frames} frames compared; worst frame has ${worstFrame} pixels off by more than a rounding step`);
assert.ok(worstFrame <= 40, 'roboeyes.js no longer matches the firmware');
console.log('ok: the web app preview matches the board');
