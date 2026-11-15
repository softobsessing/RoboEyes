// link.js — one Link interface over USB (Web Serial) and Bluetooth LE (Web Bluetooth, Nordic UART).
//
//   link.connect()        open (may show Chrome's device chooser; needs a user gesture)
//   link.send(line)       queue one '\n'-terminated ASCII line
//   link.onLine(cb)       cb(line) for every line from the board (without '\n')
//   link.onStatus(cb)     cb({state, detail}) state: disconnected|connecting|connected|reconnecting|error
//   link.disconnect()
//   link.reject()         (USB) close a port that turned out to be some other board; auto-connect skips it
//
// Writes are serialized (never two in flight). While a write is in flight, a newer `E ...`
// line replaces any older queued `E ...` line (stale eye frames are dropped, newest kept), and the
// same for `K ...` color lines (dragging the color wheel); other lines (e.g. `P`) are queued in
// order and never dropped.

export const ESPRESSIF_VID = 0x303a;
export const NUS_SERVICE = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
export const NUS_RX = '6e400002-b5a3-f393-e0a9-e50e24dcca9e'; // laptop -> board (write)
export const NUS_TX = '6e400003-b5a3-f393-e0a9-e50e24dcca9e'; // board -> laptop (notify)
import { BLE_PREFIX, bleName } from './board.js';
const SERIAL_BAUD = 115200;
const BLE_CHUNK = 20;   // fits the smallest BLE packet; the board reassembles lines

const encoder = new TextEncoder();

/** Splits an incoming text stream into lines; tolerates \r\n and split/merged chunks. */
export class LineBuffer {
  constructor(onLine) { this.buf = ''; this.onLine = onLine; }
  push(text) {
    this.buf += text;
    let i;
    while ((i = this.buf.indexOf('\n')) >= 0) {
      const line = this.buf.slice(0, i).replace(/\r$/, '');
      this.buf = this.buf.slice(i + 1);
      if (line) this.onLine(line);
    }
    if (this.buf.length > 1024) this.buf = ''; // garbage guard
  }
}

class Link {
  constructor(kind) {
    this.kind = kind;
    this.state = 'disconnected';
    this._lineCbs = [];
    this._statusCbs = [];
    this._busy = false;
    this._pendingE = null;
    this._pendingK = null;
    this._pendingOther = [];
  }
  onLine(cb) { this._lineCbs.push(cb); return this; }
  onStatus(cb) { this._statusCbs.push(cb); return this; }
  get connected() { return this.state === 'connected'; }

  send(line) {
    if (!this.connected) return;
    if (!line.endsWith('\n')) line += '\n';
    if (line.startsWith('E ')) this._pendingE = line; // keep only the newest eye frame
    else if (line.startsWith('K ')) this._pendingK = line; // and the newest colors
    else this._pendingOther.push(line);
    this._pump();
  }

  async _pump() {
    if (this._busy) return;
    this._busy = true;
    try {
      while (this.connected && (this._pendingOther.length || this._pendingK || this._pendingE)) {
        let line;
        if (this._pendingOther.length) line = this._pendingOther.shift();
        else if (this._pendingK) { line = this._pendingK; this._pendingK = null; }
        else { line = this._pendingE; this._pendingE = null; }
        try {
          await this._write(line);
        } catch (e) {
          console.warn(`[${this.kind}] write failed:`, e?.message || e);
          break; // the disconnect handler will deal with the link
        }
      }
    } finally {
      this._busy = false;
    }
  }

  _clearQueue() { this._pendingE = null; this._pendingK = null; this._pendingOther = []; }

  _setStatus(state, detail = '') {
    this.state = state;
    for (const cb of this._statusCbs) cb({ state, detail, kind: this.kind });
  }

  _emitLine(line) { for (const cb of this._lineCbs) cb(line); }

  // subclasses: connect(), disconnect(), _write(line)
}

// ---------------------------------------------------------------- USB / Web Serial

