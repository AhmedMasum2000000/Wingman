// "Listen to track" inside a DAW: the analyser is given the host's tempo and the song beat the audio starts on.
// Scores the chords as the plugin would PLAY them: the plugin plays the 16-beat progression at song beat
// (host ppq) mod 16, so a chord counts as right only if it is right at that place in the song.
// Run: node test-grid.js [-v]
'use strict';
const path = require('path');
const Gen = require(path.join(__dirname, 'm1d1-gen.js'));
const Listen = require(path.join(__dirname, 'm1d1-listen.js'));
const verbose = process.argv.includes('-v');
const SCALES = [[0, 2, 3, 5, 7, 8, 10], [0, 2, 4, 5, 7, 9, 11], [0, 2, 3, 5, 7, 9, 10], [0, 1, 3, 5, 7, 8, 10], [0, 2, 4, 6, 7, 9, 11], [0, 2, 4, 5, 7, 9, 10], [0, 2, 3, 5, 7, 8, 11]];
const keyOf = (root, scaleIdx) => ({ root, st: SCALES[scaleIdx], minor: SCALES[scaleIdx][2] === 3 });
const mod = (a, n) => ((a % n) + n) % n;

function lcg(seed) { let s = (seed * 2654435761) % 2147483647 || 1; return () => ((s = (s * 16807) % 2147483647) / 2147483647) * 2 - 1; }
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

// A song: `prog` (a list of {d, len} covering 16 or 32 beats) repeated; the capture starts at song beat `startBeat`
// and lasts `beats` beats. Returns the captured mono audio and the true chord root at every song beat.
function song(c) {
  const sr = 22050, beat = 60 / c.bpm, key = keyOf(c.root, c.scaleIdx), L = c.prog.reduce((a, e) => a + e.len, 0);
  const n = Math.ceil(c.beats * beat * sr), out = new Float64Array(n), rnd = lcg(c.seed || 3);
  const chordAt = b => { let at = 0, sb = mod(b, L); for (const el of c.prog) { if (sb < at + el.len) return { el, first: b - (sb - at), len: el.len }; at += el.len; } };
  const rootAt = b => Gen.chordTones(chordAt(b).el, key).tones.root;
  for (let b = Math.floor(c.startBeat) - 4; b < c.startBeat + c.beats + 4; b++) {
    const ch = chordAt(b); if (ch.first !== b) continue; if (c.intro && b < c.intro) continue;   // intro: drums only before song beat c.intro
    const ct = Gen.chordTones(ch.el, key), t0 = (b - c.startBeat) * beat;
    if (c.only !== 'bass') for (const m0 of ct.notes) { let m = m0; while (m > 71) m -= 12; const hz = 440 * Math.pow(2, (m - 69) / 12); for (const cents of [-4, 4]) addTone(out, sr, t0, ch.len * beat, hz * Math.pow(2, cents / 1200), 0.05, 8, c.stabs ? pluckEnv : padEnv); }
    const bhz = 440 * Math.pow(2, (36 + ct.pcs[0] - 69) / 12);
    if (c.only !== 'chords') for (let k = 0; k < ch.len; k++) addTone(out, sr, t0 + k * beat, beat * 0.9, bhz, 0.2, 6, pluckEnv);
  }
  if (c.drums) for (let b = Math.ceil(c.startBeat); b < c.startBeat + c.beats; b++) {
    const t0 = (b - c.startBeat) * beat, i0 = Math.round(t0 * sr);
    for (let i = 0; i < 0.18 * sr && i0 + i < n; i++) { const t = i / sr; out[i0 + i] += 0.5 * Math.sin(2 * Math.PI * (50 + 90 * Math.exp(-t * 30)) * t) * Math.exp(-t * 18); }
    const h0 = Math.round((t0 + beat / 2) * sr); for (let i = 0; i < 0.04 * sr && h0 + i < n; i++) out[h0 + i] += 0.06 * rnd() * Math.exp(-i / sr * 90);
  }
  let peak = 0; for (const v of out) peak = Math.max(peak, Math.abs(v));
  const f = new Float32Array(n); for (let i = 0; i < n; i++) f[i] = out[i] / peak * 0.8;
  return { samples: f, sr, rootAt, key, L };
}
function expandRoots(spec, key) { const out = []; for (const el of spec) { const r = Gen.chordTones(el, key).tones.root; for (let b = 0; b < el.len; b++) out.push(r); } return out; }

