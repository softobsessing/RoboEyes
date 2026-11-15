// vision.js — webcam + MediaPipe FaceLandmarker (vendored, runs fully in the browser), reduced to
// what "blink with me" needs: is there a face, and how closed are its eyes.
// Nothing leaves the page: frames go from <video> straight into the local wasm/WebGL model.
// (Same setup as vision.js in the doomscroll-eyes app.)
import { FaceLandmarker, FilesetResolver } from './vendor/tasks-vision/vision_bundle.mjs';

const WASM_DIR = new URL('./vendor/tasks-vision/wasm', import.meta.url).href;
const MODEL_URL = new URL('./vendor/face_landmarker.task', import.meta.url).href;

export class Vision {
  constructor() {
    this.landmarker = null;
    this.delegate = null;
    this.stream = null;
    this._fileset = null;
    this._lastTs = 0;
  }

  /** Load wasm + model. Tries the GPU delegate first and falls back to CPU. */
  async init() {
    this._fileset = await FilesetResolver.forVisionTasks(WASM_DIR);
    try {
      await this._create('GPU');
    } catch (e) {
      console.warn('[vision] GPU delegate unavailable, using CPU:', e?.message || e);
      await this._create('CPU');
    }
    return this.delegate;
  }

  async _create(delegate) {
    this.landmarker = await FaceLandmarker.createFromOptions(this._fileset, {
      baseOptions: { modelAssetPath: MODEL_URL, delegate },
      runningMode: 'VIDEO',
      numFaces: 1,
      outputFaceBlendshapes: true,
    });
    this.delegate = delegate;
  }

  /** A high frame rate helps catch quick blinks (a blink is only ~100–150 ms). */
  async startCamera(video) {
    this.stream = await navigator.mediaDevices.getUserMedia({
      video: { width: { ideal: 640 }, height: { ideal: 480 }, frameRate: { ideal: 60 }, facingMode: 'user' },
      audio: false,
    });
    video.srcObject = this.stream;
    await video.play();
  }

  stopCamera(video) {
    this.stream?.getTracks().forEach((t) => t.stop());
    this.stream = null;
    if (video) video.srcObject = null;
  }

  /** Run the model on the current video frame: {face, blink (0 open .. 1 shut)}. */
  detect(video, nowMs) {
    const ts = Math.max(nowMs, this._lastTs + 1);   // MediaPipe needs strictly increasing timestamps
    this._lastTs = ts;
    let result;
    try {
      result = this.landmarker.detectForVideo(video, ts);
    } catch (e) {
      if (this.delegate === 'GPU') {   // some GPUs fail only at first inference
        console.warn('[vision] GPU inference failed, switching to CPU:', e?.message || e);
        const old = this.landmarker;
        this.landmarker = null;
        try { old.close(); } catch {}
        this._create('CPU').catch((err) => console.error(err));
      }
      throw e;
    }
    return eyeSignals(result);
  }

  get ready() { return !!this.landmarker; }
}

/** {face, blink}: blink = average of MediaPipe's eyeBlinkLeft / eyeBlinkRight blendshapes. */
export function eyeSignals(result) {
  const cats = result?.faceBlendshapes?.[0]?.categories;
  if (!result?.faceLandmarks?.[0]?.length || !cats) return { face: false, blink: 0 };
  let l = 0, r = 0;
  for (const c of cats) {
    if (c.categoryName === 'eyeBlinkLeft') l = c.score;
    else if (c.categoryName === 'eyeBlinkRight') r = c.score;
  }
  return { face: true, blink: (l + r) / 2 };
}