export class SerialLink extends Link {
  constructor() {
    super('usb');
    this.port = null;
    this._reader = null;
    this._writer = null;
    this._pipeDone = null;
    this._userClosed = false;
    this._rejected = new WeakSet();   // ports that answered as some other kind of board
    if ('serial' in navigator) {
      navigator.serial.addEventListener('disconnect', (ev) => {
        if (ev.target === this.port || ev.port === this.port) this._lost('USB unplugged');
      });
      // A previously granted board was plugged back in: reopen it.
      navigator.serial.addEventListener('connect', (ev) => {
        const port = ev.target instanceof SerialPort ? ev.target : ev.port;
        if (!this.port && !this._userClosed && port && isEspressif(port) && !this._rejected.has(port)) this._open(port).catch(() => {});
      });
    }
  }

  static supported() { return 'serial' in navigator; }

  /** Ask the user for a port. anyPort=false filters to Espressif (VID 0x303A). */
  async connect({ anyPort = false } = {}) {
    if (!SerialLink.supported()) throw new Error('Web Serial not available (use desktop Chrome)');
    const port = await navigator.serial.requestPort(anyPort ? {} : { filters: [{ usbVendorId: ESPRESSIF_VID }] });
    this._userClosed = false;
    await this._open(port);
  }

  /** On page load: silently reopen a port the user granted before (skipping rejected ones). Returns true if opened. */
  async autoReconnect() {
    if (!SerialLink.supported()) return false;
    const ports = (await navigator.serial.getPorts()).filter((p) => !this._rejected.has(p));
    if (!ports.length) return false;
    const port = ports.find(isEspressif) || ports[0];
    try { await this._open(port); return true; } catch { return false; }
  }

  /** This port is some other board: close it and leave it alone (until the page reloads). */
  async reject() {
    if (this.port) this._rejected.add(this.port);
    await this.disconnect();
  }

  async _open(port) {
    if (this.port) await this._close();
    this._setStatus('connecting', 'opening USB port');
    try {
      await port.open({ baudRate: SERIAL_BAUD });
    } catch (e) {
      // "already open" happens if another tab / serial monitor holds it.
      this._setStatus('error', `could not open port: ${e.message}`);
      throw e;
    }
    this.port = port;
    this._writer = port.writable.getWriter();
    const decoder = new TextDecoderStream();
    this._pipeDone = port.readable.pipeTo(decoder.writable).catch(() => {});
    this._reader = decoder.readable.getReader();
    this._setStatus('connected', describePort(port));
    this._readLoop(this._reader);
    this.send('P');
  }

  async _readLoop(reader) {
    const lb = new LineBuffer((l) => this._emitLine(l));
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        if (value) lb.push(value);
      }
      if (reader === this._reader) this._lost('USB stream closed');
    } catch (e) {
      if (reader === this._reader) this._lost(`read error: ${e.message}`);
    }
  }

  _write(line) { return this._writer.write(encoder.encode(line)); }

  async _lost(why) {
    if (!this.port) return;
    await this._close();
    this._setStatus('disconnected', why);
  }

  async _close() {
    const port = this.port;
    this.port = null;
    this._clearQueue();
    const reader = this._reader, writer = this._writer;
    this._reader = this._writer = null;
    try { await reader?.cancel(); } catch {}
    try { await this._pipeDone; } catch {}
    try { writer?.releaseLock(); } catch {}
    try { await port?.close(); } catch {}
  }

  async disconnect() {
    this._userClosed = true;
    if (this.port) await this._close();
    this._setStatus('disconnected', '');
  }
}

function isEspressif(port) {
  try { return port.getInfo().usbVendorId === ESPRESSIF_VID; } catch { return false; }
}
function describePort(port) {
  try {
    const { usbVendorId: v, usbProductId: p } = port.getInfo();
    if (v === undefined) return 'USB serial';
    return `USB ${v.toString(16).padStart(4, '0')}:${(p ?? 0).toString(16).padStart(4, '0')}`;
  } catch { return 'USB serial'; }
}

// ---------------------------------------------------------------- Bluetooth LE / Nordic UART

export class BleLink extends Link {
  constructor() {
    super('ble');
    this.device = null;
    this._rx = null;
    this._tx = null;
    this._userClosed = false;
    this._decoder = new TextDecoder();
    this._lines = new LineBuffer((l) => this._emitLine(l));
    this._onNotify = (ev) => this._lines.push(this._decoder.decode(ev.target.value, { stream: true }));
    this._onGattDisconnected = () => this._handleDrop();
  }

