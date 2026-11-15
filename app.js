// app.js — the RoboEyes web app: connect to a RoboEyes board (USB or Bluetooth), pick its face
// and eye colors live, save color pairs, mirror touches in the preview, and (camera on) make it
// blink when you blink.
import { SerialLink, BleLink } from './link.js';
import { Engine, ScreenPreview, CONFIG, SCREEN } from './roboeyes.js';
import { ColorPicker, DEFAULT_COLORS, hex6 } from './colors.js';
import { PROTOCOL, MOVES_FW, bleName, colorsLine, followLine, parseColors, parseFollow, parseHearts, parseHello, parseMode } from './board.js';
import { BlinkDetector, DEFAULT_SENSITIVITY, sensitivityLabel } from './blink.js';

const $ = (id) => document.getElementById(id);
const el = {
  pill: $('pill'), btnUsb: $('btnUsb'), btnUsbAny: $('btnUsbAny'), btnBle: $('btnBle'), btnBleAny: $('btnBleAny'),
  linkMsg: $('linkMsg'), toast: $('toast'), btnTap: $('btnTap'), mood: $('mood'), moodSub: $('moodSub'),
  boardCard: $('boardCard'), boardName: $('boardName'), boardMeta: $('boardMeta'),
  btnCam: $('btnCam'), camBox: $('camBox'), video: $('video'), camMsg: $('camMsg'), blinkFlash: $('blinkFlash'),
  meterWrap: $('meterWrap'), meterFill: $('meterFill'), meterLine: $('meterLine'),
  sens: $('sens'), sensLabel: $('sensLabel'), blinkStats: $('blinkStats'),
  touchMode: $('touchMode'), touchNote: $('touchNote'), movesNote: $('movesNote'),
  btnDartL: $('btnDartL'), btnDartR: $('btnDartR'), btnShake: $('btnShake'), btnHearts: $('btnHearts'),
};

// ------------------------------------------------------------------ saved in this browser
// (the doomscroll-eyes app lives on the same site and uses "eyebuddy.*" keys; these never clash)
const LS = { colors: 'roboeyes.colors', savedColors: 'roboeyes.savedColors', board: 'roboeyes.board', blink: 'roboeyes.blink', touchMode: 'roboeyes.touchMode' };
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
      if (engine.action === 1 && !dragging) engine.followEnd(nowMs());   // the board's finger is gone
      if (s.state === 'disconnected') link = null;
    }
    if (link === l || !link) renderPill(s);
    renderConnButtons();
    renderBoard();
    renderBlinkStats();
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
  const mode = parseMode(line);
  if (mode) { setTouchMode(mode, { send: false }); return; }   // the board's own setting wins
  const hearts = parseHearts(line);
  if (hearts !== null) { setHearts(hearts, { send: false }); return; }   // what the board is showing
  const f = parseFollow(line);
  if (f) {   // a finger on the board: the preview's eyes follow it too
    if (!dragging) { if (f.up) engine.followEnd(nowMs()); else engine.follow(f.x, f.y, nowMs()); }
    return;
  }
  const w = line.trim().split(/\s+/);
  if (w[0] === 'T' && (w[1] === 'touch' || w[1] === 'release')) {
    engine.touch(w[1] === 'touch', nowMs());   // the preview swaps along with the board
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
  renderBlinkStats();
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
  renderMoves();
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
  if (touchMode === 'follow') return;   // follow mode: dragging, handled below
  if (ready()) { link.send('TAP'); return; }
  const t = nowMs();
  engine.touch(true, t);
  engine.touch(false, t);
  adoptColors({ face: engine.faceColor, eye: engine.eyeColor });
};

