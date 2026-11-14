// Tests for m1d1-listen.js: synthesizes loops with known tempo, downbeat, key and chords, runs the analyser on
// them and reports measured accuracy. Run: node test-listen.js [-v] [--only=name]
'use strict';
const assert = require('assert');
const path = require('path');
const Gen = require(path.join(__dirname, 'm1d1-gen.js'));
const Listen = require(path.join(__dirname, 'm1d1-listen.js'));

const verbose = process.argv.includes('-v'), onlyArg = (process.argv.find(a => a.startsWith('--only=')) || '').slice(7);
const SCALES = [[0, 2, 3, 5, 7, 8, 10], [0, 2, 4, 5, 7, 9, 11], [0, 2, 3, 5, 7, 9, 10], [0, 1, 3, 5, 7, 8, 10], [0, 2, 4, 6, 7, 9, 11], [0, 2, 4, 5, 7, 9, 10], [0, 2, 3, 5, 7, 8, 11]];
const SCALE_NAME = ['min', 'maj', 'dor', 'phr', 'lyd', 'mix', 'h.min'];
const NOTE = ['C', 'C#', 'D', 'Eb', 'E', 'F', 'F#', 'G', 'Ab', 'A', 'Bb', 'B'];
const keyOf = (root, scaleIdx) => ({ root, st: SCALES[scaleIdx], minor: SCALES[scaleIdx][2] === 3 });
const mod = (a, n) => ((a % n) + n) % n;

// ---------- synthesis ----------
function lcg(seed) { let s = (seed * 2654435761) % 2147483647 || 1; return () => ((s = (s * 16807) % 2147483647) / 2147483647) * 2 - 1; }
// a sum of sine partials 1/k (a saw), written with a rotating phasor per partial
function addTone(out, sr, startSec, durSec, hz, amp, partials, env) {
  const i0 = Math.max(0, Math.round(startSec * sr)), i1 = Math.min(out.length, Math.round((startSec + durSec) * sr)), n = i1 - i0;
  if (n <= 0) return;
  const e = new Float64Array(n); for (let i = 0; i < n; i++) e[i] = env(i / sr, durSec);
  for (let k = 1; k <= partials; k++) {
    const f = hz * k; if (f > sr * 0.45) break;
    const w = 2 * Math.PI * f / sr, cw = Math.cos(w), sw = Math.sin(w), ph = (hz * 7.13 * k) % (2 * Math.PI);
    let c = Math.cos(ph), s = Math.sin(ph); const a = amp / k;
    for (let i = 0; i < n; i++) { out[i0 + i] += a * s * e[i]; const c2 = c * cw - s * sw; s = s * cw + c * sw; c = c2; }
  }
}
const padEnv = (t, d) => Math.min(1, t / 0.008) * Math.min(1, (d - t) / 0.03) * (0.65 + 0.35 * Math.exp(-t / 0.6));
const pluckEnv = (t, d) => Math.min(1, t / 0.004) * Math.min(1, (d - t) / 0.02) * (0.35 + 0.65 * Math.exp(-t / 0.25));
function lowpass(x, sr, cutoff) {
  const half = 48, taps = new Float64Array(2 * half + 1), fc = cutoff / sr; let sum = 0;
  for (let i = -half; i <= half; i++) { const v = (i === 0 ? 2 * fc : Math.sin(2 * Math.PI * fc * i) / (Math.PI * i)) * (0.5 + 0.5 * Math.cos(Math.PI * i / half)); taps[i + half] = v; sum += v; }
  const out = new Float32Array(x.length);
  for (let o = 0; o < x.length; o++) { let acc = 0; const lo = Math.max(0, o - half), hi = Math.min(x.length - 1, o + half); for (let i = lo; i <= hi; i++) acc += x[i] * taps[i - o + half]; out[o] = acc / sum; }
  return out;
}
// One loop: chord tones in octaves 3-4 (two detuned saws each), a bass an octave or two below on the root or
// the inversion's bass note, a kick on every beat, a hat on the off-beats, optional noise and band-limiting.
function synth(c) {
  const sr = c.sr || 44100, beat = 60 / c.bpm, bars = c.bars || 8, offset = c.offset || 0, key = keyOf(c.root, c.scaleIdx);
  const total = Math.ceil((offset + bars * 4 * beat + 0.05) * sr), out = new Float64Array(total), rnd = lcg(c.seed || 1);
  const truth = [];
  let at = 0;
  while (at < bars * 4) for (const el of c.spec) {
    if (at >= bars * 4) break;
    const len = Math.min(el.len, bars * 4 - at), ct = Gen.chordTones(el, key), t0 = offset + at * beat;
    for (const m0 of ct.notes) {
      let m = m0; while (m > 71) m -= 12;
      const hz = 440 * Math.pow(2, (m - 69 + (c.tune || 0) / 100) / 12);
      for (const cents of [-4, 4]) addTone(out, sr, t0, len * beat, hz * Math.pow(2, cents / 1200), 0.055, c.sine ? 3 : 8, padEnv);
    }
    const bassPc = ct.pcs[0], bm = c.bassLow ? 28 + mod(bassPc - 4, 12) : 36 + bassPc;     // E1..Eb2 or C2..B2
    const bhz = 440 * Math.pow(2, (bm - 69 + (c.tune || 0) / 100) / 12);
    if (c.bassPerBeat) for (let b = 0; b < len; b++) addTone(out, sr, t0 + b * beat, beat * 0.95, bhz, 0.22, 6, pluckEnv);
    else addTone(out, sr, t0, len * beat, bhz, 0.2, 6, padEnv);
    for (let b = 0; b < len; b++) truth.push({ root: ct.tones.root, bass: bassPc, set: [...new Set(ct.pcs)].sort((a, b) => a - b).join(',') });
    at += len;
  }
  if (!c.noDrums) for (let b = 0; b < bars * 4; b++) {
    const k0 = Math.round((offset + b * beat) * sr);
    if (c.snare && b % 2 === 1) { let q = 0; for (let i = 0; i < 0.12 * sr && k0 + i < total; i++) { const w = rnd(); q += 0.35 * (w - q); out[k0 + i] += 0.3 * q * Math.exp(-i / sr / 0.04) + 0.12 * Math.sin(2 * Math.PI * 190 * i / sr) * Math.exp(-i / sr / 0.03); } }
    if (!(c.backbeat && b % 2 === 1)) for (let i = 0; i < 0.3 * sr && k0 + i < total; i++) { const t = i / sr; out[k0 + i] += 0.45 * Math.exp(-t / 0.07) * Math.sin(2 * Math.PI * (48 * t + 100 * 0.03 * (1 - Math.exp(-t / 0.03)))) + (i < 0.002 * sr ? 0.15 * rnd() : 0); }
    const h0 = Math.round((offset + (b + 0.5) * beat) * sr);
    let p1 = 0, p2 = 0;
    for (let i = 0; i < 0.06 * sr && h0 + i < total; i++) { const w = rnd(), hp = w - 2 * p1 + p2; p2 = p1; p1 = w; out[h0 + i] += (c.hat === undefined ? 0.08 : c.hat) * hp * Math.exp(-i / sr / 0.015); }
  }
  if (c.noise) for (let i = 0; i < total; i++) out[i] += c.noise * rnd();
  let x = Float32Array.from(out);
  if (c.lowpass) x = lowpass(x, sr, c.lowpass);
  return { samples: x, sr, truth };
}

