// board.js — what a RoboEyes board says, and how to recognise one.
//
// Every board has a 4-hex ID from its chip (last 2 bytes of its MAC); its Bluetooth name is
// "RoboEyes-<ID>". It answers P with
//     HELLO RoboEyes <protocol> <ID> <firmware>      e.g. "HELLO RoboEyes 2 3F2A 2"
//     C <face RRGGBB> <eyes RRGGBB>                  its current colors
// This app only drives RoboEyes boards. Other boards on the same USB chip (an EyeBuddy from
// doomscroll-eyes says "HELLO EyeBuddy ...") get pointed to their own app instead.

export const BLE_PREFIX = 'RoboEyes';
export const bleName = (id) => `${BLE_PREFIX}-${id}`;
export const idFromName = (name) => (/^RoboEyes-([0-9A-F]{4})$/i.exec(name || '') || [])[1]?.toUpperCase() || null;

/** First protocol this app speaks (firmware v2: colors, board ID, touch = color swap). */
export const PROTOCOL = 2;

/** Web apps for the other boards that use the same ESP32-S3 screen. */
export const OTHER_APPS = {
  EyeBuddy: { label: 'an EyeBuddy (doomscroll-eyes) board', app: 'doomscroll-eyes', url: 'https://softobsessing.github.io/doomscroll-eyes/' },
};

/**
 * Reads a HELLO line.
 *   "HELLO RoboEyes 2 3F2A 2" -> {kind: 'roboeyes', proto: 2, id: '3F2A', fw: 2}
 *   "HELLO RoboEyes 1"        -> {kind: 'roboeyes', proto: 1, id: null, fw: 1}   (old firmware)
 *   "HELLO EyeBuddy 4 ..."    -> {kind: 'other', name: 'EyeBuddy', other: OTHER_APPS.EyeBuddy}
 *   anything else             -> null
 */
export function parseHello(line) {
  const p = String(line).trim().split(/\s+/);
  if (p[0] !== 'HELLO' || !p[1]) return null;
  if (p[1] !== 'RoboEyes') return { kind: 'other', name: p[1], other: OTHER_APPS[p[1]] || null };
  const proto = parseInt(p[2], 10) || 1;
  if (proto < PROTOCOL) return { kind: 'roboeyes', proto, id: null, fw: proto };
  const id = /^[0-9A-F]{4}$/i.test(p[3] || '') ? p[3].toUpperCase() : null;
  return { kind: 'roboeyes', proto, id, fw: parseInt(p[4], 10) || 0 };
}

/** "C FFFFFF 000000" -> {face: 0xFFFFFF, eye: 0x000000}; anything else -> null. */
export function parseColors(line) {
  const m = /^C ([0-9A-F]{6}) ([0-9A-F]{6})$/i.exec(String(line).trim());
  return m ? { face: parseInt(m[1], 16), eye: parseInt(m[2], 16) } : null;
}

/** The colors line the app sends: K <face> <eyes>. */
export const colorsLine = ({ face, eye }) =>
  `K ${(face & 0xFFFFFF).toString(16).padStart(6, '0').toUpperCase()} ${(eye & 0xFFFFFF).toString(16).padStart(6, '0').toUpperCase()}`;

/** First firmware with finger-follow, darts, shake and heart eyes. */
export const MOVES_FW = 4;

/** "M swap" / "M follow" (the board's touch mode) -> 'swap' | 'follow'; anything else -> null. */
export const parseMode = (line) => (/^M (swap|follow)$/.exec(String(line).trim()) || [])[1] || null;

/**
 * A finger the eyes follow (ball radii from the screen center, +x right, +y down):
 *   "F 12 -40" -> {x: 0.12, y: -0.4};  "F up" -> {up: true};  anything else -> null.
 */
export function parseFollow(line) {
  const s = String(line).trim();
  if (s === 'F up') return { up: true };
  const m = /^F (-?\d{1,3}) (-?\d{1,3})$/.exec(s);
  return m ? { x: Math.max(-100, Math.min(100, +m[1])) / 100, y: Math.max(-100, Math.min(100, +m[2])) / 100 } : null;
}

/** "H 1" / "H 0" (heart eyes on / off) -> true | false; anything else -> null. */
export const parseHearts = (line) => { const m = /^H ([01])$/.exec(String(line).trim()); return m ? m[1] === '1' : null; };

/** The line that drags the board's eyes: F <x> <y> (-100..100). */
export const followLine = (x, y) => {
  const c = (v) => Math.max(-100, Math.min(100, Math.round(v * 100)));
  return `F ${c(x)} ${c(y)}`;
};
