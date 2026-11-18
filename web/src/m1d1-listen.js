// M1D1Listen: listens to a loaded audio file and reports tempo, first downbeat, key and a 4-bar chord
// progression in the preview's spec format. A JavaScript port of the plugin's C++ analysis engine
// (core/Analysis.cpp estimateTempo + detectKey, core/Timing.cpp analyseBeats + the chord Tracker, core/Fft.h),
// with three things the plugin gets from its host and the preview has to hear for itself: where the beats
// fall, which beat starts the bar, and which of the preview's seven scales the chords belong to.
// Optionally, opts.notes (a note transcription of the same audio: Spotify's Basic Pitch, run by the page) sharpens what
// the chords are read from; without it, or when it is unusable, the spectrum is read alone, exactly as before.
// A read that holds nothing (no audio, silence, samples that are not numbers, too short, no pitch) comes back with failed:
// true, failReason (a short code), no chords (an empty spec) and every confidence 0.
// Pure and deterministic: no DOM, no Math.random, no state between calls.
(function (root) {
  'use strict';

  const SCALES = [
    { name: 'Minor', st: [0, 2, 3, 5, 7, 8, 10] },
    { name: 'Major', st: [0, 2, 4, 5, 7, 9, 11] },
    { name: 'Dorian', st: [0, 2, 3, 5, 7, 9, 10] },
    { name: 'Phrygian', st: [0, 1, 3, 5, 7, 8, 10] },
    { name: 'Lydian', st: [0, 2, 4, 6, 7, 9, 11] },
    { name: 'Mixolydian', st: [0, 2, 4, 5, 7, 9, 10] },
    { name: 'Harmonic minor', st: [0, 2, 3, 5, 7, 8, 11] },
  ];
  const NOTE = ['C', 'C#', 'D', 'Eb', 'E', 'F', 'F#', 'G', 'Ab', 'A', 'Bb', 'B'];
  const MIN_TEMPO = 70, MAX_TEMPO = 180;               // Backline.h minTempo / maxTempo
  const ONSET_LATENCY = 0.75;                          // how far into its analysis window an onset is when a frame first hears it
  const mod = (a, n) => ((a % n) + n) % n;
  const clamp01 = x => (x > 1 ? 1 : x > 0 ? x : 0);

  // The chord of one spec element: the generator's own function when it is loaded, else the same maths.
  function localChordTones(s, key) {
    const sc = key.st, d = mod(Math.round(Number(s.d) || 0), 7);
    const deg = k => sc[(d + k) % 7] + 12 * Math.floor((d + k) / 7);
    let iv;
    if (s.q === 'dom7') iv = [0, 4, 7, 10]; else if (s.q === 'sus4') iv = [0, 5, 7]; else if (s.q === 'maj') iv = [0, 4, 7];
    else { const r0 = deg(0); iv = [0, deg(2) - r0, deg(4) - r0]; if (s.ext === 7) iv.push(deg(6) - r0); }
    const rootPc = (key.root + sc[d]) % 12;
    let notes = iv.map(x => 48 + rootPc + x);
    if (s.inv === 1) notes = [...notes.slice(1), notes[0] + 12];
    else if (s.inv === 2) notes = [...notes.slice(2), notes[0] + 12, notes[1] + 12];
    const tones = { root: rootPc, third: (rootPc + iv[1]) % 12, fifth: (rootPc + iv[2]) % 12 };
    if (iv[3] !== undefined) tones.seventh = (rootPc + iv[3]) % 12;
    return { pcs: notes.map(n => n % 12), notes, tones };
  }
  function chordTonesOf(opts) {
    if (opts && opts.gen && typeof opts.gen.chordTones === 'function') return opts.gen.chordTones;
    try { if (typeof M1D1Gen !== 'undefined' && M1D1Gen && typeof M1D1Gen.chordTones === 'function') return M1D1Gen.chordTones; } catch (e) { /* not loaded */ }
    return localChordTones;
  }

  // ---------- FFT (Fft.h: in-place radix 2), with the twiddles of each size kept ----------
  const fftTables = new Map();
  function fft(re, im) {
    const n = re.length;
    let tab = fftTables.get(n);
    if (!tab) {
      tab = { cos: new Float64Array(n / 2), sin: new Float64Array(n / 2) };
      for (let i = 0; i < n / 2; i++) { tab.cos[i] = Math.cos(-2 * Math.PI * i / n); tab.sin[i] = Math.sin(-2 * Math.PI * i / n); }
      fftTables.set(n, tab);
    }
    for (let i = 1, j = 0; i < n; i++) {
      let bit = n >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) { const tr = re[i]; re[i] = re[j]; re[j] = tr; const ti = im[i]; im[i] = im[j]; im[j] = ti; }
    }
    for (let len = 2; len <= n; len <<= 1) {
      const half = len >> 1, step = n / len;
      for (let i = 0; i < n; i += len)
        for (let k = 0, t = 0; k < half; k++, t += step) {
          const wr = tab.cos[t], wi = tab.sin[t], a = i + k, b = a + half;
          const vr = re[b] * wr - im[b] * wi, vi = re[b] * wi + im[b] * wr;
          re[b] = re[a] - vr; im[b] = im[a] - vi; re[a] += vr; im[a] += vi;
        }
    }
  }

  // Low-pass (windowed sinc) and keep every `factor`-th sample.
  function decimate(x, n, factor) {
    if (factor <= 1) return x.length === n ? x : x.subarray(0, n);
    const half = 8 * factor, taps = new Float64Array(2 * half + 1), fc = 0.45 / factor;
    let sum = 0;
    for (let i = -half; i <= half; i++) {
      const s = i === 0 ? 2 * fc : Math.sin(2 * Math.PI * fc * i) / (Math.PI * i);
      const w = 0.54 + 0.46 * Math.cos(Math.PI * i / half);
      taps[i + half] = s * w; sum += s * w;
    }
    for (let i = 0; i < taps.length; i++) taps[i] /= sum;
    const out = new Float32Array(Math.floor(n / factor));
    for (let o = 0; o < out.length; o++) {
      const c = o * factor;
      let acc = 0;
      const lo = Math.max(0, c - half), hi = Math.min(n - 1, c + half);
      for (let i = lo; i <= hi; i++) acc += x[i] * taps[i - c + half];
      out[o] = acc;
    }
    return out;
  }

  // ---------- tempo (Analysis.cpp estimateTempo) and where the beats fall ----------
  // Onset strength 200 times a second: how much louder each part of the spectrum got, in whole and in three
  // bands (the kick's, the instruments', the cymbals').
  function onsetEnvelopes(x, sr) {
    const hop = Math.max(1, Math.round(sr / 200)), frameRate = sr / hop;
    let n = 256; while (n < sr * 0.02) n <<= 1;
    if (x.length < sr * 2 || x.length < n) return null;
    let peak = 0;
    for (let i = 0; i < x.length; i++) { const a = Math.abs(x[i]); if (a > peak) peak = a; }
    if (!(peak > 1e-6)) return null;
    const gain = 1 / peak, win = new Float64Array(n);
    for (let i = 0; i < n; i++) win[i] = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / n);
    const numFrames = Math.floor((x.length - n) / hop) + 1, numBins = n / 2, compression = 1000 / n;
    const re = new Float64Array(n), im = new Float64Array(n);
    let prev = new Float64Array(numBins), cur = new Float64Array(numBins);
    const full = new Float64Array(numFrames), low = new Float64Array(numFrames), mid = new Float64Array(numFrames), high = new Float64Array(numFrames);
    const lowTop = Math.max(2, Math.round(200 * n / sr)), midTop = Math.round(2500 * n / sr);
    for (let f = 0; f < numFrames; f++) {
      const start = f * hop;
      for (let i = 0; i < n; i++) { re[i] = x[start + i] * gain * win[i]; im[i] = 0; }
      fft(re, im);
      let fl = 0, a = 0, b = 0, c = 0;
      for (let k = 1; k < numBins; k++) {
        cur[k] = Math.log(1 + compression * Math.sqrt(re[k] * re[k] + im[k] * im[k]));
        const d = cur[k] - prev[k];
        if (d > 0) { fl += d; if (k <= lowTop) a += d; else if (k <= midTop) b += d; else c += d; }
      }
      if (f > 0) { full[f] = fl; low[f] = a; mid[f] = b; high[f] = c; }
      const t = prev; prev = cur; cur = t;
    }
    return { full, low, mid, high, hop, n, frameRate, numFrames, numBins };
  }
  // Only real onsets count; then a little smoothing so a click is a few frames wide.
  function shapeEnvelope(raw, floorOf) {
    let strongest = 0;
    for (let i = 0; i < raw.length; i++) if (raw[i] > strongest) strongest = raw[i];
    const out = new Float64Array(raw.length);
    if (!(strongest > floorOf)) return { env: out, strongest: 0 };
    const cut = 0.08 * strongest, at = i => (i < 0 || i >= raw.length ? 0 : Math.max(0, raw[i] - cut));
    for (let i = 0; i < raw.length; i++) out[i] = 0.25 * at(i - 1) + 0.5 * at(i) + 0.25 * at(i + 1);
    return { env: out, strongest };
  }
  const lerpAt = (a, pos) => { const i = Math.floor(pos); if (i < 0 || i + 1 >= a.length) return 0; const f = pos - i; return a[i] * (1 - f) + a[i + 1] * f; };

  function estimateTempo(on) {
    const { frameRate, numFrames, numBins } = on;
    const shaped = shapeEnvelope(on.full, 0.01 * numBins);
    if (!shaped.strongest) return null;                       // nothing ever starts
    const env = Float64Array.from(shaped.env);
    let mean = 0; for (let i = 0; i < numFrames; i++) mean += env[i];
    mean /= numFrames; for (let i = 0; i < numFrames; i++) env[i] -= mean;

    const shortest = frameRate * 60 / MAX_TEMPO, longest = frameRate * 60 / MIN_TEMPO;
    const maxLag = Math.min(Math.floor(8 * longest) + 2, Math.floor(numFrames / 2));
    if (maxLag < longest + 2) return null;
    const acf = new Float64Array(maxLag + 1);
    for (let lag = 0; lag <= maxLag; lag++) {
      let s = 0; for (let f = 0; f + lag < numFrames; f++) s += env[f] * env[f + lag];
      acf[lag] = s / (numFrames - lag);
    }
    if (!(acf[0] > 1e-12)) return null;
    // How regular the onsets are at a beat period: the autocorrelation at its first `beats` multiples.
    const comb = (period, beats) => {
      let s = 0, used = 0;
      for (let b = 1; b <= beats; b++) {
        const lag = period * b, below = Math.floor(lag);
        if (below + 1 > maxLag) break;
        const fr = lag - below; s += acf[below] * (1 - fr) + acf[below + 1] * fr; used++;
      }
      return used ? s / used / acf[0] : 0;
    };
    const score = period => { const oct = Math.log2(frameRate * 60 / period / 120); return comb(period, 8) * Math.exp(-0.5 * oct * oct / 0.6); };
    const refine = (centre, reach) => {
      const beats = Math.min(16, Math.max(4, Math.floor((maxLag - 1) / (centre + reach))));
      let best = centre, bestValue = comb(centre, beats);
      for (let width = reach; width > 0.005; width *= 0.2) {
        const middle = best;
        for (let k = -5; k <= 5; k++) {
          const p = middle + k * width * 0.2;
          if (p < shortest || p > longest) continue;
          const v = comb(p, beats); if (v > bestValue) { bestValue = v; best = p; }
        }
      }
      return best;
    };
    const step = 0.125;
    let bestPeriod = 0, bestScore = 0;
    for (let p = shortest; p <= longest + 1e-9; p += step) { const s = score(p); if (s > bestScore) { bestScore = s; bestPeriod = p; } }
    const slowest = score(longest); if (slowest > bestScore) { bestScore = slowest; bestPeriod = longest; }
    if (bestScore < Math.max(0.04, 3 / Math.sqrt(numFrames))) return null;   // nothing repeats
    bestPeriod = refine(bestPeriod, step);

    // A fast waltz can win as one and a half beats: when onsets two thirds of that apart are regular over
    // three steps, they are the beats.
    const twoThirds = bestPeriod * 2 / 3;
    if (twoThirds >= shortest * 0.98) {
      const tp = refine(Math.max(twoThirds, shortest), 1);
      if (comb(tp, 3) > 0.9 * comb(bestPeriod, 2)) bestPeriod = tp;
    }
    // When the onsets half a beat apart are just as regular as the beats themselves, they are the beats.
    const half = bestPeriod * 0.5;
    if (half >= shortest * 0.98) {
      const hp = refine(Math.max(half, shortest), 1), nearest = Math.round(hp);
      let between = 0;
      for (let lag = nearest - 1; lag <= nearest + 1; lag++) between = Math.max(between, acf[lag]);
      if (between / acf[0] > 0.9 * comb(bestPeriod, 2)) bestPeriod = hp;
    }
    return { period: bestPeriod, comb, refine, shortest, longest, regularity: comb(bestPeriod, 4) };
  }

  // Where the beats fall for a period (in envelope frames). Each band votes with how sharply its onsets
  // line up; the kick's band counts most, since a hat on the off-beats is as regular as the beat itself.
  function beatPhase(bands, period) {
    const steps = Math.max(8, Math.round(period * 4)), total = new Float64Array(steps);
    const weights = [1.0, 0.6, 0.3];
    bands.forEach((env, b) => {
      const fold = new Float64Array(steps);
      let mean = 0;
      for (let s = 0; s < steps; s++) {
        let acc = 0;
        for (let pos = s * period / steps; pos < env.length - 1; pos += period) acc += lerpAt(env, pos);
        fold[s] = acc; mean += acc;
      }
      mean /= steps;
      if (!(mean > 1e-9)) return;
      for (let s = 0; s < steps; s++) total[s] += weights[b] * fold[s] / mean;
    });
    let best = 0;
    for (let s = 1; s < steps; s++) if (total[s] > total[best]) best = s;
    // between the steps, by a parabola through the three around the best
    const a = total[(best + steps - 1) % steps], b0 = total[best], c = total[(best + 1) % steps], den = a - 2 * b0 + c;
    const off = den < 0 ? 0.5 * (a - c) / den : 0;
    return mod((best + off) * period / steps, period);
  }
  // Mean onset strength on the even and on the odd beats of a grid, above what the envelope holds anywhere
  // (a steady bass note leaves a ripple in its band that is no onset).
  function parity(env, period, phase) {
    const local = new Float64Array(env.length);
    for (let i = 0; i < env.length; i++) { let m = 0; for (let j = Math.max(0, i - 2); j <= i + 2 && j < env.length; j++) if (env[j] > m) m = env[j]; local[i] = m; }
    const sorted = Float64Array.from(local).sort(), base = sorted[sorted.length >> 1];
    const sum = [0, 0], cnt = [0, 0];
    for (let k = 0, pos = phase; pos < env.length - 3; k++, pos += period) { sum[k & 1] += Math.max(0, local[Math.round(pos)] - base); cnt[k & 1]++; }
    return [cnt[0] ? sum[0] / cnt[0] : 0, cnt[1] ? sum[1] / cnt[1] : 0];
  }

  // ---------- beat-synchronous chroma (Timing.cpp analyseBeats) ----------
  function analyseBeats(x, sr, beatTimes, period) {
    const stepSamples = Math.max(1, Math.round(period * sr));
    let n = 1024; while (n < Math.max(stepSamples, sr * 0.37)) n <<= 1;
    const binHz = sr / n, re = new Float64Array(n), im = new Float64Array(n), mag = new Float64Array(n / 2);
    const peaks = [], energy = new Float64Array(beatTimes.length);
    const trim = Math.floor(0.08 * stepSamples), maxBin = Math.min(n / 2 - 1, Math.ceil(2300 / binHz));
    for (let s = 0; s < beatTimes.length; s++) {
      const list = []; peaks.push(list);
      const first = Math.max(0, Math.round(beatTimes[s] * sr) + trim);
      if (first >= x.length) continue;
      const length = Math.min(stepSamples - 2 * trim, x.length - first);
      if (length < 64 || length < 0.6 * (stepSamples - 2 * trim)) continue;
      re.fill(0); im.fill(0);
      let e = 0;
      for (let i = 0; i < length; i++) {
        const v = x[first + i]; e += v * v;
        re[i] = v * (0.5 - 0.5 * Math.cos(2 * Math.PI * (i + 0.5) / length));
      }
      energy[s] = Math.sqrt(e / length);
      fft(re, im);
      let loudest = 0;
      for (let k = 0; k <= maxBin + 1 && k < n / 2; k++) { mag[k] = Math.sqrt(re[k] * re[k] + im[k] * im[k]); if (mag[k] > loudest) loudest = mag[k]; }
      if (!(loudest > 1e-9)) continue;
      for (let k = 2; k <= maxBin; k++) {
        const m = mag[k];
        if (m < 0.01 * loudest || m < mag[k - 1] || m <= mag[k + 1]) continue;
        const a = Math.log(mag[k - 1] + 1e-12), b = Math.log(m), c = Math.log(mag[k + 1] + 1e-12), den = a - 2 * b + c;
        const f = (k + (den < 0 ? 0.5 * (a - c) / den : 0)) * binHz;
        if (f < 30 || f > 2200) continue;
        list.push({ midi: 69 + 12 * Math.log2(f / 440), hz: f, level: Math.sqrt(m / loudest) });   // compressed
      }
      // A peak three or five times as high as a louder one is mostly that note's overtone (a fifth or a major
      // third above it, which would otherwise read as a chord tone): it counts for less.
      for (let i = list.length - 1; i > 0; i--) {
        const p = list[i];
        for (let j = 0; j < i; j++) {
          const q = list[j], ratio = p.hz / q.hz;
          if (ratio < 2.9) break;
          if (q.level >= p.level && (Math.abs(ratio - 3) < 0.03 || Math.abs(ratio - 5) < 0.05)) { p.level *= 0.5; break; }
        }
      }
    }
    // The recording's own tuning: the level-weighted circular mean of how far the peaks sit from the notes. When the peaks
    // agree (their resultant is a quarter of their total or more) it is used as it is. In a full mix (drums, noise,
    // distortion) they agree less (0.07-0.22 measured on detuned rock, EDM and trap mixes) while the mean still points the
    // right way: from 0.08 up it is used when it is far enough off to matter, 12 cents or more (A = 432 Hz is 32 cents flat).
    // A smaller offset is left at 0: real instruments in tune spread that much, and reading it moves only noise.
    let c = 0, sn = 0, total = 0;
    for (const list of peaks) for (const p of list) {
      if (p.hz < 60) continue;
      const ang = 2 * Math.PI * (p.midi - Math.round(p.midi));
      c += p.level * Math.cos(ang); sn += p.level * Math.sin(ang); total += p.level;
    }
    const agree = total > 0 ? Math.sqrt(c * c + sn * sn) / total : 0, off = Math.atan2(sn, c) / (2 * Math.PI);
    const tuning = agree >= 0.25 || (agree >= 0.08 && Math.abs(off) >= 0.12) ? off : 0;
    let loudestStep = 0; for (let s = 0; s < energy.length; s++) if (energy[s] > loudestStep) loudestStep = energy[s];
    // How much of each beat's sound is one note with its overtones: the share of the peaks' level on the harmonic series
    // of its lowest strong peak (within 0.5 %; an equal-tempered third is 0.8 % off the fifth overtone), and the share off
    // that series on other pitch classes than that note and its fifth (another note: a chord's third). A tuned kick, however
    // distorted, is nearly all on its series (0.77-1.0 on synthetic 808s, 0.00-0.12 off it); every music file of the test
    // sets 0.33 at most on it; a one-chord vamp over its root's bass 0.64-0.66 on it, but 0.27-0.30 off it.
    const harmonic = new Float64Array(peaks.length), harmonicOff = new Float64Array(peaks.length), harmonicPc = new Int8Array(peaks.length).fill(-1);
    for (let s = 0; s < peaks.length; s++) {
      const list = peaks[s]; let top = 0; for (const p of list) if (p.level > top) top = p.level;
      const f0 = list.find(p => p.level >= 0.5 * top); if (!f0) continue;
      const pc0 = mod(Math.round(f0.midi - tuning), 12); let on = 0, off = 0, sum = 0;
      for (const p of list) {
        const r = p.hz / f0.hz, k = Math.round(r), pc = mod(Math.round(p.midi - tuning), 12); sum += p.level;
        if (k >= 1 && Math.abs(r - k) < 0.005 * k) on += p.level; else if (p.hz >= 50 && pc !== pc0 && pc !== (pc0 + 7) % 12) off += p.level;
      }
      if (sum > 0) { harmonic[s] = on / sum; harmonicOff[s] = off / sum; harmonicPc[s] = pc0; }
    }
    const chroma = [], bass = [], sounding = [];
    for (let s = 0; s < peaks.length; s++) {
      const full = new Float64Array(12), low = new Float64Array(12);
      if (energy[s] > 0.01 * loudestStep) {
        for (const p of peaks[s]) {
          const midi = p.midi - tuning, nearest = Math.round(midi);
          if (Math.abs(midi - nearest) > 0.4) continue;       // between two notes: not part of the harmony
          const pc = mod(nearest, 12);
          if (p.hz >= 50) full[pc] += p.level;
          if (p.hz < 200) low[pc] += p.level * (200 / Math.max(p.hz, 40));   // the lowest note counts most
        }
        sounding.push(s);
      }
      chroma.push(full); bass.push(low);
    }
    // A kick drum rings at one low pitch on every beat, whatever the chord. What the bass register holds on
    // (nearly) every beat is taken off, unless that leaves a beat with nothing (a pedal note is all it has).
    if (sounding.length >= 8) {
      const floor = new Float64Array(12);
      for (let pc = 0; pc < 12; pc++) { const v = sounding.map(s => bass[s][pc]).sort((a, b) => a - b); floor[pc] = v[Math.floor(0.1 * (v.length - 1))]; }
      for (const s of sounding) {
        let before = 0, after = 0;
        for (let pc = 0; pc < 12; pc++) { before += bass[s][pc]; after += Math.max(0, bass[s][pc] - floor[pc]); }
        if (after > 0.2 * before) for (let pc = 0; pc < 12; pc++) bass[s][pc] = Math.max(0, bass[s][pc] - floor[pc]);
      }
    }
    for (const s of sounding) for (const v of [chroma[s], bass[s]]) { let t = 0; for (let i = 0; i < 12; i++) t += v[i]; if (t > 1e-9) for (let i = 0; i < 12; i++) v[i] /= t; }
    return { chroma, bass, energy, loudestStep, tuningCents: 100 * tuning, harmonic, harmonicOff, harmonicPc };
  }

  // ---------- note evidence from a transcriber (opts.notes: Spotify's Basic Pitch, run on the same audio) ----------
  // opts.notes = { note: each piano key's activation per frame, 0..1 (a Float32Array of frames x keys, or [frames][keys]),
  //   times: each frame's time in seconds from the first sample of the audio analyse() is given (one per frame, in
  //   order), keys: 88, midi0: 21 (key 0 is A0) }. Anything else is refused with the reason, and the spectrum is read alone.
  function readNotes(nt, audioSec) {
    if (!nt || typeof nt !== 'object') throw new Error('it is not a transcription');
    const rows = Array.isArray(nt.note) ? nt.note : null, flat = !rows && ArrayBuffer.isView(nt.note) ? nt.note : null;
    if (!rows && !flat) throw new Error('it holds no note activations');
    if (!nt.note.length) throw new Error('it is empty');
    const K = nt.keys === undefined ? (rows ? (rows[0] && rows[0].length) || 0 : 88) : +nt.keys, m0 = nt.midi0 === undefined ? 21 : +nt.midi0;
    if (!(Number.isInteger(K) && K > 0 && K <= 128) || !(Number.isInteger(m0) && m0 >= 0 && m0 + K <= 128)) throw new Error('its keys are not piano keys');
    if (flat && flat.length % K) throw new Error(`${flat.length} activations do not make whole frames of ${K} keys`);
    const T = rows ? rows.length : flat.length / K;
    if (rows) for (let k = 0; k < T; k++) if (!rows[k] || rows[k].length !== K) throw new Error(`frame ${k} does not hold ${K} keys`);
    const times = nt.times;
    if (!times || typeof times.length !== 'number') throw new Error('it has no frame times');
    if (times.length !== T) throw new Error(`it has ${times.length} frame times for ${T} frames`);
    for (let k = 0; k < T; k++) {
      if (!Number.isFinite(+times[k])) throw new Error(`frame ${k} has no time`);
      if (k && +times[k] < +times[k - 1]) throw new Error('its frame times are not in order');
    }
    // seconds from the first sample: frame times in milliseconds, or frame numbers, run far past the audio
    if (+times[0] < -1 || +times[T - 1] > 1.5 * audioSec + 3) throw new Error('its frame times do not fit the audio (seconds are expected)');
    const at = rows ? (k, i) => +rows[k][i] : (k, i) => +flat[k * K + i];
    let bad = 0;
    for (let k = 0; k < T; k++) for (let i = 0; i < K; i++) { const v = at(k, i); if (!(v >= 0 && v <= 1)) bad++; }
    if (bad) throw new Error(`${bad} of its activations are not numbers from 0 to 1`);
    return { T, K, m0, timeOf: k => +times[k], at };
  }
  // Per beat (its edges trimmed as for the spectrum): each key's mean activation above the transcriber's floor (every
  // key sits near 0.1 where nothing sounds: the median key, plus a little), square-root compressed like the spectrum's
  // peak levels. A pitch class counts with its strongest key: the transcriber tells which notes sound, and a root
  // doubled in three octaves would otherwise drown a seventh played once. The bass is the lowest key from E1 to A3
  // that holds at least 0.3. Both are normalised to sum 1, as analyseBeats' are; a beat with nothing above the floor
  // is left empty.
  const NOTE_LO = 28, NOTE_HI = 96, BASS_HI = 57, BASS_ON = 0.3, ABOVE_FLOOR = 0.02, NOTES_QUIET = 0.1, MELODY_LO = 60, MELODY_ON = 0.15, MELODY_WEIGHT = 0.6;
  function notesPerBeat(nv, beatTimes, periodSec) {
    const { T, K, m0, timeOf, at } = nv;
    const chroma = [], bass = [], act = new Float64Array(K), inRange = [];
    let k0 = 0, heard = 0;
    for (let s = 0; s < beatTimes.length; s++) {
      const c = new Float64Array(12), b = new Float64Array(12);
      chroma.push(c); bass.push(b);
      const lo = beatTimes[s] + 0.08 * periodSec, hi = beatTimes[s] + 0.92 * periodSec;
      while (k0 < T && timeOf(k0) < lo) k0++;
      act.fill(0); let n = 0;
      for (let k = k0; k < T && timeOf(k) < hi; k++, n++) for (let i = 0; i < K; i++) act[i] += at(k, i);
      if (n < 2) continue;
      inRange.length = 0;
      for (let i = 0; i < K; i++) { act[i] /= n; const midi = m0 + i; if (midi >= NOTE_LO && midi <= NOTE_HI) inRange.push(act[i]); }
      if (!inRange.length) continue;
      inRange.sort((a, b2) => a - b2);
      const floor = inRange[inRange.length >> 1] + ABOVE_FLOOR;
      let total = 0;
      for (let i = 0; i < K; i++) { const midi = m0 + i; if (midi >= NOTE_LO && midi <= NOTE_HI && act[i] > floor) total += act[i] - floor; }
      if (!(total >= NOTES_QUIET)) continue;                     // nothing with a pitch on this beat
      heard++;
      for (let i = 0; i < K; i++) { const midi = m0 + i; if (midi >= NOTE_LO && midi <= NOTE_HI && act[i] > floor) c[mod(midi, 12)] = Math.max(c[mod(midi, 12)], Math.sqrt(act[i] - floor)); }
      // A melody (a voice, a lead) is the highest line and often holds a note outside the chord. The highest key from C4 up
      // that sounds clearly, when no other key plays its pitch class, is such a lone top line: it counts for less.
      for (let i = K - 1; i >= 0; i--) {
        const midi = m0 + i; if (midi > NOTE_HI) continue; if (midi < MELODY_LO) break;
        if (act[i] - floor < MELODY_ON) continue;
        const pc = mod(midi, 12); let doubled = false;
        for (let j = 0; j < K && !doubled; j++) { const mj = m0 + j; if (j !== i && mj >= NOTE_LO && mj <= NOTE_HI && mod(mj, 12) === pc && act[j] > floor + 0.05) doubled = true; }
        if (!doubled) c[pc] *= MELODY_WEIGHT;
        break;
      }
      for (let i = 0; i < K; i++) { const midi = m0 + i; if (midi < NOTE_LO || midi > BASS_HI) continue; if (act[i] >= BASS_ON) { b[mod(midi, 12)] = 1; break; } }
      for (const v of [c, b]) { let t = 0; for (let i = 0; i < 12; i++) t += v[i]; if (t > 1e-9) for (let i = 0; i < 12; i++) v[i] /= t; }
    }
    return { chroma, bass, heard };
  }

  const cosineOf = (a, b) => { let d = 0, na = 0, nb = 0; for (let i = 0; i < 12; i++) { d += a[i] * b[i]; na += a[i] * a[i]; nb += b[i] * b[i]; } return na > 1e-12 && nb > 1e-12 ? d / Math.sqrt(na * nb) : 0; };
  const isSilent = v => { let t = 0; for (let i = 0; i < 12; i++) t += v[i]; return !(t > 1e-6); };

  // ---------- the chord tracker (Timing.cpp Tracker) ----------
  // A candidate: { pcs: its notes, root, prior, bassChoices: [{ pc, cost, tag }], shape }. Each note carries a little
  // of its third harmonic (the fifth above), as real sounds have: OVERTONE of it in the spectrum's chroma, half that
  // when the chroma is half a transcriber's (which hears no overtones).
  const OVERTONE = 0.2;
  const TONAL_NONE = 0.3, TONAL_SURE = 0.45;   // tonality (see readChords): no clear notes at all .. enough to trust a reading
  function shapeOf(pcs, overtone) {
    const shape = new Float64Array(12);
    for (const pc of pcs) { shape[pc] += 1; shape[mod(pc + 7, 12)] += overtone; }
    let norm = 0; for (let i = 0; i < 12; i++) norm += shape[i] * shape[i];
    norm = Math.sqrt(norm); for (let i = 0; i < 12; i++) shape[i] /= norm;
    return shape;
  }
  const THIRD_IN_BASS = 0.03, FIFTH_IN_BASS = 0.07, BASS_WEIGHT = 0.15;
  let BPB = 4;   // beats in a bar for the analysis running now
  const positionWeight = beat => (beat % BPB === 0 ? 1 : BPB % 2 === 0 && beat % (BPB / 2) === 0 ? 1.6 : 2.4);
  const changeCost = detail => { const simple = 1 - clamp01(detail); return 0.012 + 0.45 * simple * simple; };

  // own (when the chroma and bass are mixed with a transcriber's): the spectrum's own { chroma, bass } per beat. How
  // readily the chord changes is then judged on it (that cost was measured on the spectrum), and a chord is inverted
  // only as far as its own bass hears that note too (the transcriber's lowest note can be a kick's pitch or a passing note).
  function track(candidates, chroma, bass, detail, own) {
    const T = chroma.length, states = [];
    candidates.forEach((cand, c) => { cand.bassChoices.forEach((bc, i) => states.push({ c, pc: bc.pc, cost: bc.cost, i })); });
    const S = states.length, emission = [], silent = [], fit = new Float64Array(candidates.length);
    for (let t = 0; t < T; t++) {
      const em = new Float64Array(S); emission.push(em);
      silent.push(isSilent(chroma[t]));
      if (silent[t]) continue;                                  // all states equal: the chords around decide
      let bassTop = 0, norm = 0;
      for (let i = 0; i < 12; i++) { if (bass[t][i] > bassTop) bassTop = bass[t][i]; norm += chroma[t][i] * chroma[t][i]; }
      norm = Math.sqrt(norm);
      for (let c = 0; c < candidates.length; c++) { let d = 0; const sh = candidates[c].shape; for (let i = 0; i < 12; i++) d += chroma[t][i] * sh[i]; fit[c] = d / norm + candidates[c].prior; }
      let ownTop = 0; if (own) for (let i = 0; i < 12; i++) if (own.bass[t][i] > ownTop) ownTop = own.bass[t][i];
      for (let s = 0; s < S; s++) {
        let b = bassTop > 1e-6 ? bass[t][states[s].pc] / bassTop : 0;
        if (own && states[s].i > 0) b = Math.min(b, ownTop > 1e-6 ? own.bass[t][states[s].pc] / ownTop : 0);
        em[s] = fit[states[s].c] + BASS_WEIGHT * b - states[s].cost;
      }
    }
    const novelty = new Float64Array(T).fill(0.5), nc = own ? own.chroma : chroma, nb = own ? own.bass : bass;
    for (let t = 1; t < T; t++) if (!silent[t] && !silent[t - 1]) novelty[t] = 0.75 * (1 - cosineOf(nc[t - 1], nc[t])) + 0.25 * (1 - cosineOf(nb[t - 1], nb[t]));
    // Viterbi over (chord, bass) states. A change of chord costs lambda times where in the bar it falls
    // times how little the sound changes there.
    const lambda = changeCost(detail), back = [];
    let score = new Float64Array(S), next = new Float64Array(S);
    if (T) for (let s = 0; s < S; s++) score[s] = emission[0][s] + (candidates[states[s].c].tonic && states[s].i === 0 ? 1e-4 : 0);
    for (let t = 1; t < T; t++) {
      const weight = positionWeight(t), scale = lambda * (1.25 - 0.6 * Math.min(1, novelty[t] / 0.3));
      const chordCost = scale * weight, bassCost = scale * (weight > 1 ? Math.max(weight, 2.4) : weight);
      let best = 0; for (let s = 1; s < S; s++) if (score[s] > score[best]) best = s;
      let other = -1; for (let s = 0; s < S; s++) if (states[s].c !== states[best].c && (other < 0 || score[s] > score[other])) other = s;
      const bk = new Int32Array(S); back[t] = bk;
      for (let s = 0; s < S; s++) {
        let value = score[s], from = s;
        const consider = (src, cost) => { if (src >= 0 && src !== s && score[src] - cost > value) { value = score[src] - cost; from = src; } };
        if (states[s].c !== states[best].c) consider(best, chordCost); else { consider(best, bassCost); consider(other, chordCost); }
        next[s] = value + emission[t][s]; bk[s] = from;
      }
      const tmp = score; score = next; next = tmp;
    }
    const path = new Int32Array(T);
    if (T) { let cur = 0; for (let s = 1; s < S; s++) if (score[s] > score[cur]) cur = s; for (let t = T - 1; t >= 0; t--) { path[t] = cur; if (t > 0) cur = back[t][cur]; } }
    return { states, path, emission, silent, novelty };
  }
  // Runs of one state. A bass line that steps off for a beat and comes back (C C/G C) plays a figure under
  // one chord rather than changing it.
  function runsOf(tr) {
    const runs = [], T = tr.path.length;
    for (let t = 0; t < T;) { let end = t + 1; while (end < T && tr.path[end] === tr.path[t]) end++; runs.push({ state: tr.path[t], first: t, end }); t = end; }
    for (let i = 1; i + 1 < runs.length; i++) {
      const b = runs[i - 1], a = runs[i + 1], r = runs[i];
      if (b.state === a.state && b.state !== r.state && tr.states[r.state].c === tr.states[b.state].c && r.end - r.first <= 1) r.state = b.state;
    }
    const merged = [];
    for (const r of runs) { if (merged.length && merged[merged.length - 1].state === r.state) merged[merged.length - 1].end = r.end; else merged.push({ ...r }); }
    return merged;
  }
  // The average margin of the run's chord over the best different chord: 0.5 when level, 0 for silence.
  function confidenceOf(tr, run) {
    const cand = tr.states[run.state].c;
    let margin = 0, counted = 0;
    for (let u = run.first; u < run.end; u++) {
      if (tr.silent[u]) continue;
      let other = -1e9;
      for (let s = 0; s < tr.states.length; s++) if (tr.states[s].c !== cand && tr.emission[u][s] > other) other = tr.emission[u][s];
      margin += tr.emission[u][run.state] - other; counted++;
    }
    return counted ? clamp01(0.5 + 4 * margin / counted) : 0;
  }

  // Every root in each basic quality: what is heard before a key is known.
  const FREE_QUALITIES = [
    { name: 'maj', iv: [0, 4, 7], tri: 'maj', prior: 0 }, { name: 'min', iv: [0, 3, 7], tri: 'min', prior: 0 },
    { name: 'dom7', iv: [0, 4, 7, 10], tri: 'maj', prior: -0.025 }, { name: 'maj7', iv: [0, 4, 7, 11], tri: 'maj', prior: -0.025 },
    { name: 'min7', iv: [0, 3, 7, 10], tri: 'min', prior: -0.025 }, { name: 'sus4', iv: [0, 5, 7], tri: 'sus', prior: -0.045 },
    { name: 'dim', iv: [0, 3, 6], tri: 'dim', prior: -0.05 },
  ];
  const freeCache = new Map();
  function freeCandidates(overtone) {
    if (freeCache.has(overtone)) return freeCache.get(overtone);
    const out = [];
    for (let r = 0; r < 12; r++) for (const q of FREE_QUALITIES) {
      const pcs = q.iv.map(x => (r + x) % 12);
      out.push({ pcs, root: r, tri: q.tri, seventh: q.iv.length > 3, name: q.name, prior: q.prior, tonic: false, shape: shapeOf(pcs, overtone),
        bassChoices: pcs.slice(0, 3).map((pc, i) => ({ pc, cost: i === 0 ? 0 : i === 1 ? THIRD_IN_BASS : FIFTH_IN_BASS })) });
    }
    freeCache.set(overtone, out);
    return out;
  }

  // Everything a spec element can say in a key: 7 degrees x { the scale's triad, with its 7th, a major triad,
  // a dominant 7th, a sus4 } x root position and two inversions.
  function keyCandidates(keyRoot, scaleIdx, ct, overtone) {
    const st = SCALES[scaleIdx].st, key = { root: keyRoot, st, minor: st[2] === 3 };
    const inScale = new Set(st.map(x => (x + keyRoot) % 12)), out = [];
    for (let d = 0; d < 7; d++) {
      const seen = [];
      for (const v of [{}, { ext: 7 }, { q: 'maj' }, { q: 'dom7' }, { q: 'sus4' }]) {
        const base = ct({ d, ...v }, key), pcs = base.pcs.slice(), id = pcs.slice().sort((a, b) => a - b).join(',');
        if (seen.includes(id)) continue;                       // the scale already gives this chord
        seen.push(id);
        const third = mod(base.tones.third - base.tones.root, 12), fifth = mod(base.tones.fifth - base.tones.root, 12);
        let prior = 0;
        if (pcs.length > 3) prior -= 0.025;
        if (v.q === 'sus4') prior -= 0.045;
        if (third === 3 && fifth === 6) prior -= 0.05;
        if (third === 4 && fifth === 8) prior -= 0.06;
        // a note outside the scale costs; the raised 7th of a minor key (the third of its major V) belongs
        for (const pc of pcs) if (!inScale.has(pc) && !(st[2] === 3 && mod(pc - keyRoot, 12) === 11)) prior -= 0.035;
        const bassChoices = [0, 1, 2].map(inv => {
          const el = { d, ...v }; if (inv) el.inv = inv;
          return { pc: ct(el, key).pcs[0], cost: inv === 0 ? 0 : inv === 1 ? THIRD_IN_BASS : FIFTH_IN_BASS, inv };
        });
        out.push({ pcs, root: base.tones.root, el: { d, ...v }, prior, tonic: d === 0 && !v.q && !v.ext, bassChoices, shape: shapeOf(pcs, overtone) });
      }
    }
    return out;
  }

  // ---------- key ----------
  // Krumhansl-Kessler profiles (Analysis.cpp detectKey).
  const MAJOR_PROFILE = [6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88];
  const MINOR_PROFILE = [6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17];
  function correlation(chroma, profile, tonic) {
    let mc = 0, mp = 0;
    for (let i = 0; i < 12; i++) { mc += chroma[i]; mp += profile[i]; }
    mc /= 12; mp /= 12;
    let num = 0, dc = 0, dp = 0;
    for (let i = 0; i < 12; i++) { const c = chroma[(i + tonic) % 12] - mc, p = profile[i] - mp; num += c * p; dc += c * c; dp += p * p; }
    const den = Math.sqrt(dc * dp);
    return den > 0 ? num / den : 0;
  }
  // How well a key can say a heard chord: 1 when the scale gives it, less when a spec has to override the
  // quality (least for the major V of a minor key, which is idiomatic), little when only the root fits.
  function sayable(heard, keyRoot, st) {
    const d = st.indexOf(mod(heard.root - keyRoot, 12));
    if (d < 0) return 0;
    const iv = k => mod(st[(d + k) % 7] - st[d], 12), third = iv(2), fifth = iv(4);
    const natural = third === 3 ? (fifth === 6 ? 'dim' : 'min') : fifth === 8 ? 'aug' : 'maj';
    if (heard.tri === natural) return 1;
    if (heard.tri === 'maj') return d === 4 && st[2] === 3 ? 0.95 : 0.6;
    if (heard.tri === 'sus') return d === 0 || d === 4 ? 0.9 : 0.6;
    return 0.3;
  }
  function detectKey(heard, chromaSum, phrase, kt) {
    kt = kt || {};   // weights that differ from the defaults (on a host grid, the first chord says nothing); long: more than 4 bars were read
    // heard: one { root, tri } per sounding beat; phrase: the same for the 4 (or 8) bars the progression is read from
    const scored = [];
    const first = phrase.length ? phrase[0] : null, last = phrase.length ? phrase[phrase.length - 1] : null;
    for (let r = 0; r < 12; r++) for (let s = 0; s < SCALES.length; s++) {
      const st = SCALES[s].st, minor = st[2] === 3, tonicTri = minor ? 'min' : 'maj';
      let cover = 0, share = 0;
      for (const h of heard) { cover += sayable(h, r, st); if (h.root === r && h.tri === tonicTri) share++; }
      if (heard.length) { cover /= heard.length; share /= heard.length; }
      const opens = first && first.root === r && first.tri === tonicTri ? 1 : 0;
      // a 4-bar loop that ends on V turns round to the tonic; an 8-bar phrase mostly ends at home itself, and a last chord
      // on the tonic would credit the key a fifth below (measured on real recordings read in 8 bars)
      const cadence = !kt.long && last && last.root === (r + 7) % 12 && last.tri !== 'dim' ? 1 : 0;
      // in a minor key, a major chord on the fifth degree (it holds the raised leading note) that goes to the
      // tonic chord, round the loop too, points at that tonic
      let resolves = 0;
      for (let i = 0; minor && i < phrase.length && !resolves; i++) {
        const a = phrase[i], b = phrase[(i + 1) % phrase.length];
        if (a.root === (r + 7) % 12 && a.tri === 'maj' && b.root === r && b.tri === tonicTri) resolves = 1;
      }
      const corr = correlation(chromaSum, minor ? MINOR_PROFILE : MAJOR_PROFILE, r);
      const score = cover + (kt.share ?? 0.35) * share + (kt.opens ?? 0.12) * opens + 0.05 * cadence + 0.12 * resolves + (kt.corr ?? 0.2) * corr - (s > 1 ? (kt.mode ?? 0.05) : 0);
      scored.push({ root: r, scaleIdx: s, score, cover });
    }
    scored.sort((a, b) => b.score - a.score || a.scaleIdx - b.scaleIdx || a.root - b.root);
    return scored;
  }

  // ---------- the whole analysis ----------
  function analyse(samples, sampleRate, opts) {
    const o = opts || {};
    const notes = [];
    BPB = o.grid && o.grid.beatsPerBar > 0 ? Math.max(2, Math.min(12, Math.round(+o.grid.beatsPerBar))) : 4;
    // No reading at all: failed (failReason: 'no-audio', 'invalid', 'silent', 'too-short' or 'no-pitch'; notes[0] says it in
    // words), no chords (an empty spec) and every confidence 0, so that nothing can pass it off as chords that were heard.
    const fallback = (why, code) => ({ failed: true, failReason: code, bpm: 120, bpmConfidence: 0, beatOffsetSec: 0, key: { root: 9, scaleIdx: 0 }, keyConfidence: 0,
      spec: [], confidence: [], chordConfidence: 0, bassPcs: [], notes: [why], tuningCents: 0, tonality: 0, pitched: false, oneNote: false });
    if (!samples || !samples.length || !(sampleRate > 0)) return fallback('no audio to listen to', 'no-audio');
    if (!(samples instanceof Float32Array)) { try { samples = Float32Array.from(samples); } catch (e) { return fallback('no audio to listen to', 'no-audio'); } }
    const limitSec = o.grid ? 70 : 60, sr = Number(sampleRate), total = Math.min(samples.length, Math.floor(sr * limitSec)), audioSec = samples.length / sr;
    if (samples.length > total) notes.push(`only the first ${limitSec} seconds were listened to`);
    // A sample that is not a number (NaN or infinite: a glitch in a plugin, a broken file) is read as silence, on a copy (the
    // input is left as it was): one bad sample must not void the whole read.
    const finite = v => v - v === 0;
    let any = false, bad = 0;
    for (let i = 0; i < total; i++) { const v = samples[i]; if (!finite(v)) bad++; else if (v > 1e-5 || v < -1e-5) any = true; }
    if (bad) {
      if (!any) return fallback('the audio holds no valid samples (they are not numbers)', 'invalid');
      samples = Float32Array.from(samples.subarray(0, total), v => (finite(v) ? v : 0));
      notes.push(`${bad} sample${bad === 1 ? ' was not a number (NaN or infinite) and was' : 's were not numbers (NaN or infinite) and were'} read as silence`);
    }
    if (!any) return fallback('the audio is silent', 'silent');

    const dO = Math.max(1, Math.round(sr / 22050)), dC = Math.max(1, Math.round(sr / 11025));
    const xO = decimate(samples, total, dO), srO = sr / dO, xC = dC === dO ? xO : decimate(samples, total, dC), srC = sr / dC;
    const seconds = total / sr;

    // tempo and beats
    // A known grid (inside a DAW: the host's tempo, and the song beat the audio starts on) replaces the estimate.
    const grid = o.grid && o.grid.bpm > 0 && Number.isFinite(o.grid.bpm) ? o.grid : null;
    const on = onsetEnvelopes(xO, srO);
    let bpm = 0, bpmConfidence = 0, beatOffsetSec = 0, periodSec = 0.5, first = 0, gridK0 = 0;
    const tempo = on ? estimateTempo(on) : null;
    let audioBpm = 0;
    if (grid && tempo) {
      // the beat the audio itself keeps, against the project's (octaves and a third apart count as the same beat)
      audioBpm = Math.round(on.frameRate * 60 / tempo.period * 100) / 100;
      const g = +grid.bpm, ratio = audioBpm / g, near = [0.5, 2 / 3, 1, 1.5, 2].some(r => Math.abs(ratio / r - 1) < 0.012);
      if (!near && tempo.comb(tempo.period, 4) > 0.3) notes.push(`the audio on this track seems to run at ${audioBpm} BPM, not the project's ${g}: the chords may drift against it`);
      else audioBpm = 0;
    }
    if (grid) {
      bpm = Math.min(Math.max(+grid.bpm, 20), 400); periodSec = 60 / bpm; bpmConfidence = 1;
      const at0 = Math.max(0, +grid.firstBeatSec || 0);
      gridK0 = Math.floor(at0 / periodSec + 1e-9); first = at0 - gridK0 * periodSec;   // earlier whole beats are kept too
    } else if (tempo) {
      const bands = [on.low, on.mid, on.high].map(b => shapeEnvelope(b, 0).env);
      let period = tempo.period, phase = beatPhase(bands, period);
      // When every other step of the grid carries next to nothing below the cymbals (no kick, no bass or chord
      // starting), those steps are off-beats: the beat is the grid half as fast.
      const body = new Float64Array(bands[0].length);
      for (let i = 0; i < body.length; i++) body[i] = bands[0][i] + bands[1][i];
      const par = parity(body, period, phase), uneven = Math.min(par[0], par[1]) < 0.15 * Math.max(par[0], par[1]);
      if (uneven && period * 2 <= tempo.longest * 1.02) {
        period = tempo.refine(Math.min(period * 2, tempo.longest), 1);
        phase = beatPhase(bands, period);
        notes.push('every other pulse is cymbals only, so the slower tempo was taken (double it if the track feels twice as fast)');
      } else if (period * 0.5 >= tempo.shortest * 0.98) {
        // The other way round: when kicks, bass notes or chords start half-way between the beats about as
        // strongly as on them, those are beats too and the grid is twice as fast.
        const fine = tempo.refine(Math.max(period * 0.5, tempo.shortest), 1), finePhase = beatPhase(bands, fine), fp = parity(body, fine, finePhase);
        if (Math.min(fp[0], fp[1]) > 0.5 * Math.max(fp[0], fp[1])) { period = fine; phase = finePhase; }
      }
      bpm = on.frameRate * 60 / period;
      if (Math.abs(bpm - Math.round(bpm)) < bpm * 0.0015) bpm = Math.round(bpm);   // loops are nearly always made at a whole tempo
      bpm = Math.min(Math.max(bpm, MIN_TEMPO), MAX_TEMPO);
      periodSec = 60 / bpm;
      // a frame hears an onset once it is a little way into the window
      first = mod((phase * on.hop + ONSET_LATENCY * on.n) / srO, periodSec);
      if (first > 0.9 * periodSec) first -= periodSec;       // a beat that falls a hair before the file starts is its first beat
      bpmConfidence = clamp01(tempo.comb(period, 4) * 1.25);
      bpm = Math.round(bpm * 100) / 100;
    } else {
      bpm = 120; periodSec = 0.5;
      notes.push('no steady beat was found: the tempo is a guess and the chords are read on a 120 BPM grid');
      // the grid starts where the sound does
      let thr = 0; for (let i = 0; i < xC.length; i++) { const a = Math.abs(xC[i]); if (a > thr) thr = a; }
      thr *= 0.05; let i0 = 0; while (i0 < xC.length && Math.abs(xC[i0]) < thr) i0++;
      first = mod(i0 / srC, periodSec);
    }

    // the beats of the grid, and what sounds on each
    const beatTimes = [];
    for (let k = 0; first + k * periodSec < seconds - 0.25 * periodSec && k < 400; k++) beatTimes.push(first + k * periodSec);
    if (beatTimes.length < 2) return fallback('the audio is too short to find chords in', 'too-short');
    const all = analyseBeats(xC, srC, beatTimes, periodSec);
    // the middle's own chroma: what is sounding where, and how passes compare, are judged on it alone
    const mid = all.chroma.map(c => Float64Array.from(c));
    // Note evidence (opts.notes) goes half and half with the spectrum's chroma and bass on every beat the spectrum hears,
    // and the chords are read from that mix. What was measured on the spectrum keeps reading the spectrum's own chroma
    // and bass (spectral, spectralBass): which beats sound, where the bar starts and how passes compare (mid above), how
    // readily the tracker changes chord and inverts one, and the drums / one-note tests.
    let spectral = null, spectralBass = null, overtone = OVERTONE, notesHeard = null;
    if (o.notes) {
      let heard = null;
      try { heard = notesPerBeat(readNotes(o.notes, audioSec), beatTimes, periodSec); } catch (e) { notes.push(`the note transcription could not be used: ${e.message}`); }
      if (heard && heard.heard) {
        notesHeard = heard;
        spectral = all.chroma.map(c => Float64Array.from(c)); spectralBass = all.bass.map(b => Float64Array.from(b));
        for (let k = 0; k < beatTimes.length; k++) {
          if (!(all.energy[k] > 0.01 * all.loudestStep)) continue;
          for (let i = 0; i < 12; i++) { all.chroma[k][i] = 0.5 * all.chroma[k][i] + 0.5 * heard.chroma[k][i]; all.bass[k][i] = 0.5 * all.bass[k][i] + 0.5 * heard.bass[k][i]; }
        }
        overtone = OVERTONE / 2;
      }
    }
    const midBass = spectralBass || all.bass;
    if (o.side && o.side.length >= total) {
      let sideIn = o.side instanceof Float32Array ? o.side : Float32Array.from(o.side);
      for (let i = 0; i < total; i++) if (!finite(sideIn[i])) { sideIn = Float32Array.from(sideIn.subarray(0, total), v => (finite(v) ? v : 0)); break; }
      const xS = decimate(sideIn, total, dC), side = analyseBeats(xS, srC, beatTimes, periodSec);
      for (let k = 0; k < beatTimes.length; k++) {
        // Only on beats where the music sounds in the middle too, and where the side carries a large share of it (a
        // part spread out of phase). Quiet beats are left alone: their side is reverb tails, which would read as chords.
        const loudMid = all.energy[k] > 0.05 * all.loudestStep, ratio = all.energy[k] > 1e-9 ? side.energy[k] / all.energy[k] : 0;
        const w = loudMid ? Math.min(1, Math.max(0, (ratio - 0.5) / 0.5)) : 0;
        if (w > 0.05) for (let i = 0; i < 12; i++) { all.chroma[k][i] += w * side.chroma[k][i]; if (spectral) spectral[k][i] += w * side.chroma[k][i]; }
      }
    }
    if (Math.abs(all.tuningCents) > 12) notes.push(`the recording is tuned ${Math.abs(Math.round(all.tuningCents))} cents ${all.tuningCents > 0 ? 'sharp' : 'flat'}; the chords were read against its own tuning`);

    // which beat starts the bar: chords mostly change on bar lines, and when that does not decide, the first
    // beat that sounds does
    // A beat sounds when it is loud enough and its notes stand out as the music's usually do (hiss or room
    // noise before the music starts spreads over all twelve).
    const G = beatTimes.length, loud = t => !isSilent(mid[t]) && all.energy[t] > 0.05 * all.loudestStep;
    const top4 = t => Array.from(mid[t]).sort((a, b) => b - a).slice(0, 4).reduce((a, b) => a + b, 0);
    const clear = []; for (let t = 0; t < G; t++) if (loud(t)) clear.push(top4(t));
    clear.sort((a, b) => a - b);
    // dullBeat (set when the grid is read): beats of a 4-bar stretch with far less pitch than the rest of the
    // recording (a drums-only intro or break) are left out of the chords
    let dullBeat = null;
    const clearEnough = clear.length ? 0.7 * clear[clear.length >> 1] : 0, soundsAtAll = t => loud(t) && top4(t) >= clearEnough, sounding = t => soundsAtAll(t) && !(dullBeat && dullBeat[t]);
    let firstSound = 0; while (firstSound < G - 1 && !sounding(firstSound)) firstSound++;
    const nov = new Float64Array(G);
    for (let t = firstSound + 1; t < G; t++) if (sounding(t) && sounding(t - 1)) nov[t] = 0.75 * (1 - cosineOf(mid[t - 1], mid[t])) + 0.25 * (1 - cosineOf(midBass[t - 1], midBass[t]));
    const barScore = [0, 0, 0, 0], barCount = [0, 0, 0, 0];
    for (let t = firstSound + 1; t < G; t++) { barScore[t % 4] += nov[t]; barCount[t % 4]++; }
    let novTop = 0;
    for (let p = 0; p < 4; p++) { barScore[p] = barCount[p] ? barScore[p] / barCount[p] : 0; if (barScore[p] > novTop) novTop = barScore[p]; }
    // spec16, specBass16: the spectrum's own chroma and bass on the same beats as chroma and bass (with note evidence)
    let down, N, chroma, bass, downSec, spec16 = null, specBass16 = null; const gridInfo = {};
    let windowed = !!grid;
    if (grid) ({ down, N, chroma, bass, downSec, spec16, specBass16 } = gridWindow(grid));
    else {
    let barPhase = firstSound % 4, barBest = -1;
    for (let p = 0; p < 4; p++) { const v = (novTop > 1e-9 ? barScore[p] / novTop : 0) + (p === firstSound % 4 ? 0.3 : 0); if (v > barBest) { barBest = v; barPhase = p; } }
    down = firstSound; while (down % 4 !== barPhase) down++;
    if (down >= G - 1) down = firstSound;
    if (down !== firstSound) notes.push('the audio seems to start before the bar line: the progression is read from the first bar line after it');

    // beats from the first downbeat on, as far as they sound
    let lastSound = G - 1; while (lastSound > down && !sounding(lastSound)) lastSound--;
    N = lastSound - down + 1; chroma = all.chroma.slice(down, down + N); bass = all.bass.slice(down, down + N);
    if (spectral) { spec16 = spectral.slice(down, down + N); specBass16 = spectralBass.slice(down, down + N); }
    // Two passes of 4 bars or more: read as on a host's grid that starts on the first downbeat (passes compared, the
    // alike ones averaged, 8 bars when the 4-bar stretches differ).
    if (tempo && N >= 8 * BPB) {
      const w = gridWindow({ segments: [{ sec: beatTimes[down], beat: 0 }] });
      if (w.N) { ({ down, N, chroma, bass, spec16, specBass16 } = w); downSec = w.firstSec; windowed = true; }
    }
    }
    if (!N) return fallback('nothing with a pitch was heard', 'no-pitch');
    beatOffsetSec = Math.max(0, windowed ? downSec : beatTimes[down]);

    // The 16 beats of the song's 4-bar grid that hold what was heard: song beats 16w ... 16w+15. When the music
    // repeats every 4 bars, every pass is averaged (steadier chords); when it does not (a longer progression),
    // only the latest pass is read, and the notes say which bars.
    function gridWindow(g) {
      const segs = Array.isArray(g.segments) && g.segments.length ? g.segments.map(s => ({ sec: +s.sec || 0, beat: +s.beat || 0 })).sort((a, b) => a.sec - b.sec)
        : [{ sec: 0, beat: (+g.startBeat || 0) - gridK0 }];
      const songBeat = k => { const tk = beatTimes[k]; let s = segs[0]; for (const x of segs) if (x.sec <= tk + 0.25 * periodSec) s = x; return Math.round(s.beat + (tk - s.sec) / periodSec); };
      const bpb = BPB, W4 = 4 * bpb, W8 = 8 * bpb;
      // How much of each 4-bar stretch's sound is in its three strongest notes. A stretch well below the best one
      // (drums only, while the rest has chords) says nothing about the chords: its beats are left out.
      {
        const share = k => { const c = Array.from(mid[k]), t = c.reduce((a, b) => a + b, 0); if (!(t > 1e-9)) return 0; c.sort((a, b) => b - a); return (c[0] + c[1] + c[2]) / t; };
        const by = new Map(); for (let k = 0; k < G; k++) { if (!soundsAtAll(k)) continue; const w = Math.floor(songBeat(k) / W4); if (!by.has(w)) by.set(w, []); by.get(w).push(k); }
        const tonal = [...by.entries()].filter(([, ks]) => ks.length >= 4).map(([w, ks]) => [w, ks.reduce((a, k) => a + share(k), 0) / ks.length]);
        const best = Math.max(0, ...tonal.filter(([w]) => by.get(w).length >= W4 / 2).map(x => x[1]));
        const dull = tonal.filter(([, v]) => v < 0.5 && v < 0.8 * best).map(x => x[0]);
        gridInfo.tonal = tonal.map(([w, v]) => w + ":" + v.toFixed(2)).join(" ");
        if (dull.length && dull.length < tonal.length) { dullBeat = new Uint8Array(G); for (const w of dull) for (const k of by.get(w)) dullBeat[k] = 1; gridInfo.dull = dull; }
      }
      const soundingIn = ks => ks.filter(k => k >= 0 && sounding(k)).length;
      const like = (ks, ref) => { let s = 0, n = 0; for (let p = 0; p < ks.length; p++) if (ks[p] >= 0 && ref[p] >= 0 && sounding(ks[p]) && sounding(ref[p])) { s += cosineOf(mid[ks[p]], mid[ref[p]]); n++; } return n ? s / n : 1; };
      const windowsOf = len => {
        const w = new Map();
        for (let k = 0; k < G; k++) { const b = songBeat(k), i = Math.floor(b / len); if (!w.has(i)) w.set(i, new Array(len).fill(-1)); w.get(i)[mod(b, len)] = k; }
        return w;
      };
      // a stretch counts as whole when at most two of its beats are missing (the audio may end on its last beat)
      const fullOf = (w, len) => [...w.entries()].filter(([, ks]) => ks.filter(k => k >= 0).length >= len - 2 && soundingIn(ks) >= len / 2).sort((a, b) => a[0] - b[0]);
      let len = W4, windows = windowsOf(W4), full = fullOf(windows, W4);
      // the chord heard on each beat (best plain triad by chroma and bass), to compare passes by what they say
      const FC = freeCandidates(OVERTONE).filter(f => !f.seventh);
      const labelOf = k => {
        if (k < 0 || !sounding(k)) return -1;
        const c = mid[k], b = midBass[k]; let norm = 0, bn = 0; for (let i = 0; i < 12; i++) { norm += c[i] * c[i]; bn += b[i] * b[i]; }
        norm = Math.sqrt(norm) || 1; bn = Math.sqrt(bn) || 1;
        let best = -1e9, lab = -1;
        for (const f of FC) { let d = 0; for (let i = 0; i < 12; i++) d += c[i] * f.shape[i]; const s = d / norm + 0.35 * b[f.root] / bn + f.prior; if (s > best) { best = s; lab = f.root * 4 + (f.tri === 'min' ? 1 : f.tri === 'dim' ? 2 : f.tri === 'aug' ? 3 : 0); } }
        return lab;
      };
      const labels = []; for (let k = 0; k < G; k++) labels.push(labelOf(k));
      const mismatch = lag => { let n = 0, m = 0; for (let k = 0; k + lag < G; k++) if (labels[k] >= 0 && labels[k + lag] >= 0) { n++; if (labels[k] !== labels[k + lag]) m++; } return n >= 8 ? m / n : -1; };
      // how alike two stretches are, on the beats both have (at least 8 sounding beats in common)
      const overlap = (a, b) => { let n = 0; for (let p = 0; p < a.length; p++) if (a[p] >= 0 && b[p] >= 0 && sounding(a[p]) && sounding(b[p])) n++; return n; };
      const ws = [...windows.entries()].sort((a, b) => a[0] - b[0]);
      const next = [], skip = [];
      for (const [w, ks] of ws) {
        const a = windows.get(w - 1), b = windows.get(w - 2);
        if (a && overlap(ks, a) >= 8) next.push(like(ks, a));
        if (a && b && overlap(ks, b) >= 8 && overlap(ks, a) >= 8) skip.push([like(ks, b), like(ks, a)]);
      }
      const m16 = mismatch(W4), m32 = mismatch(W8);
      gridInfo.next = next; gridInfo.skip = skip; gridInfo.m16 = m16; gridInfo.m32 = m32;
      // 8 bars when the music changes from one 4-bar stretch to the next: plainly (a stretch unlike the one before),
      // or subtly (each stretch is more like the one two before than the one just before, and the chords heard
      // repeat after 32 beats much more than after 16)
      const plain = next.some(v => v < 0.8);
      const mean = a => a.reduce((x, y) => x + y, 0) / a.length;
      const subtle = skip.length > 0 && mean(skip.map(([s, n]) => s - n)) >= 0.02 && m32 >= 0 && m16 - m32 >= 0.12;
      // The progression is built from every stretch that agrees with the latest one, beat by beat at its place in the
      // cycle: a stretch that starts late (an intro bar, a part played once) is completed by the next one.
      const pick = (wins, L) => {
        const list = [...wins.entries()].filter(([, ks]) => soundingIn(ks) >= 4).sort((a, b) => a[0] - b[0]);
        if (!list.length) return null;
        let li = list.length - 1; while (li > 0 && soundingIn(list[li][1]) < L / 2) li--;   // the latest stretch heard well
        const latest = list[li][1];
        const used = list.filter(([, ks]) => overlap(ks, latest) < 4 || like(ks, latest) >= 0.8);
        let covered = 0; for (let p = 0; p < L; p++) if (used.some(([, ks]) => ks[p] >= 0 && sounding(ks[p]))) covered++;
        return { list, latest, latestW: list[li][0], used, covered };
      };
      let sel = pick(windows, W4);
      if (plain || subtle) {
        const s32 = pick(windowsOf(W8), W8);
        if (s32 && s32.covered >= W8 - 2) { len = W8; sel = s32; }
        else notes.push(grid ? 'the progression seems longer than 4 bars: play 8 whole bars and listen again to read all of it'
          : 'the progression seems longer than 4 bars: open a longer part of the song (8 whole bars) to read all of it');
      }
      if (!sel) return { N: 0 };
      if (sel.covered < len - 2) notes.push(grid ? `less than ${len / bpb} bars were heard on the grid: play the track a little longer and listen again for every chord`
        : `less than ${len / bpb} bars of the file have chords: open a longer part of the song to read every chord`);
      const used = sel.used, latest = sel.latest;
      if (used.length < sel.list.length) {
        const w = sel.latestW;
        notes.push(`the progression changes over more than ${len / bpb} bars: these are bars ${Math.floor(len * w / bpb) + 1}-${Math.floor(len * (w + 1) / bpb)} ${grid ? 'of your song' : 'of the file, from its first downbeat'}`);
      } else { const passes = used.filter(([, ks]) => soundingIn(ks) >= len / 2).length; if (passes > 1 && full.length > 1) notes.push(`${passes} passes of the ${len / bpb} bars were heard and averaged`); }
      const ch = [], ba = [], sc = [], sb = [];
      for (let p = 0; p < len; p++) {
        const c = new Float64Array(12), b = new Float64Array(12), fc = new Float64Array(12), fb = new Float64Array(12); let n = 0;
        for (const [, ks] of used) {
          const k = ks[p]; if (k < 0 || !sounding(k)) continue;
          for (let i = 0; i < 12; i++) { c[i] += all.chroma[k][i]; b[i] += all.bass[k][i]; if (spectral) { fc[i] += spectral[k][i]; fb[i] += spectralBass[k][i]; } }
          n++;
        }
        if (n) for (let i = 0; i < 12; i++) { c[i] /= n; b[i] /= n; fc[i] /= n; fb[i] /= n; }
        ch.push(c); ba.push(b); sc.push(fc); sb.push(fb);
      }
      // where the cycle starts: the time of the latest stretch's first place (it may lie before the audio; then a cycle later)
      const p0 = latest.findIndex(k => k >= 0), k0 = latest[p0];
      let start = beatTimes[k0] - p0 * periodSec; while (start < -1e-6) start += len * periodSec;
      const firstUsed = used.find(([w]) => w >= 0);
      let firstSec = start;
      if (firstUsed) { const ks = firstUsed[1], pf = ks.findIndex(k => k >= 0); firstSec = beatTimes[ks[pf]] - pf * periodSec; while (firstSec < -1e-6) firstSec += len * periodSec; }
      return { down: k0, downSec: start, firstSec, N: len, chroma: ch, bass: ba, spec16: spectral ? sc : null, specBass16: spectral ? sb : null };
    }

    // the 16 beats the progression is read from; shorter audio is looped (in whole bars when it has any)
    const LEN = windowed ? N : 16, MAXCH = LEN > 4 * BPB ? 16 : 8;
    const loopLen = N >= LEN ? LEN : N >= BPB ? N - (N % BPB) : N, at = t => t % loopLen;

    // The key and the chords from one reading of the beats from the first downbeat on (ch, ba: their chroma and bass).
    // own: the spectrum's own chroma and bass on the same beats, when ch and ba mix in a transcriber's; verdict: whether
    // there are chords to read at all ({ pitched, oneNote }), when that is known already. Null when nothing with a pitch
    // was heard; `said` holds its notes.
    function readChords(ch, ba, own, overtone, verdict, whole) {
      const said = [];
      // what is heard before a key is known
      const free = freeCandidates(overtone), freeTrack = track(free, ch, ba, 0.75, own);
      const heardAt = t => { const c = free[freeTrack.states[freeTrack.path[t]].c]; return { root: c.root, tri: c.tri }; };
      const heard = [], chromaSum = new Float64Array(12);
      for (let t = 0; t < N; t++) { if (freeTrack.silent[t]) continue; heard.push(heardAt(t)); for (let i = 0; i < 12; i++) chromaSum[i] += ch[t][i]; }
      if (!heard.length) return null;
      // the key from the whole recording (when there is more of it than the stretch read)
      let keyHeard = heard, keySum = chromaSum;
      if (whole && whole.chroma.length > N) {
        const wt = track(free, whole.chroma, whole.bass, 0.75, whole.own), kh = [], ks = new Float64Array(12);
        for (let t = 0; t < whole.chroma.length; t++) { if (wt.silent[t]) continue; const c = free[wt.states[wt.path[t]].c]; kh.push({ root: c.root, tri: c.tri }); for (let i = 0; i < 12; i++) ks[i] += whole.chroma[t][i]; }
        if (kh.length) { keyHeard = kh; keySum = ks; }
      }
      if (N < LEN) said.push(`only ${N} beat${N === 1 ? '' : 's'} of audio after the first downbeat: ${loopLen === N ? 'they were' : `the first ${loopLen} were`} looped to fill 4 bars`);
      const phrase = []; for (let t = 0; t < Math.min(LEN, loopLen); t++) if (!freeTrack.silent[t]) phrase.push(heardAt(t));
      // (on the host's grid the window starts on the song's 4-bar grid, which is often but not always where the progression
      // starts: its first chord counts for a third of what it does in a file; measured best for songs on and off the grid)
      const keys = detectKey(keyHeard, keySum, phrase, { opens: grid ? 0.04 : 0.12, long: !grid && LEN > 4 * BPB }), key = { root: keys[0].root, scaleIdx: keys[0].scaleIdx };
      const samePitches = (a, b) => { const A = SCALES[a.scaleIdx].st.map(x => (x + a.root) % 12).sort().join(), B = SCALES[b.scaleIdx].st.map(x => (x + b.root) % 12).sort().join(); return A === B; };
      const rival = keys.find(k => k !== keys[0] && !(k.root === keys[0].root && k.scaleIdx > 1 && keys[0].scaleIdx <= 1));
      let keyConfidence = clamp01(0.25 + 4 * (keys[0].score - rival.score)) * clamp01(keys[0].cover);
      if (keyConfidence < 0.6) said.push(`the key is not certain: ${NOTE[rival.root]} ${SCALES[rival.scaleIdx].name} fits nearly as well${samePitches(rival, keys[0]) ? ' (it uses the same notes)' : ''}`);

      // the chords a spec can say in that key, over the 16 beats
      const cands = keyCandidates(key.root, key.scaleIdx, chordTonesOf(o), overtone), c16 = [], b16 = [];
      for (let t = 0; t < LEN; t++) { c16.push(ch[at(t)]); b16.push(ba[at(t)]); }
      const own16 = own ? { chroma: c16.map((_, t) => own.chroma[at(t)]), bass: b16.map((_, t) => own.bass[at(t)]) } : null;
      const tr = track(cands, c16, b16, 0.75, own16);
      let runs = runsOf(tr).map(r => ({ ...r, confidence: confidenceOf(tr, r) }));
      // a spec holds at most 8 chords: the least certain short ones give way to the chord before them
      let merged = 0;
      while (runs.length > MAXCH) {
        let w = 1; for (let i = 2; i < runs.length; i++) { const a = runs[i], b = runs[w]; if (a.end - a.first < b.end - b.first || (a.end - a.first === b.end - b.first && a.confidence < b.confidence)) w = i; }
        runs[w - 1].end = runs[w].end; runs.splice(w, 1); merged++;
      }
      if (merged) said.push(`the chords change more often than a progression of ${MAXCH} can hold: ${merged} short chord${merged === 1 ? ' was' : 's were'} left out`);
      if (runs.length === 1) runs = [{ ...runs[0], end: LEN / 2 }, { ...runs[0], first: LEN / 2 }];

      // how much better a chord from outside the key would have fitted each beat
      const outside = new Float64Array(LEN);
      for (let t = 0; t < LEN; t++) {
        if (tr.silent[t]) continue;
        let norm = 0; for (let i = 0; i < 12; i++) norm += c16[t][i] * c16[t][i];
        norm = Math.sqrt(norm);
        let bestFree = -1e9; for (const f of free) { let d = 0; for (let i = 0; i < 12; i++) d += c16[t][i] * f.shape[i]; d = d / norm + f.prior; if (d > bestFree) bestFree = d; }
        const st = tr.states[tr.path[t]], cd = cands[st.c];
        let d = 0; for (let i = 0; i < 12; i++) d += c16[t][i] * cd.shape[i];
        outside[t] = bestFree - (d / norm + cd.prior);
      }
      const spec = [], confidence = [], approximated = new Set(), quiet = new Set();
      for (const r of runs) {
        const st = tr.states[r.state], cd = cands[st.c], el = { d: cd.el.d, len: r.end - r.first };
        if (cd.el.q) el.q = cd.el.q;
        if (cd.el.ext) el.ext = cd.el.ext;
        const inv = cd.bassChoices[st.i].inv; if (inv) el.inv = inv;
        if (el.len <= 2 && r.first % BPB !== 0 && (el.len === 1 || inv)) el.sub = true;       // a passing chord
        let worst = 0, sil = 0;
        for (let t = r.first; t < r.end; t++) { worst += outside[t]; if (tr.silent[t]) sil++; }
        worst /= el.len;
        let conf = r.confidence;
        if (worst > 0.1) { approximated.add(Math.floor(r.first / BPB) + 1); conf *= 0.5; }
        if (sil === el.len) quiet.add(Math.floor(r.first / BPB) + 1);
        spec.push(el); confidence.push(Math.round(conf * 1000) / 1000);
      }
      for (const b of approximated) said.push(`a chord outside the key was approximated in bar ${b}`);
      for (const b of quiet) said.push(`nothing with a pitch sounds in part of bar ${b}: the chord there is carried over`);

      const bassPcs = [];
      for (let t = 0; t < LEN; t++) {
        let top = -1, v = 0; for (let i = 0; i < 12; i++) if (b16[t][i] > v) { v = b16[t][i]; top = i; }
        bassPcs.push(v > 0.3 ? top : -1);
      }
      // how much the sound has pitch at all: per beat, the share of the three strongest notes in all twelve
      // (chords and bass put most of it in three notes; drums and noise spread it out). On the spectrum's own chroma:
      // these tests were measured on it.
      const tonalOf = t => (own16 ? own16.chroma[t] : c16[t]);
      let tonal = 0, tonalN = 0;
      for (let t = 0; t < LEN; t++) { const c = Array.from(tonalOf(t)), s = c.reduce((a, b) => a + b, 0); if (!(s > 1e-9)) continue; c.sort((a, b) => b - a); tonal += (c[0] + c[1] + c[2]) / s; tonalN++; }
      const tonality = tonalN ? Math.round(tonal / tonalN * 1000) / 1000 : 0;
      // drums or noise only: no chords to read (measured: drum loops 0.31-0.36, mastered songs with chords 0.36 and up).
      // A kick whose pitch sweeps can reach 0.46; what it gives fits no key: weak key, and in half the bars or more a
      // chord from outside the key fits better (every song of the test corpus: none).
      const offKey = approximated.size / Math.max(1, Math.ceil(LEN / BPB));
      const pitched = verdict ? verdict.pitched : !(tonality < 0.34 || (tonality < 0.37 && keyConfidence < 0.4) || (tonality < 0.5 && keyConfidence < 0.45 && offKey >= 0.5));
      if (!pitched) said.push('no chords were heard: the audio seems to be drums or noise only');
      // A tuned kick (an 808) or one held bass note: the same single note is the strongest on every beat, and next to
      // it only its own overtones (its fifth). That tells neither the chords nor major from minor. Harmony moves its
      // strongest note across the loop, or sounds other notes too (even a one-chord vamp has its third).
      // Measured: 808 loops 7-38, one held bass note about 20, one-chord vamps with bass and drums 126 and up.
      const topCount = new Array(12).fill(0), prof = new Array(12).fill(0); let beatsWithPitch = 0;
      for (let t = 0; t < LEN; t++) {
        const c = tonalOf(t); let top = 0, v = 0; for (let i = 0; i < 12; i++) if (c[i] > v) { v = c[i]; top = i; }
        if (!(v > 1e-9)) continue;
        // a beat counts by how much of its sound is in its strongest note: a snare or hats between kicks hardly count
        let sum = 0; for (let i = 0; i < 12; i++) sum += c[i];
        const w = v / sum;
        topCount[top] += w; beatsWithPitch += w; for (let i = 0; i < 12; i++) prof[i] += w * c[i] / v;
      }
      const lone = verdict && verdict.lone !== undefined ? verdict.lone : topCount.indexOf(Math.max(...topCount)), tops = topCount.filter(k => k >= 0.15 * beatsWithPitch).length;
      let others = 0; for (let i = 0; i < 12; i++) if (i !== 0 && i !== 7) others += prof[(lone + i) % 12];
      others = prof[lone] > 0 ? 100 * others / prof[lone] : 0;
      const oneNote = verdict ? verdict.oneNote : pitched && beatsWithPitch > 0 && tops <= 1 && others < 60;
      if (oneNote) { keyConfidence = Math.min(keyConfidence, 0.3); said.push(`only one note sounds (${NOTE[lone]}), like a tuned kick or a held bass note: the key and chords are a guess`); }
      let cc = 0; spec.forEach((el, i) => { cc += confidence[i] * el.len; });
      let chordConfidence = Math.round(clamp01(cc / LEN) * (0.5 + 0.5 * keyConfidence) * (oneNote ? 0.5 : 1) * 1000) / 1000;
      // How sure the key and chords can be at most. Little of the sound in a few notes (tonality under 0.45: drums, a tuned
      // kick with drums, muted strumming; measured on real guitar recordings: 50-60 % of the roots right there, against 75-80 %
      // above 0.5) or little of the progression heard (a clip shorter than it, looped) caps every confidence, the note
      // transcription's too (it hears notes in drums as readily as in chords).
      let heardBeats = 0; for (let t = 0; t < Math.min(loopLen, LEN); t++) if (!freeTrack.silent[t]) heardBeats++;
      const pitchCap = clamp01((tonality - TONAL_NONE) / (TONAL_SURE - TONAL_NONE)), heardCap = 0.4 + 0.6 * Math.min(1, heardBeats / LEN);
      const cap = Math.min(pitchCap, heardCap);
      if (cap < 1) {
        keyConfidence = Math.min(keyConfidence, cap); chordConfidence = Math.min(chordConfidence, Math.round(cap * 1000) / 1000);
        for (let i = 0; i < confidence.length; i++) confidence[i] = Math.min(confidence[i], Math.round(cap * 1000) / 1000);
        if (pitched && pitchCap < 0.75) said.push('little of the sound is in clear notes (drums, noise or muted playing): the key and chords are not certain');
      }
      return { key, keyConfidence, spec, confidence, chordConfidence, bassPcs, tonality, pitched, oneNote, said,
        spread: { tops, others: Math.round(others), offKey: Math.round(offKey * 100) / 100 } };
    }
    // The spectrum's own reading says whether there are chords to read at all (the drums-only and one-note tests were
    // measured on it); what the transcription hears over the whole audio can overrule it (below). With note evidence, the
    // chords are then read again from the mix, but only when there are: for drums only or a single note, the
    // transcription has nothing to add.
    const wholeK = []; for (let k = 0; k < G; k++) if (sounding(k)) wholeK.push(k);
    const wholeOf = (c, b, own) => ({ chroma: wholeK.map(k => c[k]), bass: wholeK.map(k => b[k]), own: own ? { chroma: wholeK.map(k => own.c[k]), bass: wholeK.map(k => own.b[k]) } : null });
    const readPlain = verdict => (spec16 ? readChords(spec16, specBass16, null, OVERTONE, verdict, wholeOf(spectral, spectralBass, null))
      : readChords(chroma, bass, null, OVERTONE, verdict, wholeOf(all.chroma, all.bass, null)));
    let plain = readPlain(null);
    if (!plain) return fallback('nothing with a pitch was heard', 'no-pitch');
    // One note with its overtones on nearly every beat (a distorted tuned kick: its fifth, third and seventh overtones spell
    // a chord, and a snare between its hits keeps the chroma test above from seeing one note) is one note.
    if (plain.pitched && !plain.oneNote && wholeK.length >= 8) {
      let sum = 0, off = 0, on = 0; const pcs = new Array(12).fill(0);
      for (const k of wholeK) { sum += all.harmonic[k]; off += all.harmonicOff[k]; if (all.harmonic[k] >= 0.6 && all.harmonicPc[k] >= 0) { on++; pcs[all.harmonicPc[k]]++; } }
      const lone = pcs.indexOf(Math.max(...pcs));
      if (sum >= 0.6 * wholeK.length && off <= 0.15 * wholeK.length && on && pcs[lone] >= 0.9 * on) plain = readPlain({ pitched: true, oneNote: true, lone }) || plain;
    }
    // What the transcription hears over the beats that sound overrules the spectrum's verdict, which drums can fool:
    // - A drum kit (tuned toms, a cowbell, a kick) puts its two strongest 'notes' a semitone apart on nearly every beat (97 %
    //   measured; music in tune 37 % at most), always the same ones (the strongest is one note on 71-81 % of the beats) and
    //   little else (the rest 1.7-2.6 times the strongest). Music a quarter tone off splits every note between two keys too
    //   (up to 90 %), but its strongest note moves with the chords (15-50 %), many notes sound (3.5-11 times) and the
    //   spectrum hears its tuning. -> drums only.
    // - A lone tuned kick or bass note is the strongest note on nearly every beat, with next to nothing on its third or
    //   anywhere but its fifth (98-100 % of the beats, third 0.17-0.19 and the rest 0.54-0.58 of it; music at most 89 %, and
    //   0.23 / 0.85 then). -> one note.
    // - Muted strumming (real guitar: tonality 0.33-0.37) can look like drums to the spectrum, while the transcription hears
    //   notes on every beat, many (5-11 times the strongest), moving (the strongest one on 24-38 % of the beats) and not in
    //   clusters (0-11 %); drum loops: clusters on 60-97 % of the beats, the same strongest note on 63-81 %. -> music.
    if (notesHeard && wholeK.length >= 8) {
      let n = 0, cluster = 0; const tops = new Array(12).fill(0);
      for (const k of wholeK) {
        const v = notesHeard.chroma[k]; let a = 0; for (let i = 1; i < 12; i++) if (v[i] > v[a]) a = i;
        if (!(v[a] > 0)) continue;
        let b = a ? 0 : 1; for (let i = 0; i < 12; i++) if (i !== a && v[i] > v[b]) b = i;
        n++; tops[a]++; if (v[b] > 0.5 * v[a] && (mod(a - b, 12) === 1 || mod(b - a, 12) === 1)) cluster++;
      }
      const lone = tops.indexOf(Math.max(...tops)), top = n ? tops[lone] / n : 0; let third = 0, rest = 0, m = 0;
      for (const k of wholeK) {
        const v = notesHeard.chroma[k]; if (!(v[lone] > 0)) continue;
        m++; third += (v[(lone + 3) % 12] + v[(lone + 4) % 12]) / v[lone];
        for (let i = 0; i < 12; i++) if (i !== lone && i !== (lone + 7) % 12) rest += v[i] / v[lone];
      }
      if (m) { third /= m; rest /= m; }
      let verdict = null;
      if (n >= 8 && plain.pitched && !plain.oneNote) {
        const drums = cluster >= 0.6 * n && top >= 0.6 && rest < 3 && Math.abs(all.tuningCents) < 25;
        const one = !drums && top >= 0.9 && third < 0.25 && rest < 0.7;
        if (drums || one) verdict = { pitched: !drums, oneNote: one, lone };
      } else if (!plain.pitched && n >= 0.8 * wholeK.length && cluster < 0.3 * n && top < 0.5 && rest >= 3 && plain.tonality >= 0.3) verdict = { pitched: true, oneNote: false };
      if (verdict) plain = readPlain(verdict) || plain;
    }
    const read = spec16 && plain.pitched && !plain.oneNote ? readChords(chroma, bass, { chroma: spec16, bass: specBass16 }, overtone, plain, wholeOf(all.chroma, all.bass, { c: spectral, b: spectralBass })) || plain : plain;
    notes.push(...read.said);
    if (bpmConfidence < 0.35 && tempo && !grid) notes.push('the beat is not steady or not clear: tempo and chord timing may be off');
    return { bpm, bpmConfidence: Math.round(bpmConfidence * 1000) / 1000, beatOffsetSec: Math.round(beatOffsetSec * 10000) / 10000,
      key: read.key, keyConfidence: Math.round(read.keyConfidence * 1000) / 1000, spec: read.spec, confidence: read.confidence, chordConfidence: read.chordConfidence,
      bassPcs: read.bassPcs, notes, tuningCents: Math.round(all.tuningCents * 10) / 10, tonality: read.tonality, pitched: read.pitched, oneNote: read.oneNote,
      audioBpm, gridInfo, spread: read.spread };
  }

  const M1D1Listen = Object.freeze({ analyse, SCALES: Object.freeze(SCALES.map(s => Object.freeze({ name: s.name, st: Object.freeze(s.st.slice()) }))) });
  root.M1D1Listen = M1D1Listen;
  if (typeof module !== 'undefined' && module.exports) module.exports = M1D1Listen;
})(typeof globalThis !== 'undefined' ? globalThis : this);