// ---------- cases ----------
const P = (...els) => els.map(e => { const o = { d: e[0], len: e[1] }; if (e[2]) Object.assign(o, e[2]); return o; });
const MAJOR = [
  P([0, 4], [4, 4], [5, 4], [3, 4]),
  P([0, 4], [3, 4], [4, 4], [0, 4]),
  P([0, 2], [5, 2], [3, 2], [4, 2], [0, 2], [5, 2], [1, 2], [4, 2]),
  P([0, 4], [5, 4], [1, 4], [4, 4, { ext: 7 }]),
  P([0, 2], [4, 2, { inv: 1 }], [5, 4], [3, 4], [4, 2, { q: 'sus4' }], [4, 2]),
  P([0, 3], [4, 1], [5, 3], [3, 1], [0, 4], [4, 4]),
  P([0, 4], [3, 2], [0, 2, { inv: 2 }], [1, 4], [4, 4]),
  P([0, 4], [2, 4], [3, 4], [4, 4]),
];
const MINOR = [
  P([0, 4], [5, 4], [2, 4], [6, 4]),
  P([0, 4], [3, 4], [4, 4], [0, 4]),
  P([0, 4], [3, 4], [4, 4, { q: 'dom7' }], [0, 4]),
  P([0, 4], [5, 2], [5, 2, { inv: 1 }], [2, 4], [6, 3], [4, 1, { q: 'dom7' }]),
  P([0, 2], [6, 2, { inv: 1 }], [2, 4], [3, 4], [4, 2, { q: 'sus4' }], [4, 2, { q: 'maj' }]),
  P([0, 2], [5, 2], [3, 2], [4, 2], [0, 2], [6, 2], [5, 2], [4, 2, { q: 'maj' }]),
  P([0, 1], [6, 1], [5, 2], [3, 4], [0, 1], [2, 1], [6, 2], [4, 4]),
  P([0, 4], [6, 4], [5, 4], [4, 4, { q: 'maj' }]),
];
const TEMPOS = [70, 76, 84, 90, 96, 100, 105, 110, 116, 120, 124, 128, 132, 138, 140, 145, 150, 156, 160, 166, 170, 174];
const OFFSETS = [0, 0.21, 0.5, 0, 0.34, 1.0, 0, 0.08, 0.77, 0, 1.6, 0.13];
const cases = [];
let n = 0;
// sweep 1: all 12 keys, major and minor, clean
for (let r = 0; r < 12; r++) for (const scaleIdx of [1, 0]) {
  const pool = scaleIdx ? MAJOR : MINOR;
  cases.push({ name: `s1 ${NOTE[r]} ${SCALE_NAME[scaleIdx]}`, root: r, scaleIdx, spec: pool[n % pool.length], bpm: TEMPOS[(n * 5) % TEMPOS.length], offset: OFFSETS[n % OFFSETS.length], bassLow: n % 2 === 0, bassPerBeat: n % 3 === 0, seed: n + 1 });
  n++;
}
// sweep 2: all 12 keys again with other progressions, tempos, sample rates, noise and mp3-like band-limiting
for (let r = 0; r < 12; r++) for (const scaleIdx of [0, 1]) {
  const pool = scaleIdx ? MAJOR : MINOR;
  cases.push({ name: `s2 ${NOTE[r]} ${SCALE_NAME[scaleIdx]}`, root: r, scaleIdx, spec: pool[(n + 3) % pool.length], bpm: TEMPOS[(n * 7 + 3) % TEMPOS.length], offset: OFFSETS[(n * 5 + 2) % OFFSETS.length],
    bassLow: n % 2 === 1, bassPerBeat: n % 3 !== 0, noise: [0, 0.01, 0.03][n % 3], lowpass: [0, 16000, 11000, 0][n % 4], sr: [44100, 48000, 44100, 22050][n % 4], sine: n % 5 === 0, seed: n + 1 });
  n++;
}
// modes and harmonic minor
const MODAL = [
  [2, 2, P([0, 4], [3, 4], [0, 4], [3, 4]), 122], [9, 2, P([0, 4], [3, 4], [0, 4], [6, 4]), 96], [4, 2, P([0, 2], [3, 2], [0, 2], [3, 2], [0, 2], [3, 2], [6, 2], [3, 2]), 110],
  [7, 5, P([0, 4], [6, 4], [0, 4], [6, 4]), 128], [2, 5, P([0, 4], [6, 4], [3, 4], [0, 4]), 104],
  [5, 4, P([0, 4], [1, 4], [0, 4], [1, 4]), 118], [0, 4, P([0, 4], [1, 4], [0, 4], [1, 2], [4, 2]), 134],
  [4, 3, P([0, 4], [1, 4], [0, 4], [1, 4]), 140], [11, 3, P([0, 4], [1, 4], [0, 4], [6, 4]), 92],
  [9, 6, P([0, 4], [4, 4], [0, 4], [6, 4]), 126], [3, 6, P([0, 4], [3, 4], [4, 4], [6, 2], [4, 2]), 100], [6, 6, P([0, 2], [4, 2], [0, 2], [6, 2], [0, 4], [4, 4]), 150],
];
MODAL.forEach(([r, s, spec, bpm], i) => cases.push({ name: `mode ${NOTE[r]} ${SCALE_NAME[s]}`, root: r, scaleIdx: s, spec, bpm, offset: OFFSETS[(i * 3) % OFFSETS.length], bassLow: i % 2 === 0, bassPerBeat: i % 2 === 1, seed: 100 + i }));
// specials
cases.push({ name: 'x short 4 bars', root: 9, scaleIdx: 0, spec: MINOR[0], bpm: 124, bars: 4, seed: 201 });
cases.push({ name: 'x short 2 bars (looped)', root: 0, scaleIdx: 1, spec: P([0, 4], [3, 4], [0, 4], [3, 4]), bpm: 100, bars: 2, seed: 202 });
cases.push({ name: 'x long 16 bars', root: 5, scaleIdx: 1, spec: MAJOR[0], bpm: 128, bars: 16, offset: 0.4, seed: 203 });
cases.push({ name: 'x low mp3 7k', root: 2, scaleIdx: 0, spec: MINOR[2], bpm: 118, lowpass: 7000, noise: 0.01, seed: 204 });
cases.push({ name: 'x loud hats', root: 7, scaleIdx: 1, spec: MAJOR[1], bpm: 88, hat: 0.3, seed: 205 });
cases.push({ name: 'x loud hats slow', root: 4, scaleIdx: 0, spec: MINOR[1], bpm: 72, hat: 0.3, offset: 0.3, seed: 206 });
cases.push({ name: 'x fast loud hats', root: 10, scaleIdx: 0, spec: MINOR[0], bpm: 172, hat: 0.3, seed: 207 });
cases.push({ name: 'x VI-iv-i-v (not on tonic)', root: 9, scaleIdx: 0, spec: P([5, 4], [3, 4], [0, 4], [4, 4]), bpm: 120, seed: 208 });
cases.push({ name: 'x vi-IV-I-V major (not on tonic)', root: 0, scaleIdx: 1, spec: P([5, 4], [3, 4], [0, 4], [4, 4]), bpm: 126, seed: 209 });
cases.push({ name: 'x no drums', root: 3, scaleIdx: 1, spec: MAJOR[0], bpm: 110, noDrums: true, bassPerBeat: true, seed: 210 });