// ------------------------------------------------------------------ blink with me (camera)
// The face model (MediaPipe, ~25 MB, runs in this browser) only loads once the camera is turned on.
const blinkPrefs = { on: false, sensitivity: DEFAULT_SENSITIVITY, ...loadJSON(LS.blink, {}) };
const detector = new BlinkDetector(blinkPrefs.sensitivity);
let vision = null, modelReady = null, camOn = false, camFace = false, mirrored = 0, flashTimer = 0;
const saveBlinkPrefs = () => saveJSON(LS.blink, { on: blinkPrefs.on, sensitivity: blinkPrefs.sensitivity });

el.sens.value = Math.round(blinkPrefs.sensitivity * 100);
const showSens = () => { el.sensLabel.textContent = sensitivityLabel(blinkPrefs.sensitivity); };
showSens();
el.sens.oninput = () => {
  blinkPrefs.sensitivity = detector.sensitivity = el.sens.value / 100;
  showSens();
  saveBlinkPrefs();
};

async function startCamera() {
  let stage = 'model';
  el.camBox.hidden = false;
  el.camMsg.textContent = 'starting the camera…';
  el.btnCam.disabled = true;
  try {
    if (!vision) {
      const { Vision } = await import('./vision.js');
      vision = new Vision();
      modelReady = vision.init();   // loads while the camera permission prompt is up
      modelReady.catch(() => {});   // (awaited below)
    }
    stage = 'camera';
    if (!navigator.mediaDevices?.getUserMedia) throw Object.assign(new Error('no camera API'), { name: 'NoCameraApi' });
    await vision.startCamera(el.video);
    el.camMsg.textContent = 'loading the face model…';
    stage = 'model';
    await modelReady;
    camOn = true;
    blinkPrefs.on = true; saveBlinkPrefs();
    el.camMsg.textContent = '';
    el.meterWrap.hidden = false;
    const step = () => { if (!vision.stream) return; onCameraFrame(); el.video.requestVideoFrameCallback(step); };
    el.video.requestVideoFrameCallback(step);
  } catch (e) {
    console.warn('[camera]', e);
    vision?.stopCamera(el.video);
    if (stage === 'model') vision = null;   // try loading again next time
    camOn = false;
    el.camMsg.textContent = stage === 'camera' ? ({
      NotAllowedError: 'Camera blocked. Allow it with the icon in the address bar, then try again.',
      NotFoundError: 'No camera found.',
      NotReadableError: 'The camera is busy in another app.',
      NoCameraApi: 'No camera access here. Use Chrome on https:// or http://localhost.',
    }[e?.name] || `Camera unavailable: ${e?.message || e}`) : `The face model didn't load: ${e?.message || e}`;
  }
  el.btnCam.disabled = false;
  renderCamButton();
  renderBlinkStats();
}

function stopCamera() {
  vision?.stopCamera(el.video);
  camOn = false; camFace = false;
  blinkPrefs.on = false; saveBlinkPrefs();
  el.camBox.hidden = true;
  el.meterWrap.hidden = true;
  renderCamButton();
  renderBlinkStats();
}

function onCameraFrame() {
  if (!vision?.ready) return;
  const t = performance.now();
  let sig;
  try { sig = vision.detect(el.video, Math.floor(t)); } catch { return; }
  const hadFace = camFace;
  camFace = sig.face;
  if (detector.update(sig.face, sig.blink, t)) blinkWithYou();
  el.camMsg.textContent = sig.face ? '' : 'Looking for your face…';
  el.meterFill.style.width = `${Math.min(100, (sig.face ? sig.blink : 0) * 100)}%`;
  el.meterFill.dataset.closed = String(detector.closed);
  el.meterLine.style.left = `${Math.min(99.5, detector.threshold * 100)}%`;
  if (hadFace !== camFace) renderBlinkStats();
}

// You blinked: the preview blinks now, and the board does too (B).
function blinkWithYou() {
  engine.mirrorBlink(nowMs());
  if (ready()) link.send('B');
  mirrored++;
  el.blinkFlash.classList.add('on');
  clearTimeout(flashTimer);
  flashTimer = setTimeout(() => el.blinkFlash.classList.remove('on'), 120);
  renderBlinkStats();
}

