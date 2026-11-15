// app.js — the RoboEyes web app: connect to a RoboEyes board (USB or Bluetooth), pick its face
// and eye colors live, save color pairs, and mirror touches in the preview.
import { SerialLink, BleLink } from './link.js';
import { Engine, ScreenPreview } from './roboeyes.js';
import { ColorPicker, DEFAULT_COLORS, hex6 } from './colors.js';
import { PROTOCOL, bleName, colorsLine, parseColors, parseHello } from './board.js';

const $ = (id) => document.getElementById(id);
const el = {
  pill: $('pill'), btnUsb: $('btnUsb'), btnUsbAny: $('btnUsbAny'), btnBle: $('btnBle'), btnBleAny: $('btnBleAny'),
  linkMsg: $('linkMsg'), toast: $('toast'), btnTap: $('btnTap'), mood: $('mood'), moodSub: $('moodSub'),
  boardCard: $('boardCard'), boardName: $('boardName'), boardMeta: $('boardMeta'),
};

// ------------------------------------------------------------------ saved in this browser
// (the doomscroll-eyes app lives on the same site and uses "eyebuddy.*" keys; these never clash)
const LS = { colors: 'roboeyes.colors', savedColors: 'roboeyes.savedColors', board: 'roboeyes.board' };
function loadJSON(key, fallback) {
  try { const v = JSON.parse(localStorage.getItem(key)); return v ?? fallback; } catch { return fallback; }
}
function saveJSON(key, v) { try { localStorage.setItem(key, JSON.stringify(v)); } catch {} }

// ------------------------------------------------------------------ live preview
const engine = new Engine((Math.random() * 2 ** 32) >>> 0);
const preview = new ScreenPreview($('screen'));
const nowMs = () => Math.floor(performance.now());

// ------------------------------------------------------------------ colors
let colors = loadJSON(LS.colors, DEFAULT_COLORS);
engine.setColors(colors.face, colors.eye);
const showColors = (c) => {   // the stage glows in the face color
  document.documentElement.style.setProperty('--face', `#${hex6(c.face)}`);
  document.documentElement.style.setProperty('--eyes', `#${hex6(c.eye)}`);
};
showColors(colors);

const picker = new ColorPicker({
  colors,
  saved: loadJSON(LS.savedColors, []),
  onChange: (c) => {   // live while dragging: preview now, board right away
    colors = c;
    showColors(c);
    engine.setColors(c.face, c.eye);
    if (ready()) link.send(colorsLine(c));
    saveJSON(LS.colors, c);
  },
  onSavedChange: (saved) => saveJSON(LS.savedColors, saved),
  target: 'face',   // the face starts white, so the wheel shows all its colors (black eyes = a black wheel)
});

// The board (or a touch on the preview) changed the colors: show them, don't send them back.
function adoptColors(c) {
  if (c.face === colors.face && c.eye === colors.eye) return;
  colors = { face: c.face, eye: c.eye };
  picker.setColors(colors, { silent: true });
  showColors(colors);
  engine.setColors(colors.face, colors.eye);   // no-op if the preview is already swapping to them
  saveJSON(LS.colors, colors);
}

// ------------------------------------------------------------------ board link
const usb = new SerialLink();
const ble = new BleLink();
let link = null;        // the active Link
let board = null;       // {id, fw, proto} once the board said HELLO RoboEyes
let helloTimers = [];
let known = loadJSON(LS.board, null);   // last board seen here: {id, fw, lastSeen}

const ready = () => !!(link?.connected && board);

for (const l of [usb, ble]) {
  l.onStatus((s) => {
    if (s.state === 'connected') {
      link = l;
      board = null;
      waitForHello(l);
    } else if (link === l) {
      clearHelloTimers();
      board = null;
      if (s.state === 'disconnected') link = null;
    }
    if (link === l || !link) renderPill(s);
    renderConnButtons();
    renderBoard();
  });
  l.onLine((line) => handleBoardLine(l, line));
}