// ---------- measuring ----------
function expand(spec, key) {
  const out = [];
  for (const el of spec) { const ct = Gen.chordTones(el, key); for (let b = 0; b < el.len; b++) out.push({ root: ct.tones.root, bass: ct.pcs[0], set: [...new Set(ct.pcs)].sort((a, b) => a - b).join(',') }); }
  return out;
}
function checkShape(r, name) {
  assert.ok(Number.isFinite(r.bpm) && r.bpm >= 60 && r.bpm <= 200, `${name}: bpm ${r.bpm}`);
  for (const k of ['bpmConfidence', 'keyConfidence', 'chordConfidence']) assert.ok(r[k] >= 0 && r[k] <= 1, `${name}: ${k} ${r[k]}`);
  assert.ok(Number.isFinite(r.beatOffsetSec) && r.beatOffsetSec >= 0, `${name}: beatOffsetSec`);
  assert.ok(Number.isInteger(r.key.root) && r.key.root >= 0 && r.key.root < 12 && Number.isInteger(r.key.scaleIdx) && r.key.scaleIdx >= 0 && r.key.scaleIdx < 7, `${name}: key`);
  assert.ok(Array.isArray(r.spec) && r.spec.length >= 2 && r.spec.length <= 8, `${name}: spec has ${r.spec && r.spec.length} elements`);
  assert.strictEqual(r.spec.reduce((a, e) => a + e.len, 0), 16, `${name}: lens sum`);
  for (const e of r.spec) {
    assert.ok(Number.isInteger(e.d) && e.d >= 0 && e.d <= 6 && Number.isInteger(e.len) && e.len >= 1, `${name}: element ${JSON.stringify(e)}`);
    assert.ok(e.q === undefined || ['dom7', 'sus4', 'maj'].includes(e.q), `${name}: q`);
    assert.ok(e.ext === undefined || e.ext === 7, `${name}: ext`);
    assert.ok(e.inv === undefined || e.inv === 1 || e.inv === 2, `${name}: inv`);
    assert.ok(e.sub === undefined || e.sub === true, `${name}: sub`);
    for (const k of Object.keys(e)) assert.ok(['d', 'len', 'q', 'ext', 'inv', 'sub'].includes(k), `${name}: stray field ${k}`);
  }
  assert.strictEqual(r.confidence.length, r.spec.length, `${name}: confidence length`);
  for (const c of r.confidence) assert.ok(c >= 0 && c <= 1, `${name}: confidence ${c}`);
  assert.ok(Array.isArray(r.bassPcs) && r.bassPcs.length === 16 && r.bassPcs.every(p => Number.isInteger(p) && p >= -1 && p < 12), `${name}: bassPcs`);
  assert.ok(Array.isArray(r.notes) && r.notes.every(s => typeof s === 'string'), `${name}: notes`);
  // the generator accepts the spec as a frame
  const key = keyOf(r.key.root, r.key.scaleIdx);
  for (const e of r.spec) assert.ok(Gen.chordTones(e, key).pcs.length >= 3);
}