function renderCamButton() {
  el.btnCam.textContent = camOn ? 'Turn off camera' : 'Turn on camera';
  el.btnCam.classList.toggle('primary', !camOn);
}

function renderBlinkStats() {
  const oldFw = ready() && board.fw < 3;
  el.blinkStats.textContent = !camOn ? 'Camera off.'
    : oldFw ? `Only the preview blinks with you: this board has firmware v${board.fw}. Flash the new firmware (v3) so the board does too.`
    : !camFace ? 'Looking for your face…'
    : `${mirrored} blink${mirrored === 1 ? '' : 's'} copied${ready() ? ' to the board' : ' (preview only: no board connected)'}.`;
}

el.btnCam.onclick = () => (camOn ? stopCamera() : startCamera());

// ------------------------------------------------------------------ touch & moves
// Touch mode: what touching the screen does. The board keeps its own (saved on it) and tells us
// with "M swap|follow"; picking one here sends it. Moves play on the preview and the board together.
const ACT_FOLLOW = 1, ACT_DART = 2, ACT_SHAKE = 3;   // engine.action (same numbers as eyes_anim.h)
let touchMode = loadJSON(LS.touchMode, 'swap') === 'follow' ? 'follow' : 'swap';
const movesOk = () => ready() && board.fw >= MOVES_FW;

function setTouchMode(mode, { send = true } = {}) {
  touchMode = mode === 'follow' ? 'follow' : 'swap';
  saveJSON(LS.touchMode, touchMode);
  if (send && movesOk()) link.send(`M ${touchMode}`);
  if (touchMode !== 'follow' && dragging) endDrag();
  renderMoves();
}
for (const b of el.touchMode.querySelectorAll('button')) b.onclick = () => setTouchMode(b.dataset.mode);

function renderMoves() {
  for (const b of el.touchMode.querySelectorAll('button')) b.setAttribute('aria-pressed', String(b.dataset.mode === touchMode));
  el.btnTap.dataset.mode = touchMode;
  el.btnTap.title = touchMode === 'follow' ? 'Drag on the face: the eyes follow' : 'Touch the face';
  el.btnTap.setAttribute('aria-label', touchMode === 'follow' ? 'Drag on the face: the eyes follow your pointer' : 'Touch the face: swap the colors');
  const old = ready() && board.fw < MOVES_FW;
  el.touchNote.textContent = touchMode === 'follow'
    ? 'Touch the board and drag: the eyes follow your finger, stay a moment when you let go, then wander home. Drag on the preview to do the same.'
    : 'Touch the board: the face and eye colors swap. Touch again to swap back.';
  if (old) el.touchNote.textContent += ` (Your board has firmware v${board.fw}, which only swaps colors.)`;
  el.movesNote.textContent = old ? `Preview only: this board has firmware v${board.fw}. Flash the new firmware (v${MOVES_FW}) so the board moves too.`
    : ready() ? 'The board and the preview move together. Shortcuts: ← → S and H.'
    : 'No board connected: the moves play on the preview. Shortcuts: ← → S and H.';
  el.moodSub.textContent = touchMode === 'follow'
    ? (ready() ? 'Touch the board, or drag on the face here: the eyes follow.' : 'Drag on the face: the eyes follow.')
    : (ready() ? 'Touch the board, or click the face here, to swap the colors.' : 'Click the face to try a touch: the colors swap.');
}