// how the plugin plays a result: progression beat (songBeat mod 16)
// every beat of one whole cycle of the song (4 or 8 bars), as the plugin plays it: progression beat = song beat mod its length
function asPlayed(res, s, c, windowStart) {
  const roots = expandRoots(res.spec, { root: res.key.root, st: SCALES[res.key.scaleIdx], minor: SCALES[res.key.scaleIdx][2] === 3 });
  let ok = 0; for (let b = windowStart; b < windowStart + s.L; b++) if (roots[mod(b, roots.length)] === s.rootAt(b)) ok++;
  return ok;
}
const P = (...e) => e.map(([d, len, x]) => ({ d, len, ...(x || {}) }));
module.exports = { song, expandRoots, P, SCALES };
if (require.main === module) {
const cases = [];
const progs = {
  pop: P([0, 4], [5, 4], [3, 4], [4, 4]),
  minorPush: P([0, 4], [5, 4], [2, 4], [6, 2], [4, 2]),
  twoPerBar: P([0, 2], [4, 2], [5, 2], [3, 2], [0, 2], [4, 2], [3, 4]),
  eightBar: P([0, 4], [4, 4], [5, 4], [3, 4], [1, 4], [4, 4], [0, 4], [4, 4]),
};
let seed = 1;
for (const [name, prog] of Object.entries(progs))
  for (const bpm of [72, 96, 128, 150, 174])
    for (const startBeat of [0, 6, 21])
      cases.push({ name: `${name} ${bpm} BPM from beat ${startBeat}`, prog, bpm, startBeat, beats: Math.round(60 * bpm / 60),   // the plugin keeps 64 s; 60 s are read
 root: (seed * 5) % 12, scaleIdx: seed % 2, drums: seed % 3 !== 0, stabs: seed % 4 === 0, seed: seed++ });

let free = 0, grid = 0, total = 0, keyFree = 0, keyGrid = 0, tempoFree = 0, fails = [];
for (const c of cases) {
  const s = song(c), bpb = 4;
  // the window the plugin plays against: the latest full 4-bar line of the song inside the capture
  const end = c.startBeat + c.beats, Lt = s.L, wl = Math.floor(end / Lt) * Lt - Lt, win = wl >= c.startBeat ? wl : Math.ceil(c.startBeat / Lt) * Lt;
  const rf = Listen.analyse(s.samples, s.sr, { gen: Gen });
  const rg = Listen.analyse(s.samples, s.sr, { gen: Gen, grid: { bpm: c.bpm, firstBeatSec: 0, startBeat: c.startBeat, beatsPerBar: bpb } });
  // free mode: as the old plugin played it (its spec at ppq mod 16, its own downbeat ignored, as the page did)
  const okF = asPlayed(rf, s, c, win), okG = asPlayed(rg, s, c, win);
  free += okF; grid += okG; total += s.L;
  if (rf.key.root === s.key.root) keyFree++; if (rg.key.root === s.key.root) keyGrid++;
  if (Math.abs(rf.bpm - c.bpm) <= 1) tempoFree++;
  if (okG < s.L) fails.push(`${c.name}: ${okG}/${s.L} (${rg.notes.join('; ')})`);
  if (verbose) console.log(`${c.name.padEnd(36)} free ${String(okF).padStart(2)}/${s.L} bpm ${rf.bpm}   grid ${String(okG).padStart(2)}/${s.L}  (${rg.spec.reduce((a, e) => a + e.len, 0)} beats read)`);
}
const pct = (a, b) => (100 * a / b).toFixed(1) + ' %';
console.log(`${cases.length} songs. Chord root right where the plugin plays it, every beat of a whole cycle of the song:`);
console.log(`  before (analyser guesses tempo and downbeat): ${pct(free, total)}   tempo right ${pct(tempoFree, cases.length)}   key root ${pct(keyFree, cases.length)}`);
console.log(`  host grid (host tempo, song beat):             ${pct(grid, total)}   key root ${pct(keyGrid, cases.length)}`);
if (fails.length) { console.log('below 16/16 with the host grid:'); fails.forEach(f => console.log('  ' + f)); }
process.exitCode = grid / total >= 0.97 ? 0 : 1;
}