if (require.main !== module) { module.exports = { synth, expand, keyOf, cases, P, NOTE, SCALE_NAME, lcg, mod }; return; }

let failures = 0;
const check = (ok, msg) => { if (!ok) { failures++; console.log('FAIL: ' + msg); } };
const stat = { cases: 0, tempoOk: 0, tempoOctave: 0, tempoWrong: 0, offsetOk: 0, downbeatOk: 0, keyOk: 0, keyRootOk: 0, beats: 0, rootOk: 0, fullOk: 0, bassOk: 0, bassBeats: 0, ms: 0 };
const group = {};
for (const c of cases) {
  if (onlyArg && !c.name.includes(onlyArg)) continue;
  const a = synth(c), t0 = process.hrtime.bigint();
  const r = Listen.analyse(a.samples, a.sr, { gen: Gen });
  const ms = Number(process.hrtime.bigint() - t0) / 1e6;
  checkShape(r, c.name);
  const offset = c.offset || 0, beat = 60 / c.bpm;
  const tOk = Math.abs(r.bpm - c.bpm) <= 1, tOct = !tOk && (Math.abs(r.bpm - 2 * c.bpm) <= 2 || Math.abs(r.bpm - c.bpm / 2) <= 0.5);
  const offErr = Math.abs(r.beatOffsetSec - offset), offOk = offErr <= 0.035;
  const barErr = Math.abs(mod(r.beatOffsetSec - offset + 2 * beat, 4 * beat) - 2 * beat);       // right bar line, any bar
  const kOk = r.key.root === c.root && r.key.scaleIdx === c.scaleIdx;
  const got = expand(r.spec, keyOf(r.key.root, r.key.scaleIdx));
  let rootOk = 0, fullOk = 0, bassOk = 0, bassBeats = 0;
  for (let b = 0; b < 16; b++) {
    const w = a.truth[b % a.truth.length];
    if (got[b].root === w.root) rootOk++;
    if (got[b].root === w.root && got[b].set === w.set && got[b].bass === w.bass) fullOk++;
    if (r.bassPcs[b] >= 0) { bassBeats++; if (r.bassPcs[b] === w.bass) bassOk++; }
  }
  const g = c.name.split(' ')[0]; group[g] = group[g] || { n: 0, tempo: 0, key: 0, root: 0, full: 0 };
  group[g].n++; group[g].tempo += tOk ? 1 : 0; group[g].key += kOk ? 1 : 0; group[g].root += rootOk; group[g].full += fullOk;
  stat.cases++; stat.tempoOk += tOk ? 1 : 0; stat.tempoOctave += tOct ? 1 : 0; stat.tempoWrong += !tOk && !tOct ? 1 : 0;
  stat.offsetOk += offOk ? 1 : 0; stat.downbeatOk += barErr <= 0.035 ? 1 : 0; stat.keyOk += kOk ? 1 : 0; stat.keyRootOk += r.key.root === c.root ? 1 : 0;
  stat.beats += 16; stat.rootOk += rootOk; stat.fullOk += fullOk; stat.bassOk += bassOk; stat.bassBeats += bassBeats; stat.ms = Math.max(stat.ms, ms);
  if (verbose || !tOk || !kOk || rootOk < 16 || fullOk < 16 || !offOk)
    console.log(`${(tOk && kOk && fullOk === 16 && offOk) ? 'ok  ' : 'miss'} ${c.name.padEnd(34)} bpm ${String(r.bpm).padStart(6)}/${c.bpm}${tOk ? '' : tOct ? ' OCTAVE' : ' WRONG'}  off ${r.beatOffsetSec.toFixed(3)}/${offset.toFixed(2)}${offOk ? '' : ' OFF'}  key ${NOTE[r.key.root]} ${SCALE_NAME[r.key.scaleIdx]}${kOk ? '' : ' (want ' + NOTE[c.root] + ' ' + SCALE_NAME[c.scaleIdx] + ')'}  roots ${rootOk}/16 full ${fullOk}/16  conf ${r.bpmConfidence}/${r.keyConfidence}/${r.chordConfidence}  ${ms.toFixed(0)} ms` +
      (verbose || fullOk < 16 ? `\n       got  ${JSON.stringify(r.spec)}\n       want ${JSON.stringify(c.spec)}` : '') + (verbose && r.notes.length ? `\n       notes: ${r.notes.join(' | ')}` : ''));
}
const pct = (a, b) => (b ? (100 * a / b).toFixed(1) : '0.0') + ' %';
console.log(`\n${stat.cases} synthetic cases`);
console.log(`tempo within 1 BPM        ${stat.tempoOk}/${stat.cases}  ${pct(stat.tempoOk, stat.cases)}   (octave errors ${stat.tempoOctave}, other errors ${stat.tempoWrong})`);
console.log(`first downbeat within 35ms ${stat.offsetOk}/${stat.cases}  ${pct(stat.offsetOk, stat.cases)}   (on a bar line, any bar: ${stat.downbeatOk})`);
console.log(`key root + scale          ${stat.keyOk}/${stat.cases}  ${pct(stat.keyOk, stat.cases)}   (root alone ${stat.keyRootOk})`);
console.log(`chord root per beat       ${stat.rootOk}/${stat.beats}  ${pct(stat.rootOk, stat.beats)}`);
console.log(`full chord per beat       ${stat.fullOk}/${stat.beats}  ${pct(stat.fullOk, stat.beats)}   (root + quality + bass)`);
console.log(`bass pitch class per beat ${stat.bassOk}/${stat.bassBeats}  ${pct(stat.bassOk, stat.bassBeats)}   (of the beats a bass was reported on)`);
console.log(`slowest analysis          ${stat.ms.toFixed(0)} ms`);
for (const g of Object.keys(group)) console.log(`  ${g.padEnd(5)} n ${group[g].n}  tempo ${pct(group[g].tempo, group[g].n)}  key ${pct(group[g].key, group[g].n)}  roots ${pct(group[g].root, group[g].n * 16)}  full ${pct(group[g].full, group[g].n * 16)}`);