// link.js sends P on connect. A board that's still booting may miss it: ask again, then give up.
function waitForHello(l) {
  clearHelloTimers();
  helloTimers = [
    setTimeout(() => { if (link === l && l.connected && !board) l.send('P'); }, 1500),
    setTimeout(() => {
      if (link !== l || !l.connected || board) return;
      showLinkMsg('No answer from this board. Is the RoboEyes firmware on it? (If the Serial Monitor is open in the Arduino IDE, close it.)', 'warn');
      renderPill({ state: 'connected', kind: l.kind });
    }, 4500),
  ];
}
function clearHelloTimers() { helloTimers.forEach(clearTimeout); helloTimers = []; }

function handleBoardLine(l, line) {
  if (l !== link) return;
  const hello = parseHello(line);
  if (hello) { onHello(l, hello); return; }
  if (!board) return;   // nothing else counts until it has said who it is
  const c = parseColors(line);
  if (c) { adoptColors(c); return; }
  const w = line.trim().split(/\s+/);
  if (w[0] === 'T' && (w[1] === 'touch' || w[1] === 'release')) {
    engine.touch(w[1] === 'touch', nowMs());   // the preview swaps + smiles along with the board
    return;
  }
  console.debug('[board]', line);   // e.g. "D fps 58.9 draw_ms 9.1"
}

async function onHello(l, h) {
  clearHelloTimers();
  if (h.kind === 'other') {
    // Same USB chip, different firmware: point to its own app and leave it alone.
    const o = h.other;
    showLinkMsg(o ? `That's ${o.label}, not a RoboEyes. It has its own app:` : `That board says it's "${h.name}", not a RoboEyes. This app only works with RoboEyes boards.`,
      'warn', o ? { text: `open ${o.app}`, href: o.url } : null);
    if (l === usb) {
      await usb.reject();
      usb.autoReconnect().catch(() => {});   // maybe another granted port is a RoboEyes
    } else await l.disconnect();
    return;
  }
  if (h.proto < PROTOCOL) {
    showLinkMsg(`This RoboEyes has old firmware (v${h.fw}). Flash the new firmware from the README to use this app.`, 'warn',
      { text: 'how to flash', href: 'https://github.com/softobsessing/RoboEyes#flashing-the-firmware' });
    if (l === usb) await usb.reject(); else await l.disconnect();
    return;
  }
  const first = !board;
  board = { id: h.id, fw: h.fw, proto: h.proto };
  if (h.id) { known = { id: h.id, fw: h.fw, lastSeen: Date.now() }; saveJSON(LS.board, known); }
  showLinkMsg('');
  if (first) toast(`Connected to ${h.id ? bleName(h.id) : 'RoboEyes'}`);
  // the board's own colors come next (C line) and the page takes them
  renderPill({ state: l.state, kind: l.kind });
  renderConnButtons();
  renderBoard();
}

function showLinkMsg(text, kind = 'info', action = null) {
  el.linkMsg.hidden = !text;
  el.linkMsg.dataset.kind = kind;
  el.linkMsg.textContent = text;
  if (text && action) {
    const a = document.createElement('a');
    a.href = action.href;
    a.textContent = action.text;
    el.linkMsg.append(' ', a);
  }
}

function renderPill(s) {
  const kind = s.kind === 'usb' ? 'USB' : 'Bluetooth';
  const who = board?.id ? bleName(board.id) : 'RoboEyes';
  const hello = s.state === 'connected' && !board;
  el.pill.dataset.state = hello ? 'connecting' : s.state;
  el.pill.textContent = {
    connected: hello ? `${kind} · saying hello…` : `${kind} · ${who}`,
    connecting: `${kind}: connecting…`,
    reconnecting: s.detail || `${kind}: reconnecting…`,
    error: s.detail || `${kind} error`,
    disconnected: s.detail ? `not connected (${s.detail})` : 'not connected',
  }[s.state];
  el.pill.title = s.detail || '';
}

