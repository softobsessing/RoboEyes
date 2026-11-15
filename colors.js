// colors.js — the Colors card: pick the screen and eye colors with a color wheel + brightness
// slider (or a hex code), and keep a list of saved color pairs. No board/link code in here:
// app.js passes callbacks and decides what to do with the colors.

export const DEFAULT_COLORS = { face: 0xFFFFFF, eye: 0x000000 };   // white face, black eyes
export const MAX_SAVED = 8;

export const hex6 = (n) => (n & 0xFFFFFF).toString(16).padStart(6, '0').toUpperCase();
export function parseHex(text) {
  const m = /^#?([0-9a-f]{6}|[0-9a-f]{3})$/i.exec(String(text).trim());
  if (!m) return null;
  let h = m[1];
  if (h.length === 3) h = h.replace(/./g, (c) => c + c);
  return parseInt(h, 16);
}

export function hsvToRgb(h, s, v) {   // h 0..360, s/v 0..1 -> 0xRRGGBB
  const f = (n) => {
    const k = (n + h / 60) % 6;
    return v - v * s * Math.max(0, Math.min(k, 4 - k, 1));
  };
  const to8 = (x) => Math.round(x * 255);
  return (to8(f(5)) << 16) | (to8(f(3)) << 8) | to8(f(1));
}
export function rgbToHsv(c) {   // 0xRRGGBB -> {h, s, v}
  const r = ((c >> 16) & 255) / 255, g = ((c >> 8) & 255) / 255, b = (c & 255) / 255;
  const max = Math.max(r, g, b), min = Math.min(r, g, b), d = max - min;
  let h = 0;
  if (d) {
    if (max === r) h = 60 * (((g - b) / d) % 6);
    else if (max === g) h = 60 * ((b - r) / d + 2);
    else h = 60 * ((r - g) / d + 4);
  }
  if (h < 0) h += 360;
  return { h, s: max ? d / max : 0, v: max };
}

const css = (c) => `#${hex6(c)}`;

export class ColorPicker {
  /**
   * @param {object} o
   * @param {{face:number, eye:number}} o.colors   starting colors
   * @param {{face:number, eye:number}[]} o.saved  saved pairs (newest first)
   * @param {(colors:{face:number, eye:number}) => void} o.onChange      every change, live
   * @param {(saved:{face:number, eye:number}[]) => void} o.onSavedChange
   * @param {'eye'|'face'} [o.target]  which color the wheel edits first
   */
  constructor({ colors, saved, onChange, onSavedChange, target = 'eye' }) {
    const $ = (id) => document.getElementById(id);
    this.el = {
      wheel: $('wheel'), bright: $('bright'), hex: $('hex'), saved: $('savedColors'),
      swEye: $('swEye'), swFace: $('swFace'), save: $('btnSaveColors'), reset: $('btnResetColors'),
      targets: [...document.querySelectorAll('#colorTarget button')],
    };
    this.colors = { ...colors };
    this.saved = saved.slice(0, MAX_SAVED);
    this.onChange = onChange;
    this.onSavedChange = onSavedChange;
    this.target = target;
    this.hsv = rgbToHsv(this.colors[this.target]);
    this.ctx = this.el.wheel.getContext('2d');
    this.size = this.el.wheel.width;
    this.radius = this.size / 2 - 4;
    this.wheelV = -1;

    for (const b of this.el.targets) b.onclick = () => this.selectTarget(b.dataset.target);
    this.el.bright.oninput = () => { this.hsv.v = this.el.bright.value / 100; this.applyHsv(); };
    this.el.hex.onchange = () => {
      const c = parseHex(this.el.hex.value);
      if (c === null) { this.renderControls(); return; }
      this.setTargetColor(c, true);
    };
    this.el.save.onclick = () => this.saveCurrent();
    this.el.reset.onclick = () => this.setColors(DEFAULT_COLORS);

    // drag on the wheel: hue = angle, saturation = distance from the center
    const pick = (ev) => {
      const r = this.el.wheel.getBoundingClientRect();
      const x = (ev.clientX - r.left) * (this.size / r.width) - this.size / 2;
      const y = (ev.clientY - r.top) * (this.size / r.height) - this.size / 2;
      let h = Math.atan2(y, x) * 180 / Math.PI;
      if (h < 0) h += 360;
      this.hsv.h = h;
      this.hsv.s = Math.min(1, Math.hypot(x, y) / this.radius);
      if (this.hsv.v < 0.05) { this.hsv.v = 1; this.el.bright.value = 100; }   // picking on a black wheel: light it up
      this.applyHsv();
    };
    this.el.wheel.onpointerdown = (ev) => { this.el.wheel.setPointerCapture(ev.pointerId); this.dragging = true; pick(ev); };
    this.el.wheel.onpointermove = (ev) => { if (this.dragging) pick(ev); };
    this.el.wheel.onpointerup = this.el.wheel.onpointercancel = () => { this.dragging = false; };

    this.renderControls();
    this.renderSaved();
  }

