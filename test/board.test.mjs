// node test/board.test.mjs — what counts as a RoboEyes board, and the color lines.
import assert from 'node:assert';
import { parseHello, parseColors, colorsLine, idFromName, bleName, PROTOCOL, parseMode, parseFollow, followLine, parseHearts } from '../board.js';

let n = 0;
const t = (name, fn) => { fn(); n++; console.log(`ok  ${name}`); };

t('a current RoboEyes board', () => {
  assert.deepStrictEqual(parseHello('HELLO RoboEyes 2 3f2a 2'), { kind: 'roboeyes', proto: 2, id: '3F2A', fw: 2 });
  assert.strictEqual(PROTOCOL, 2);
});
t('old RoboEyes firmware is recognised as old', () => {
  const h = parseHello('HELLO RoboEyes 1');
  assert.strictEqual(h.kind, 'roboeyes');
  assert.ok(h.proto < PROTOCOL);
});
t('an EyeBuddy board is pointed to the doomscroll-eyes app', () => {
  const h = parseHello('HELLO EyeBuddy 4 3F2A 8 0123456789abcdef');
  assert.strictEqual(h.kind, 'other');
  assert.strictEqual(h.name, 'EyeBuddy');
  assert.match(h.other.url, /doomscroll-eyes/);
});
t('an unknown board is "other" with no app', () => {
  assert.deepStrictEqual(parseHello('HELLO Gizmo 1'), { kind: 'other', name: 'Gizmo', other: null });
});
t('non-hello lines are ignored', () => {
  for (const l of ['C FFFFFF 000000', 'T touch', 'HELLO', 'D fps 60 draw_ms 9', '']) assert.strictEqual(parseHello(l), null);
});
t('color lines both ways', () => {
  assert.deepStrictEqual(parseColors('C ffffff 000000'), { face: 0xFFFFFF, eye: 0 });
  assert.deepStrictEqual(parseColors('C 14213D FF5FA2\r'), { face: 0x14213D, eye: 0xFF5FA2 });
  assert.strictEqual(parseColors('C light'), null);   // old firmware's color line
  assert.strictEqual(colorsLine({ face: 0x14213D, eye: 0xFF5FA2 }), 'K 14213D FF5FA2');
  assert.strictEqual(colorsLine({ face: 0, eye: 0xA }), 'K 000000 00000A');
});
t('Bluetooth names', () => {
  assert.strictEqual(bleName('3F2A'), 'RoboEyes-3F2A');
  assert.strictEqual(idFromName('RoboEyes-3f2a'), '3F2A');
  assert.strictEqual(idFromName('EyeBuddy-3F2A'), null);
});
t('touch mode lines', () => {
  assert.strictEqual(parseMode('M swap'), 'swap');
  assert.strictEqual(parseMode('M follow'), 'follow');
  assert.strictEqual(parseMode('M dance'), null);
  assert.strictEqual(parseMode('MOVE'), null);
});
t('follow lines both ways', () => {
  assert.deepStrictEqual(parseFollow('F 12 -40'), { x: 0.12, y: -0.4 });
  assert.deepStrictEqual(parseFollow('F up'), { up: true });
  assert.deepStrictEqual(parseFollow('F 250 0'), { x: 1, y: 0 });
  assert.strictEqual(parseFollow('F fast'), null);
  assert.strictEqual(followLine(0.123, -0.456), 'F 12 -46');
  assert.strictEqual(followLine(3, -3), 'F 100 -100');
});
t('heart eyes lines', () => {
  assert.strictEqual(parseHearts('H 1'), true);
  assert.strictEqual(parseHearts('H 0'), false);
  assert.strictEqual(parseHearts('HELLO RoboEyes 2 3F2A 4'), null);
});
console.log(`\n${n} passed`);