if (!onlyArg) {
  check(stat.cases >= 60, `at least 60 cases (${stat.cases})`);
  check(stat.tempoOk / stat.cases >= 0.95, `tempo accuracy ${pct(stat.tempoOk, stat.cases)} is under 95 %`);
  check(stat.keyOk / stat.cases >= 0.90, `key accuracy ${pct(stat.keyOk, stat.cases)} is under 90 %`);
  check(stat.rootOk / stat.beats >= 0.90, `chord root accuracy ${pct(stat.rootOk, stat.beats)} is under 90 %`);
  check(stat.fullOk / stat.beats >= 0.80, `full chord accuracy ${pct(stat.fullOk, stat.beats)} is under 80 %`);
  check(stat.offsetOk / stat.cases >= 0.90, `downbeat accuracy ${pct(stat.offsetOk, stat.cases)} is under 90 %`);

  // ---------- a second set the analyser was not tuned on ----------
  // Progressions written by the generator itself (they often do not start on the tonic and mix in 7ths, sus
  // chords and inversions), random keys and scales, tempos that are not whole numbers, other drum patterns,
  // a recording tuned 30 cents sharp, heavy noise, and no drums at all. A key is only one of several right
  // answers for many of these, so the key is reported, not asserted; the chords have one right answer.
  const rnd01 = (() => { const r = lcg(777); return () => (r() + 1) / 2; })();
  const variants = { 'kick + hat': {}, 'kick + snare on 2 and 4': { snare: true }, 'kick on 1 and 3, snare on 2 and 4': { snare: true, backbeat: true }, 'tuned 30 cents sharp': { tune: 30 }, 'noisy, 9 kHz band': { noise: 0.05, lowpass: 9000 }, 'no drums': { noDrums: true, bassPerBeat: true } };
  console.log('\nsecond set: generator progressions, 10 per row');
  const all2 = { n: 0, tempo: 0, root: 0, drumN: 0, drumTempo: 0, drumRoot: 0, key: 0 };
  for (const [vn, v] of Object.entries(variants)) {
    const st = { n: 0, tempo: 0, oct: 0, off: 0, key: 0, same: 0, root: 0, full: 0 };
    for (let i = 0; i < 10; i++) {
      const root = Math.floor(rnd01() * 12), scaleIdx = [0, 0, 0, 1, 1, 1, 2, 5, 6, 0, 1, 3][Math.floor(rnd01() * 12)], key = keyOf(root, scaleIdx);
      const spec = Gen.progression({ key, seed: 1000 + Math.floor(rnd01() * 100000) }).spec.map(e => ({ ...e }));
      const bpm = Math.round((70 + rnd01() * 105) * 10) / 10, offset = rnd01() < 0.4 ? 0 : Math.round(rnd01() * 1500) / 1000;
      const a = synth({ root, scaleIdx, spec, bpm, offset, bassLow: rnd01() < 0.5, bassPerBeat: rnd01() < 0.5, seed: i + 1, ...v });
      const r = Listen.analyse(a.samples, a.sr, { gen: Gen }); checkShape(r, vn);
      const tOk = Math.abs(r.bpm - bpm) <= 1, got = expand(r.spec, keyOf(r.key.root, r.key.scaleIdx));
      let ro = 0, fu = 0; for (let b = 0; b < 16; b++) { const w = a.truth[b]; if (got[b].root === w.root) ro++; if (got[b].root === w.root && got[b].set === w.set && got[b].bass === w.bass) fu++; }
      const pcs = k => SCALES[k.scaleIdx].map(x => (x + k.root) % 12).sort().join();
      st.n++; st.tempo += tOk ? 1 : 0; st.oct += !tOk && (Math.abs(r.bpm - 2 * bpm) <= 2 || Math.abs(r.bpm - bpm / 2) <= 0.5) ? 1 : 0; st.off += Math.abs(r.beatOffsetSec - offset) <= 0.035 ? 1 : 0;
      st.key += r.key.root === root && r.key.scaleIdx === scaleIdx ? 1 : 0; st.same += pcs(r.key) === pcs({ root, scaleIdx }) ? 1 : 0; st.root += ro; st.full += fu;
      if (verbose && (!tOk || ro < 16)) console.log(`   ${vn}: ${NOTE[root]} ${SCALE_NAME[scaleIdx]} ${bpm} -> ${r.bpm} ${NOTE[r.key.root]} ${SCALE_NAME[r.key.scaleIdx]} roots ${ro} full ${fu}\n      want ${JSON.stringify(spec)}\n      got  ${JSON.stringify(r.spec)}`);
    }
    console.log(`  ${vn.padEnd(34)} tempo ${pct(st.tempo, st.n)} (octave errors ${st.oct})  downbeat ${pct(st.off, st.n)}  key exact ${pct(st.key, st.n)}, same seven notes ${pct(st.same, st.n)}  chord roots ${pct(st.root, st.n * 16)}  full chords ${pct(st.full, st.n * 16)}`);
    all2.n += st.n; all2.tempo += st.tempo; all2.root += st.root; all2.key += st.key;
    if (!v.noDrums) { all2.drumN += st.n; all2.drumTempo += st.tempo; all2.drumRoot += st.root; }
  }
  console.log(`  all ${all2.n}: tempo ${pct(all2.tempo, all2.n)}, key exact ${pct(all2.key, all2.n)}, chord roots ${pct(all2.root, all2.n * 16)}; with drums: tempo ${pct(all2.drumTempo, all2.drumN)}, chord roots ${pct(all2.drumRoot, all2.drumN * 16)}`);
  check(all2.drumTempo / all2.drumN >= 0.9, `second set, with drums: tempo ${pct(all2.drumTempo, all2.drumN)} is under 90 %`);
  check(all2.drumRoot / (all2.drumN * 16) >= 0.9, `second set, with drums: chord roots ${pct(all2.drumRoot, all2.drumN * 16)} is under 90 %`);

  // ---------- behaviour ----------
  const c0 = cases[1], a0 = synth(c0);
  const r1 = Listen.analyse(a0.samples, a0.sr, { gen: Gen }), r2 = Listen.analyse(a0.samples, a0.sr, { gen: Gen }), r3 = Listen.analyse(a0.samples, a0.sr);
  check(JSON.stringify(r1) === JSON.stringify(r2), 'the same audio gives the same answer twice');
  check(JSON.stringify(r1) === JSON.stringify(r3), 'the built-in chord maths gives the same answer as M1D1Gen.chordTones');
  const copy = Float32Array.from(a0.samples); Listen.analyse(copy, a0.sr);
  check(copy.every((v, i) => v === a0.samples[i]), 'the input samples are left as they were');
  check(typeof globalThis.M1D1Listen === 'object' && globalThis.M1D1Listen.analyse === Listen.analyse, 'one global, M1D1Listen');
  const src = require('fs').readFileSync(path.join(__dirname, 'm1d1-listen.js'), 'utf8').replace(/\/\/.*$/gm, '');
  check(!/Math\.random|document\.|window\.|Date\.now|performance\./.test(src), 'no Math.random, DOM or clock in the analyser');

  // 60 s (and more: only the first minute is listened to) in well under 2 s
  const long = synth({ root: 9, scaleIdx: 0, spec: MINOR[0], bpm: 174, bars: 46, seed: 5 });
  let t0 = process.hrtime.bigint(); const rl = Listen.analyse(long.samples, long.sr, { gen: Gen }); let ms = Number(process.hrtime.bigint() - t0) / 1e6;
  console.log(`63 s at 174 BPM, 44.1 kHz: ${ms.toFixed(0)} ms, bpm ${rl.bpm}, key ${NOTE[rl.key.root]} ${SCALE_NAME[rl.key.scaleIdx]}`);
  check(ms < 2000, `60 s of audio took ${ms.toFixed(0)} ms`); check(Math.abs(rl.bpm - 174) <= 1, `long take tempo ${rl.bpm}`); check(rl.notes.some(s => /60 seconds/.test(s)), 'long take says only a minute was heard');
  const long48 = synth({ root: 4, scaleIdx: 1, spec: MAJOR[0], bpm: 70, bars: 18, sr: 48000, seed: 6 });
  t0 = process.hrtime.bigint(); const rl2 = Listen.analyse(long48.samples, long48.sr, { gen: Gen }); ms = Number(process.hrtime.bigint() - t0) / 1e6;
  console.log(`62 s at 70 BPM, 48 kHz: ${ms.toFixed(0)} ms, bpm ${rl2.bpm}`);
  check(ms < 2000, `60 s of 48 kHz audio took ${ms.toFixed(0)} ms`); check(Math.abs(rl2.bpm - 70) <= 1, `long 48k take tempo ${rl2.bpm}`);

  // input that cannot be analysed still gives a valid, honest answer
  const odd = {
    'empty': [new Float32Array(0), 44100], 'silence': [new Float32Array(44100 * 5), 44100], 'null': [null, 44100], 'bad rate': [new Float32Array(1000).fill(0.1), 0],
    'NaN': [new Float32Array(44100).fill(NaN), 44100], 'half a second': [synth({ root: 0, scaleIdx: 1, spec: MAJOR[0], bpm: 120, bars: 8 }).samples.subarray(0, 22050), 44100],
    'noise': [(() => { const r = lcg(9), x = new Float32Array(44100 * 8); for (let i = 0; i < x.length; i++) x[i] = 0.3 * r(); return x; })(), 44100],
    'one held chord': [synth({ root: 0, scaleIdx: 1, spec: P([0, 8], [0, 8]), bpm: 120, bars: 4, noDrums: true }).samples, 44100],
    '8 kHz audio': [synth({ root: 0, scaleIdx: 1, spec: MAJOR[0], bpm: 120, bars: 8, sr: 8000 }).samples, 8000],
  };
  for (const [name, [x, sr]] of Object.entries(odd)) {
    let r; try { r = Listen.analyse(x, sr, { gen: Gen }); checkShape(r, name); } catch (e) { check(false, `${name}: ${e.message}`); continue; }
    if (['empty', 'silence', 'null', 'bad rate', 'NaN', 'noise'].includes(name)) check(r.notes.length > 0 && (r.bpmConfidence === 0 || r.chordConfidence < 0.5), `${name}: says it could not listen (${JSON.stringify(r.notes)}, conf ${r.bpmConfidence}/${r.chordConfidence})`);
    if (verbose) console.log(`  ${name}: bpm ${r.bpm} conf ${r.bpmConfidence}/${r.keyConfidence}/${r.chordConfidence} notes ${JSON.stringify(r.notes)}`);
  }
  const held = Listen.analyse(odd['one held chord'][0], 44100, { gen: Gen });
  check(held.key.root === 0 && held.key.scaleIdx === 1 && held.spec.every(e => e.d === 0), `one held C major chord reads as C major (${JSON.stringify(held.key)} ${JSON.stringify(held.spec)})`);
  // an out-of-key chord is flagged, not silently mislabelled
  const borrowed = synth({ root: 0, scaleIdx: 1, spec: P([0, 4], [3, 4], [0, 4], [4, 4]), bpm: 120, bars: 8, seed: 11 });
  { // put an Ab major chord (outside C major) over bar 3 of each loop by synthesizing it separately
    const key = keyOf(0, 1), sr = 44100, beat = 0.5, x = new Float64Array(borrowed.samples.length), flat6 = [56, 60, 63];
    const base = synth({ root: 0, scaleIdx: 1, spec: P([0, 4], [3, 4], [0, 4], [4, 4]), bpm: 120, bars: 8, seed: 11, noDrums: false });
    void key; void base;
    const dry = synth({ root: 0, scaleIdx: 1, spec: P([0, 4], [3, 4]), bpm: 120, bars: 2, seed: 11 }).samples, tail = synth({ root: 0, scaleIdx: 1, spec: P([4, 4], [4, 4]), bpm: 120, bars: 1, seed: 11 }).samples;
    const drums = synth({ root: 0, scaleIdx: 1, spec: P([0, 4], [0, 4]), bpm: 120, bars: 1, seed: 11 }).samples;
    void drums;
    for (let loop = 0; loop < 2; loop++) {
      const o = Math.round(loop * 16 * beat * sr);
      for (let i = 0; i < 8 * beat * sr; i++) x[o + i] += dry[i];
      for (const m of flat6) for (const cents of [-4, 4]) addTone(x, sr, loop * 8 + 4, 2, 440 * Math.pow(2, (m - 69 + cents / 100) / 12), 0.055, 8, padEnv);
      addTone(x, sr, loop * 8 + 4, 2, 440 * Math.pow(2, (44 - 69) / 12), 0.2, 6, padEnv);
      for (let i = 0; i < 4 * beat * sr; i++) x[o + Math.round(12 * beat * sr) + i] += tail[i];
    }
    const r = Listen.analyse(Float32Array.from(x), sr, { gen: Gen });
    checkShape(r, 'borrowed chord');
    check(r.key.root === 0 && r.key.scaleIdx === 1, `borrowed bVI: key stays C major (${NOTE[r.key.root]} ${SCALE_NAME[r.key.scaleIdx]})`);
    check(r.notes.some(s => /outside the key.*bar 3/.test(s)), `borrowed bVI in bar 3 is reported (${JSON.stringify(r.notes)})`);
    if (verbose) console.log('  borrowed:', JSON.stringify(r.spec), r.notes);
  }
}
console.log(failures ? `\n${failures} FAILED` : '\nall assertions passed');
process.exit(failures ? 1 : 0);