  static supported() { return 'bluetooth' in navigator; }

  /** Chooser filtered to one board ("RoboEyes-3F2A") when id is given, else any RoboEyes board. */
  async connect({ id = null } = {}) {
    if (!BleLink.supported()) throw new Error('Web Bluetooth not available (use desktop Chrome)');
    const device = await navigator.bluetooth.requestDevice({
      filters: id ? [{ name: bleName(id) }] : [{ namePrefix: BLE_PREFIX }],
      optionalServices: [NUS_SERVICE],
    });
    await this._use(device);
  }

  /** On page load: reconnect to a board this site was allowed to use before, without the chooser.
   *  Needs Chrome's Bluetooth getDevices() (not in every Chrome); quietly does nothing otherwise. */
  async autoReconnect(id) {
    if (!BleLink.supported() || !navigator.bluetooth.getDevices || !id) return false;
    const device = (await navigator.bluetooth.getDevices()).find((d) => d.name === bleName(id));
    if (!device) return false;
    try {
      await Promise.race([this._use(device), new Promise((_, rej) => setTimeout(() => rej(new Error('not in range')), 8000))]);
      return true;
    } catch { this._userClosed = true; this._setStatus('disconnected', ''); return false; }
  }

  get deviceName() { return this.device?.name || ''; }

  async _use(device) {
    if (this.device && this.device !== device) await this.disconnect();
    this._userClosed = false;
    this.device = device;
    device.removeEventListener('gattserverdisconnected', this._onGattDisconnected);
    device.addEventListener('gattserverdisconnected', this._onGattDisconnected);
    this._setStatus('connecting', `connecting to ${device.name || BLE_PREFIX}`);
    try {
      await this._attach();
    } catch (e) {
      this._userClosed = true; // don't start the reconnect loop for a failed first attempt
      try { device.gatt.disconnect(); } catch {}
      this._setStatus('error', `Bluetooth: ${e.message}`);
      throw e;
    }
  }

  async _attach() {
    const server = await this.device.gatt.connect();
    const svc = await server.getPrimaryService(NUS_SERVICE);
    this._rx = await svc.getCharacteristic(NUS_RX);
    this._tx = await svc.getCharacteristic(NUS_TX);
    this._tx.removeEventListener('characteristicvaluechanged', this._onNotify);
    this._tx.addEventListener('characteristicvaluechanged', this._onNotify);
    await this._tx.startNotifications();
    this._lines.buf = '';
    this._setStatus('connected', `Bluetooth ${this.device.name || BLE_PREFIX}`);
    this.send('P');
  }

  async _write(line) {
    // Long lines go out in 20-byte pieces; the board's line reader joins them.
    const data = encoder.encode(line);
    const rx = this._rx;
    const noResp = rx.properties.writeWithoutResponse && rx.writeValueWithoutResponse;
    for (let i = 0; i < data.length; i += BLE_CHUNK) {
      const piece = data.subarray(i, i + BLE_CHUNK);
      if (noResp) await rx.writeValueWithoutResponse(piece);
      else await rx.writeValue(piece);
    }
  }

  async _handleDrop() {
    this._clearQueue();
    this._rx = this._tx = null;
    if (this._userClosed || !this.device) { if (this.state !== 'error') this._setStatus('disconnected', ''); return; }
    if (this.state === 'reconnecting') return; // loop already running
    const delays = [500, 1000, 2000, 4000, 8000];
    for (let i = 0; i < delays.length; i++) {
      this._setStatus('reconnecting', `Bluetooth lost, retry ${i + 1}/${delays.length}`);
      await new Promise((r) => setTimeout(r, delays[i]));
      if (this._userClosed || !this.device) return;
      try { await this._attach(); return; } catch (e) { console.warn('[ble] reconnect failed:', e?.message || e); }
    }
    this._setStatus('disconnected', 'Bluetooth lost (gave up; click Connect Bluetooth)');
  }

  async disconnect() {
    this._userClosed = true;
    this._clearQueue();
    try { if (this.device?.gatt?.connected) this.device.gatt.disconnect(); } catch {}
    this._rx = this._tx = null;
    this._setStatus('disconnected', '');
  }
}
