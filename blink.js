// blink.js — spots your blinks in the camera's eye-closed score (0 = wide open, 1 = shut).
//
// Everyone's "open" score is different (glasses, lighting, looking down at the screen all push it
// up), so it doesn't use one fixed line. A blink is the score jumping up QUICKLY: at least a margin
// above the lowest it was in the last third of a second. Eyes close in ~50-100 ms when you blink;
// a squint or looking down takes far longer, so those don't count.
//
// Sensitivity 0..1 sets the margin: 1 = the tiniest quick blink counts, 0 = only a big, full one.
// The default is extra sensitive.

export const DEFAULT_SENSITIVITY = 0.9;
export const SENSITIVITY_LABELS = [   // [from, label] for the slider readout
  [0, 'relaxed'], [0.35, 'normal'], [0.6, 'sensitive'], [0.8, 'extra sensitive'],
];
export const sensitivityLabel = (v) => SENSITIVITY_LABELS.filter(([from]) => v >= from).pop()[1];

const MARGIN_MIN = 0.08;     // at sensitivity 1: a jump this big = a blink
const MARGIN_MAX = 0.45;     // at sensitivity 0
const WINDOW_MS = 330;       // "quickly" = within this long
const REARM = 0.5;           // eyes count as open again below (score before the blink) + REARM × margin
const MAX_CLOSED_MS = 1500;  // shut longer than this: it was one blink, and now that's just how they look

export class BlinkDetector {
  constructor(sensitivity = DEFAULT_SENSITIVITY) {
    this.sensitivity = sensitivity;
    this.hist = [];          // recent [t, score] while open
    this.closed = false;     // eyes shut right now (a blink is in progress)
    this.closedAt = 0;
    this.openLevel = 0;      // score just before the blink started
    this.count = 0;
  }

  get margin() {
    const s = Math.min(1, Math.max(0, this.sensitivity));
    return MARGIN_MAX + (MARGIN_MIN - MARGIN_MAX) * s;
  }
  /** The lowest score in the last WINDOW_MS (how open your eyes just were). */
  get low() {
    let lo = Infinity;
    for (const [, v] of this.hist) if (v < lo) lo = v;
    return lo === Infinity ? 0 : lo;
  }
  /** The score that counts as a blink right now (for a meter). */
  get threshold() { return Math.min(1, (this.closed ? this.openLevel : this.low) + this.margin); }

  /**
   * One camera frame. `face` = a face was found, `score` = its eye-closed score (0..1), `t` in ms.
   * Returns true on the frame your eyes close (once per blink).
   */
  update(face, score, t) {
    if (!face) { this.closed = false; this.hist.length = 0; return false; }
    const margin = this.margin;

    if (this.closed) {
      if (score < this.openLevel + REARM * margin || t - this.closedAt > MAX_CLOSED_MS) {
        this.closed = false;
        this.hist = [[t, score]];
      }
      return false;
    }

    while (this.hist.length && t - this.hist[0][0] > WINDOW_MS) this.hist.shift();
    const lo = this.hist.length ? this.low : score;
    this.hist.push([t, score]);
    if (score - lo >= margin) {
      this.closed = true;
      this.closedAt = t;
      this.openLevel = lo;
      this.count++;
      return true;
    }
    return false;
  }
}