function renderConnButtons() {
  const mine = known?.id || null;
  el.btnUsb.textContent = usb.connected ? 'Disconnect USB' : 'Connect USB';
  el.btnBle.textContent = ble.connected || ble.state === 'reconnecting' ? 'Disconnect Bluetooth'
    : mine ? `Connect ${bleName(mine)}` : 'Connect Bluetooth';
  el.btnBle.title = mine ? `Bluetooth, only ${bleName(mine)}` : 'Bluetooth: pick your RoboEyes';
  el.btnUsb.disabled = !SerialLink.supported();
  el.btnUsbAny.disabled = !SerialLink.supported() || usb.connected;
  el.btnBle.disabled = el.btnBleAny.disabled = !BleLink.supported();
  el.btnBleAny.hidden = !mine;
}

function ago(t) {
  const m = Math.round((Date.now() - t) / 60000);
  return m < 1 ? 'just now' : m < 60 ? `${m} min ago` : m < 1440 ? `${Math.round(m / 60)} h ago` : `${Math.round(m / 1440)} d ago`;
}

function renderBoard() {
  const live = ready();
  el.boardCard.dataset.live = String(live);
  if (live) {
    el.boardName.textContent = board.id ? bleName(board.id) : 'RoboEyes';
    el.boardMeta.textContent = `connected over ${link.kind === 'usb' ? 'USB' : 'Bluetooth'} · firmware v${board.fw}`;
  } else if (known?.id) {
    el.boardName.textContent = bleName(known.id);
    el.boardMeta.textContent = `not connected · last seen ${ago(known.lastSeen)} · firmware v${known.fw}`;
  } else {
    el.boardName.textContent = 'No board yet';
    el.boardMeta.textContent = 'Plug it in with USB, or connect over Bluetooth.';
  }
}

async function connectWith(l, opts) {
  const other = l === usb ? ble : usb;
  showLinkMsg('');
  try {
    if (other.connected || other.state === 'reconnecting') await other.disconnect();
    await l.connect(opts);
  } catch (e) {
    if (e?.name === 'NotFoundError') {   // chooser cancelled or nothing matched
      const detail = l === usb ? 'no board picked; try "Other port…"' : opts?.id ? `${bleName(opts.id)} not found; is it on and nearby?` : 'no RoboEyes picked';
      renderPill({ state: 'disconnected', kind: l.kind, detail });
    } else {
      renderPill({ state: 'error', kind: l.kind, detail: e?.message || String(e) });
    }
  }
  renderConnButtons();
}

el.btnUsb.onclick = () => (usb.connected ? usb.disconnect() : connectWith(usb));
el.btnUsbAny.onclick = () => connectWith(usb, { anyPort: true });
el.btnBle.onclick = () => (ble.connected || ble.state === 'reconnecting' ? ble.disconnect() : connectWith(ble, { id: known?.id || null }));
el.btnBleAny.onclick = () => connectWith(ble, {});

// ------------------------------------------------------------------ touch
// Click the face = a touch. With a board connected the board does it (TAP) and reports back
// (T touch, C <colors>, T release), and the preview follows; otherwise the preview does it alone.
el.btnTap.onclick = () => {
  if (ready()) { link.send('TAP'); return; }
  const t = nowMs();
  engine.touch(true, t);
  engine.touch(false, t);
  adoptColors({ face: engine.faceColor, eye: engine.eyeColor });
};

let toastTimer = 0;
function toast(msg) {
  el.toast.textContent = msg;
  el.toast.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.toast.hidden = true; }, 2500);
}

// ------------------------------------------------------------------ frame loop
let lastSmile = null, lastLive = null;
function frame() {
  engine.update(nowMs());
  preview.draw(engine);
  const smile = engine.smilingNow, live = ready();
  if (smile !== lastSmile || live !== lastLive) {
    lastSmile = smile; lastLive = live;
    el.mood.dataset.smile = String(smile);
    el.mood.textContent = smile ? 'smiling at you' : 'just looking around';
    el.moodSub.textContent = live ? 'Touch the board, or click the face here.' : 'Click the face to try a touch.';
  }
  requestAnimationFrame(frame);
}
requestAnimationFrame(frame);

renderPill({ state: 'disconnected', kind: 'usb' });
renderConnButtons();
renderBoard();
// Reopen a board this site was allowed to use before: USB first, then the last Bluetooth board.
usb.autoReconnect().then((ok) => ok || ble.autoReconnect(known?.id)).catch(() => {});
