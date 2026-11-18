/*
 * M1D1 note reader: Spotify's Basic Pitch (ICASSP 2022 model, nmp.onnx) run with onnxruntime-web.
 *
 * Copyright 2022 Spotify AB
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * ---------------------------------------------------------------------------------------------------------------
 * MODIFIED FILE. Modified by the M1D1 contributors, 2026: ported to JavaScript for onnxruntime-web; frame times
 * from the framing.
 *   - Ported from basic_pitch/inference.py (spotify/basic-pitch@fa5997a): the windowing of run_inference /
 *     get_audio_input / window_audio_file (3840 zeros in front, windows of 43844 samples every 36164) and the
 *     stitching of unwrap_output (15 frames dropped at each window edge, 142 kept).
 *   - Changed: frame times come from that framing, t[k] = ((k div 142) * 36164 + (k mod 142) * 256) / 22050 s,
 *     instead of k * 256 / 22050; the output keeps every frame whose centre lies inside the audio; windows whose
 *     frames are all past the end are not run; only the note activations are returned.
 *   - New (not from Basic Pitch): the resampler to 22050 Hz, the onnxruntime-web session handling and the Web
 *     Worker protocol.
 * This file is part of M1D1 (AGPLv3); the parts above that come from Basic Pitch stay under the Apache License.
 * ---------------------------------------------------------------------------------------------------------------
 *
 * One file, three uses (UMD):
 *   - Node (the corpus harness):   const P = require('./m1d1-pitch.js'); await P.run(ort, session, f32, sr)
 *   - a page:                      window.M1D1Pitch
 *   - a Web Worker's source:       <this file> + 'M1D1Pitch.workerMain(self);' (see workerMain below)
 * The result is the analyser's opts.notes: { note: Float32Array(T * 88), times: Float64Array(T), keys: 88,
 * midi0: 21, T } with note[k * 88 + i] = activation (0..1) of MIDI note 21 + i in frame k, and times[k] in seconds
 * from sample 0 of the samples given (whatever their rate).
 */