  selectTarget(t) {
    this.target = t;
    this.hsv = rgbToHsv(this.colors[t]);
    this.renderControls();
  }

  // replace both colors (reset / a saved pair). silent: just show them (they came from the board).
  setColors(c, { silent = false } = {}) {
    this.colors = { face: c.face, eye: c.eye };
    this.hsv = rgbToHsv(this.colors[this.target]);
    this.renderControls();
    if (!silent) this.onChange({ ...this.colors });
  }

  setTargetColor(c, fromHex = false) {
    this.colors[this.target] = c;
    if (fromHex) this.hsv = rgbToHsv(c);
    this.renderControls();
    this.onChange({ ...this.colors });
  }

  applyHsv() {
    this.colors[this.target] = hsvToRgb(this.hsv.h, this.hsv.s, this.hsv.v);
    this.renderControls();
    this.onChange({ ...this.colors });
  }

  saveCurrent() {
    const { face, eye } = this.colors;
    this.saved = [{ face, eye }, ...this.saved.filter((p) => p.face !== face || p.eye !== eye)].slice(0, MAX_SAVED);
    this.renderSaved();
    this.onSavedChange(this.saved);
  }

  renderControls() {
    const c = this.colors[this.target];
    for (const b of this.el.targets) b.setAttribute('aria-pressed', String(b.dataset.target === this.target));
    this.el.swEye.style.background = css(this.colors.eye);
    this.el.swFace.style.background = css(this.colors.face);
    this.el.bright.value = Math.round(this.hsv.v * 100);
    if (document.activeElement !== this.el.hex) this.el.hex.value = css(c);
    this.drawWheel();
  }

  drawWheel() {
    const { size, radius } = this;
    if (this.wheelV !== this.hsv.v) {   // rebuild the wheel image only when brightness changes
      this.wheelV = this.hsv.v;
      const img = this.ctx.createImageData(size, size), d = img.data;
      for (let y = 0; y < size; y++) for (let x = 0; x < size; x++) {
        const dx = x + 0.5 - size / 2, dy = y + 0.5 - size / 2, r = Math.hypot(dx, dy);
        if (r > radius + 1) continue;
        let h = Math.atan2(dy, dx) * 180 / Math.PI;
        if (h < 0) h += 360;
        const c = hsvToRgb(h, Math.min(1, r / radius), this.hsv.v), i = (y * size + x) * 4;
        d[i] = c >> 16; d[i + 1] = (c >> 8) & 255; d[i + 2] = c & 255;
        d[i + 3] = Math.round(255 * Math.min(1, Math.max(0, radius + 1 - r)));   // soft edge
      }
      this.wheelImg = img;
    }
    this.ctx.putImageData(this.wheelImg, 0, 0);
    // marker
    const a = this.hsv.h * Math.PI / 180, r = this.hsv.s * radius;
    const mx = size / 2 + Math.cos(a) * r, my = size / 2 + Math.sin(a) * r;
    this.ctx.lineWidth = 3;
    this.ctx.strokeStyle = this.hsv.v > 0.55 ? '#1d1d1f' : '#fff';
    this.ctx.beginPath(); this.ctx.arc(mx, my, 8, 0, Math.PI * 2); this.ctx.stroke();
  }

  renderSaved() {
    const box = this.el.saved;
    box.textContent = '';
    if (!this.saved.length) {
      const p = document.createElement('span');
      p.className = 'muted small';
      p.textContent = 'Nothing saved yet. Pick colors and press "Save these colors".';
      box.append(p);
      return;
    }
    this.saved.forEach((pair, i) => {
      const item = document.createElement('div');
      item.className = 'saved-item';
      const apply = document.createElement('button');
      apply.type = 'button';
      apply.className = 'saved-face';
      apply.title = `face ${css(pair.face)} · eyes ${css(pair.eye)}`;
      apply.style.background = css(pair.face);
      for (let k = 0; k < 2; k++) {
        const eye = document.createElement('span');
        eye.style.background = css(pair.eye);
        apply.append(eye);
      }
      apply.onclick = () => this.setColors(pair);
      const del = document.createElement('button');
      del.type = 'button';
      del.className = 'saved-del';
      del.title = 'Remove';
      del.setAttribute('aria-label', 'Remove saved colors');
      del.textContent = '×';
      del.onclick = () => {
        this.saved.splice(i, 1);
        this.renderSaved();
        this.onSavedChange(this.saved);
      };
      item.append(apply, del);
      box.append(item);
    });
  }
}