function dart(dir) {
  engine.dart(dir, nowMs());
  if (movesOk()) link.send(dir < 0 ? 'DART L' : 'DART R');
}
function shake() {
  const seed = ((Math.random() * 2 ** 32) >>> 0) || 1;   // same seed on both = the same bounces
  engine.shake(seed, nowMs());
  if (movesOk()) link.send(`SHAKE ${seed}`);
}
// Heart eyes: on until you turn them off (the preview and the board together).
function setHearts(on, { send = true } = {}) {
  engine.setHearts(on, nowMs());
  el.btnHearts.setAttribute('aria-pressed', String(on));
  el.btnHearts.lastChild.textContent = on ? ' Back to normal eyes' : ' Heart eyes';
  if (send && movesOk()) link.send(on ? 'H 1' : 'H 0');
}
el.btnHearts.onclick = () => setHearts(!engine.heartsOn);
el.btnDartL.onclick = () => dart(-1);
el.btnDartR.onclick = () => dart(1);
el.btnShake.onclick = () => shake();
document.addEventListener('keydown', (e) => {
  if (e.metaKey || e.ctrlKey || e.altKey || e.target.closest?.('input, textarea')) return;
  if (e.key === 'ArrowLeft') { dart(-1); e.preventDefault(); }
  else if (e.key === 'ArrowRight') { dart(1); e.preventDefault(); }
  else if (e.key === 's' || e.key === 'S') shake();
  else if (e.key === 'h' || e.key === 'H') setHearts(!engine.heartsOn);
});

// Follow mode: drag on the preview and the eyes follow, on the board too (F lines, ~30 a second).
let dragging = false, lastFollowSent = 0, lastFollow = null;
function pointOnFace(e) {
  const r = $('screen').getBoundingClientRect();
  const px = (e.clientX - r.left) / r.width * SCREEN, py = (e.clientY - r.top) / r.height * SCREEN;
  return [(px - SCREEN / 2) / CONFIG.faceRadius, (py - SCREEN / 2) / CONFIG.faceRadius];
}
function dragTo(e, force) {
  const [x, y] = pointOnFace(e), t = nowMs();
  engine.follow(x, y, t);
  lastFollow = [x, y];
  if (movesOk() && (force || t - lastFollowSent >= 33)) { link.send(followLine(x, y)); lastFollowSent = t; }
}
function endDrag() {
  dragging = false;
  engine.followEnd(nowMs());
  if (movesOk()) {
    if (lastFollow) link.send(followLine(...lastFollow));   // where it really ended (sends are throttled)
    link.send('F up');
  }
  lastFollow = null;
}
el.btnTap.addEventListener('pointerdown', (e) => {
  if (touchMode !== 'follow') return;
  dragging = true;
  el.btnTap.setPointerCapture(e.pointerId);
  dragTo(e, true);
  e.preventDefault();
});
el.btnTap.addEventListener('pointermove', (e) => { if (dragging) dragTo(e, false); });
el.btnTap.addEventListener('pointerup', () => { if (dragging) endDrag(); });
el.btnTap.addEventListener('pointercancel', () => { if (dragging) endDrag(); });

let toastTimer = 0;
function toast(msg) {
  el.toast.textContent = msg;
  el.toast.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.toast.hidden = true; }, 2500);
}

// ------------------------------------------------------------------ frame loop
let lastMood = '';
function frame() {
  engine.update(nowMs());
  preview.draw(engine);
  const act = engine.action;
  const mood = act === ACT_FOLLOW ? 'following your finger'
    : act === ACT_DART ? (engine.anim.dartDir < 0 ? 'darting left' : 'darting right')
    : act === ACT_SHAKE ? 'whoa, shaken!'
    : engine.heartsOn ? 'in love'
    : camOn && camFace ? 'blinking with you' : 'just looking around';
  if (mood !== lastMood) { lastMood = mood; el.mood.textContent = mood; }
  requestAnimationFrame(frame);
}
requestAnimationFrame(frame);

renderPill({ state: 'disconnected', kind: 'usb' });
renderConnButtons();
renderBoard();
renderMoves();
renderCamButton();
renderBlinkStats();
if (blinkPrefs.on) startCamera();   // it was on last time (Chrome remembers the camera permission)
// Reopen a board this site was allowed to use before: USB first, then the last Bluetooth board.
usb.autoReconnect().then((ok) => ok || ble.autoReconnect(known?.id)).catch(() => {});