(function (root, factory) {
  'use strict';
  const api = factory();
  if (typeof module === 'object' && module && module.exports) module.exports = api;
  else root.M1D1Pitch = api;
})(typeof self !== 'undefined' ? self : typeof window !== 'undefined' ? window : globalThis, function () {
  'use strict';
  const VERSION = 'm1d1-pitch 1 (basic-pitch icassp_2022 nmp.onnx, onnxruntime-web 1.20.1)';
  // basic_pitch/constants.py and inference.py
  const SR = 22050;                          // AUDIO_SAMPLE_RATE
  const FFT_HOP = 256;                       // FFT_HOP
  const N_WIN = SR * 2 - FFT_HOP;            // AUDIO_N_SAMPLES = 43844
  const OVERLAP_FRAMES = 30;                 // DEFAULT_OVERLAPPING_FRAMES
  const OVERLAP = OVERLAP_FRAMES * FFT_HOP;  // 7680
  const PAD = OVERLAP / 2;                   // 3840 zeros in front of the audio
  const WIN_HOP = N_WIN - OVERLAP;           // 36164 samples between windows
  const FRAMES = 172;                        // ANNOTATIONS_N_FRAMES per window
  const EDGE = OVERLAP_FRAMES / 2;           // 15 frames dropped at each window edge
  const KEEP = FRAMES - OVERLAP_FRAMES;      // 142 frames kept per window
  const KEYS = 88, MIDI0 = 21;               // A0 .. C8, one bin per semitone
  const OUT_NOTE = 'StatefulPartitionedCall:1';   // inference.py: note = :1, onset = :2, contour = :0
  const MAX_SEC = 64;                        // the plugin's longest capture; longer input is cut there

  // ---------- frame times from the framing ----------
  // frame k = window (k div 142), kept frame (k mod 142); it is centred on padded sample w * 36164 + (j + 15) * 256,
  // i.e. input sample w * 36164 + j * 256 (the 3840-sample pad is exactly 15 hops)
  const frameSample = k => Math.floor(k / KEEP) * WIN_HOP + (k % KEEP) * FFT_HOP;
  const frameTime = k => frameSample(k) / SR;
  // frames kept for L samples at 22050 Hz: every frame whose centre lies inside the audio (bp_run.py trim "exact")
  function frameCount(L) {
    if (!(L > 0)) return 0;
    const nWin = Math.ceil((L + PAD) / WIN_HOP);            // window_audio_file: range(0, padded length, hop)
    let T = 0;
    while (T < nWin * KEEP && frameSample(T) < L) T++;
    return T;
  }

  // ---------- resampling to 22050 Hz ----------
  // Kaiser-windowed sinc, linear phase, no delay: output sample n sits at input time n * srIn / srOut, so times stay
  // in seconds of the input. Length ceil(L * srOut / srIn), as librosa.resample (Basic Pitch's own loader).
  // Fitted to soxr "HQ" (librosa's default, what Basic Pitch's own loader uses), 24 kHz in: -1.1 dB at 10.4 kHz,
  // -9 at 10.6, -33 at 10.8, -95 at 11.0 (soxr -1.1 / -9.2 / -33.5 / -104). Rational rate ratios (24000 -> 22050 = 147/160,
  // 44100 -> 22050 = 1/2, 48000 -> 22050 = 147/320) use an exact polyphase table; other ratios interpolate one.
  const I0 = v => { let s = 1, t = 1; for (let k = 1; k < 64; k++) { t *= (v / (2 * k)) * (v / (2 * k)); s += t; if (t < 1e-14 * s) break; } return s; };
  function kernelSpec(srIn, srOut, opts) {
    const o = opts || {};
    const ratio = srOut / srIn, nyq = 0.5 * Math.min(1, ratio);           // output Nyquist in cycles per input sample
    const fc = (o.cutoff || 0.957) * nyq;                                 // -6 dB point, cycles per input sample
    const tw = (o.transition || 0.09) * nyq;                              // full transition width
    const atten = o.atten || 140;                                         // stopband attenuation, dB
    const beta = atten > 50 ? 0.1102 * (atten - 8.7) : 0.5842 * Math.pow(atten - 21, 0.4) + 0.07886 * (atten - 21);
    const half = Math.ceil((atten - 8) / (2.285 * 2 * Math.PI * tw) / 2) + 1;   // Kaiser's length formula, per side
    const i0b = I0(beta);
    const h = d => {                                                       // d: distance in input samples
      const r = d / half; if (r <= -1 || r >= 1) return 0;
      const w = I0(beta * Math.sqrt(1 - r * r)) / i0b;
      const s = d === 0 ? 2 * fc : Math.sin(2 * Math.PI * fc * d) / (Math.PI * d);
      return s * w;
    };
    return { ratio, fc, beta, half, h };
  }
  // best p/q with q <= maxQ (continued fractions); exact for integer rates
  function rational(x, maxQ) {
    let h0 = 1, h1 = 0, k0 = 0, k1 = 1, v = x;
    for (let i = 0; i < 40; i++) {
      const a = Math.floor(v), h2 = a * h0 + h1, k2 = a * k0 + k1;
      if (k2 > maxQ) break;
      h1 = h0; h0 = h2; k1 = k0; k0 = k2;
      if (Math.abs(v - a) < 1e-12) break;
      v = 1 / (v - a);
    }
    return { p: h0, q: k0 };
  }
  // onProgress(fraction), when given, is called every 8192 output samples (the Worker reports it to the page)
  function resample(x, srIn, srOut, opts, onProgress) {
    srOut = srOut || SR;
    if (!(srIn > 0) || !(srOut > 0)) throw new Error('bad sample rate ' + srIn);
    if (Math.abs(srIn / srOut - 1) < 1e-12) return x;
    const K = kernelSpec(srIn, srOut, opts), half = K.half, taps = 2 * half + ((4 - (2 * half) % 4) % 4);   // padded with zero taps to a multiple of 4
    const nOut = Math.max(0, Math.ceil(x.length * srOut / srIn - 1e-7)), y = new Float32Array(nOut), L = x.length;
    // output n sits at input position n * srIn / srOut = n * M / P (P/M the rate ratio when it is rational)
    const r = rational(srOut / srIn, 4096), exact = Math.abs(r.p / r.q - srOut / srIn) < 1e-12 * (srOut / srIn);
    if (exact) {
      const P = r.p, M = r.q, tab = new Float32Array(P * taps), norm = new Float64Array(P);
      for (let ph = 0; ph < P; ph++) {                     // phase ph: fractional input position ph / P
        let s = 0;
        for (let j = 0; j < taps; j++) { const v = K.h(j - half + 1 - ph / P); tab[ph * taps + j] = v; s += v; }
        norm[ph] = s;
      }
      // unity gain at DC on every phase (as soxr's passband): divide each phase by its own sum
      for (let ph = 0; ph < P; ph++) { const g = 1 / norm[ph]; for (let j = 0; j < taps; j++) tab[ph * taps + j] *= g; }
      for (let n = 0; n < nOut; n++) {
        if (onProgress && (n & 8191) === 0) onProgress(n / nOut);
        const num = n * M, c = Math.floor(num / P), ph = num - c * P, k0 = c - half + 1, base = ph * taps;
        let acc = 0;
        if (k0 >= 0 && k0 + taps <= L) {         // four partial sums: about a quarter faster in V8
          let a0 = 0, a1 = 0, a2 = 0, a3 = 0;
          for (let j = 0; j < taps; j += 4) { const q = k0 + j, u = base + j; a0 += x[q] * tab[u]; a1 += x[q + 1] * tab[u + 1]; a2 += x[q + 2] * tab[u + 2]; a3 += x[q + 3] * tab[u + 3]; }
          acc = (a0 + a1) + (a2 + a3);
        }
        else { for (let j = 0; j < taps; j++) { const k = k0 + j; if (k >= 0 && k < L) acc += x[k] * tab[base + j]; } }
        y[n] = acc;
      }
      return y;
    }
    // any other ratio: a finely sampled kernel table, linearly interpolated
    const OS = 1024, N = half * OS, tab = new Float64Array(N + 2);
    for (let i = 0; i <= N + 1; i++) tab[i] = K.h(i / OS);
    let dc = 0; for (let k = -half; k <= half; k++) dc += K.h(k);
    const g = 1 / dc, step = srIn / srOut;
    for (let n = 0; n < nOut; n++) {
      if (onProgress && (n & 8191) === 0) onProgress(n / nOut);
      const t = n * step, c = Math.floor(t);
      let acc = 0;
      for (let k = c - half + 1; k <= c + half; k++) {
        if (k < 0 || k >= L) continue;
        const d = Math.abs(t - k) * OS, i = d | 0;
        if (i > N) continue;
        acc += x[k] * (tab[i] + (tab[i + 1] - tab[i]) * (d - i));
      }
      y[n] = acc * g;
    }
    return y;
  }

  // ---------- the model on 22050 Hz audio ----------
  // ort: the onnxruntime-web module; session: an InferenceSession of nmp.onnx; x: Float32Array at 22050 Hz.
  // opts.batch: windows per model call (default 1); opts.onProgress(fraction); opts.cancelled(): true stops;
  // opts.pause(): awaited before each window (the Worker's way to take its messages, a cancel, during a run:
  // onnxruntime-web's single-threaded run never gives the event loop a turn by itself).
  async function transcribe(ort, session, x, opts) {
    const o = opts || {}, batch = Math.max(1, o.batch | 0 || 1), L = x.length;
    const T = frameCount(L), nWin = Math.ceil(T / KEEP);         // windows whose kept frames are all past the end are skipped
    const note = new Float32Array(T * KEYS), inName = session.inputNames[0];
    for (let w0 = 0; w0 < nWin; w0 += batch) {
      if (o.pause) await o.pause();
      if (o.cancelled && o.cancelled()) throw new Error('cancelled');
      const nb = Math.min(batch, nWin - w0), buf = new Float32Array(nb * N_WIN);
      for (let b = 0; b < nb; b++) {                             // window w = padded[w * 36164, + 43844), zeros past the end
        const start = (w0 + b) * WIN_HOP - PAD, from = Math.max(0, start), to = Math.min(L, start + N_WIN);
        if (to > from) buf.set(x.subarray(from, to), b * N_WIN + (from - start));
      }
      const feeds = {}; feeds[inName] = new ort.Tensor('float32', buf, [nb, N_WIN, 1]);
      const out = await session.run(feeds);
      const y = out[OUT_NOTE];
      if (!y || !y.dims || y.dims.length !== 3 || y.dims[0] !== nb || y.dims[1] !== FRAMES || y.dims[2] !== KEYS)
        throw new Error('unexpected model output ' + (y && y.dims ? y.dims.join('x') : typeof y));
      const d = y.data;
      for (let b = 0; b < nb; b++) for (let j = 0; j < KEEP; j++) {      // unwrap_output: drop 15 frames at each edge
        const k = (w0 + b) * KEEP + j; if (k >= T) break;
        const src = (b * FRAMES + EDGE + j) * KEYS;
        note.set(d.subarray(src, src + KEYS), k * KEYS);
      }
      for (const name in out) if (out[name] && typeof out[name].dispose === 'function') out[name].dispose();
      if (o.onProgress) o.onProgress(Math.min(1, (w0 + nb) / nWin));
    }
    const times = new Float64Array(T);
    for (let k = 0; k < T; k++) times[k] = frameTime(k);
    return { note, times, keys: KEYS, midi0: MIDI0, T };
  }

  // samples (any rate, mono) -> opts.notes. Cut at 64 s, resampled to 22050 Hz, run, times in seconds of `samples`.
  // opts: as transcribe's, plus resample (kernel options) and onResample(fraction) while resampling.
  async function run(ort, session, samples, sr, opts) {
    if (!samples || typeof samples.length !== 'number') throw new Error('no audio');
    if (!(sr >= 4000 && sr <= 384000)) throw new Error('bad sample rate ' + sr);
    const n = Math.min(samples.length, Math.floor(MAX_SEC * sr));
    let x = samples instanceof Float32Array ? samples.subarray(0, n) : Float32Array.from(samples.slice(0, n));
    for (let i = 0; i < x.length; i++) if (!Number.isFinite(x[i])) { x = Float32Array.from(x, v => (Number.isFinite(v) ? v : 0)); break; }
    const t0 = now();
    const a = resample(x, sr, SR, opts && opts.resample, opts && opts.onResample);
    const t1 = now();
    const r = await transcribe(ort, session, a, opts);
    const t2 = now();
    r.ms = { resample: Math.round(t1 - t0), model: Math.round(t2 - t1) };
    r.sr = sr; r.seconds = n / sr;
    return r;
  }

  async function createSession(ort, modelBytes) {
    return ort.InferenceSession.create(modelBytes, { executionProviders: ['wasm'], graphOptimizationLevel: 'all' });
  }
  const now = () => (typeof performance !== 'undefined' && performance.now ? performance.now() : Date.now());

  // ---------- the Web Worker ----------
  // The page makes the worker from a blob URL of this file plus 'M1D1Pitch.workerMain(self);'. Messages:
  //   in  { type: 'init', ort, mjs, wasm, model }   each a gzip + base64 string: ort.wasm.min.js, its .mjs glue,
  //                                                 ort-wasm-simd-threaded.wasm, nmp.onnx (onnxruntime-web 1.20.1)
  //   in  { type: 'run', id, audio: Float32Array (transferred), sr }   runs are done one at a time, in order
  //   in  { type: 'cancel', id }                    that run is no longer wanted: it stops before its next model
  //                                                 window (or does not start), and answers 'cancelled'
  //   out { type: 'hello' }                         at once: the worker's script is running
  //   out { type: 'ready', ms }                     the reader is set up (decoded, model compiled)
  //   out { type: 'progress', id, stage: 'start' | 'resample' | 'model', done }   while a run goes on: at its start,
  //                                                 at least every 250 ms of resampling, after every model window
  //   out { type: 'result', id, note, times (ArrayBuffers, transferred), T, keys, midi0, ms }
  //   out { type: 'cancelled', id }  /  { type: 'error', id, stage: 'init' | 'run', message }
  // ORT loads from blob URLs: env.wasm.wasmPaths = { mjs, wasm } (env.wasm.wasmBinary fails when the glue itself
  // comes from a blob URL in 1.20.1), one thread (the page is not cross-origin isolated).
  function workerMain(scope) {
    let ready = null, ortMod = null, session = null, chain = Promise.resolve();
    const pending = new Set(), cancelled = new Set();     // runs received and not answered yet; those cancelled
    const unb64 = s => { const bin = atob(s), u8 = new Uint8Array(bin.length); for (let i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i); return u8; };
    const gunzip = async s => new Uint8Array(await new Response(new Blob([unb64(s)]).stream().pipeThrough(new DecompressionStream('gzip'))).arrayBuffer());
    const blobUrl = (bytes, type) => URL.createObjectURL(new Blob([bytes], { type }));
    async function init(m) {
      const t0 = now();
      if (!m.ort || !m.mjs || !m.wasm || !m.model) throw new Error('the note reader is not in this page');
      const [js, mjs, wasm, model] = await Promise.all([m.ort, m.mjs, m.wasm, m.model].map(gunzip));
      const t1 = now();
      const urls = [blobUrl(js, 'text/javascript'), blobUrl(mjs, 'text/javascript'), blobUrl(wasm, 'application/wasm')];
      scope.importScripts(urls[0]);
      ortMod = scope.ort;
      if (!ortMod || !ortMod.InferenceSession) throw new Error('onnxruntime-web did not load');
      ortMod.env.logLevel = 'error';
      ortMod.env.wasm.numThreads = 1;
      ortMod.env.wasm.proxy = false;
      ortMod.env.wasm.wasmPaths = { mjs: urls[1], wasm: urls[2] };
      session = await createSession(ortMod, model);
      urls.forEach(u => URL.revokeObjectURL(u));        // loaded and compiled: the 11 MB of blobs can go
      const ms = { decode: Math.round(t1 - t0), session: Math.round(now() - t1) };
      scope.postMessage({ type: 'ready', ms, version: VERSION, ort: ortMod.env.versions && ortMod.env.versions.web });
      return ms;
    }
    async function runOne(m) {
      let stage = 'init';
      const isCancelled = () => cancelled.has(m.id);
      try {
        if (!ready) throw new Error('not initialised');
        const initMs = await ready;
        stage = 'run';
        if (isCancelled()) throw new Error('cancelled');
        let last = 0;
        const progress = (st, done) => { const t = now(); if (st === 'resample' && t - last < 250) return; last = t; scope.postMessage({ type: 'progress', id: m.id, stage: st, done }); };
        progress('start', 0);
        const r = await run(ortMod, session, m.audio, m.sr, { batch: m.batch || 1, cancelled: isCancelled, pause: () => new Promise(res => setTimeout(res, 0)),
          onResample: f => progress('resample', f), onProgress: f => progress('model', f) });
        r.ms.init = initMs;
        scope.postMessage({ type: 'result', id: m.id, note: r.note.buffer, times: r.times.buffer, T: r.T, keys: r.keys, midi0: r.midi0, ms: r.ms },
          [r.note.buffer, r.times.buffer]);
      } catch (e) {
        if (stage === 'run' && isCancelled()) scope.postMessage({ type: 'cancelled', id: m.id });
        else scope.postMessage({ type: 'error', id: m.id, stage, message: String((e && e.message) || e) });
      } finally { pending.delete(m.id); cancelled.delete(m.id); }
    }
    scope.onmessage = ev => {
      const m = ev.data || {};
      if (m.type === 'init') { if (!ready) { ready = init(m); ready.catch(() => {}); } return; }
      if (m.type === 'cancel') { if (pending.has(m.id)) cancelled.add(m.id); return; }
      if (m.type !== 'run') return;
      pending.add(m.id); chain = chain.then(() => runOne(m));
    };
    scope.postMessage({ type: 'hello', version: VERSION });
  }

  return Object.freeze({ VERSION, SR, FFT_HOP, N_WIN, PAD, WIN_HOP, FRAMES, EDGE, KEEP, KEYS, MIDI0, MAX_SEC, OUT_NOTE,
    frameSample, frameTime, frameCount, kernelSpec, resample, transcribe, run, createSession, workerMain });
});
