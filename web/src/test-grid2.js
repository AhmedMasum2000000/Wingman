// "Listen to track" in harder host situations: node test-grid2.js
'use strict';
const assert = require('assert');
const Gen = require('./m1d1-gen.js'), Listen = require('./m1d1-listen.js'), G = require('./test-grid.js');
const SC = G.SCALES, mod = (a, n) => ((a % n) + n) % n;
const keyOfRes = r => ({ root: r.key.root, st: SC[r.key.scaleIdx], minor: SC[r.key.scaleIdx][2] === 3 });
const pcsOf = (el, key) => [...new Set(Gen.chordTones(el, key).pcs)].sort((a, b) => a - b).join(',');
function played(res) {   // the chord the plugin plays at each progression beat
  const key = keyOfRes(res), out = []; for (const el of res.spec) { const ct = Gen.chordTones(el, key); for (let b = 0; b < el.len; b++) out.push({ root: ct.tones.root, pcs: [...new Set(ct.pcs)].sort((a, b) => a - b).join(',') }); }
  return out;
}
let failures = 0; const check = (ok, msg) => { console.log((ok ? 'ok    ' : 'FAIL  ') + msg); if (!ok) failures++; };

// 1. a song in 3/4: four bars of three beats, read on the host's 3/4 grid
{
  const c = { prog: G.P([0, 3], [5, 3], [3, 3], [4, 3]), bpm: 96, startBeat: 5, beats: 60, root: 7, scaleIdx: 1, drums: true, seed: 11 };
  const s = G.song(c);
  const r = Listen.analyse(s.samples, s.sr, { gen: Gen, grid: { bpm: c.bpm, firstBeatSec: 0, startBeat: c.startBeat, beatsPerBar: 3 } });
  const p = played(r), L = p.length; let ok = 0; for (let b = 24; b < 36; b++) if (p[mod(b, L)].root === s.rootAt(b)) ok++;
  check(L === 12 && ok === 12, `3/4: ${L} beats read, ${ok}/12 roots right where played`);
}
// 2. a 3-bar loop in the host: every pass is at song beats 0-11, the capture carries a segment per pass
{
  const c = { prog: G.P([0, 4], [5, 4], [4, 4]), bpm: 120, startBeat: 0, beats: 48, root: 9, scaleIdx: 0, drums: true, seed: 12 };
  const s = G.song(c), period = 60 / c.bpm, segments = [0, 1, 2, 3].map(i => ({ sec: i * 12 * period, beat: 0 }));
  const r = Listen.analyse(s.samples, s.sr, { gen: Gen, grid: { bpm: c.bpm, firstBeatSec: 0, startBeat: 0, beatsPerBar: 4, segments } });
  const p = played(r); let ok = 0; for (let b = 0; b < 12; b++) if (p[mod(b, p.length)].root === s.rootAt(b)) ok++;
  check(ok === 12, `3-bar host loop: ${ok}/12 roots right at ppq 0-11 (progression ${p.length} beats)`);
  // without segments the same audio would be read as if the song went on, and the chords land in the wrong place
  const r0 = Listen.analyse(s.samples, s.sr, { gen: Gen, grid: { bpm: c.bpm, firstBeatSec: 0, startBeat: 0, beatsPerBar: 4 } });
  const p0 = played(r0); let ok0 = 0; for (let b = 0; b < 12; b++) if (p0[mod(b, p0.length)].root === s.rootAt(b)) ok0++;
  console.log(`      (without segments: ${ok0}/12)`);
}
// 3. chords spread wide in stereo (all in the side), bass in the middle: the chord qualities still come through.
//    E major in A minor (the major V) cannot be told from the bass alone.
{
  const c = { prog: G.P([0, 4], [5, 4], [3, 4], [4, 4, { q: 'maj' }]), bpm: 110, startBeat: 0, beats: 48, root: 9, scaleIdx: 0, drums: false, seed: 13 };
  const bass = G.song({ ...c, only: 'bass' }), chords = G.song({ ...c, only: 'chords' });
  const grid = { bpm: c.bpm, firstBeatSec: 0, startBeat: 0, beatsPerBar: 4 };
  const rMid = Listen.analyse(bass.samples, bass.sr, { gen: Gen, grid });
  const rBoth = Listen.analyse(bass.samples, bass.sr, { gen: Gen, side: chords.samples, grid });
  const key = { root: c.root, st: SC[c.scaleIdx], minor: SC[c.scaleIdx][2] === 3 };
  const truthPcs = b => { let at = 0; for (const el of c.prog) { if (mod(b, 16) < at + el.len) return pcsOf(el, key); at += el.len; } };
  const score = r => { const p = played(r); let ok = 0; for (let b = 0; b < 16; b++) if (p[b % p.length].pcs === truthPcs(b)) ok++; return ok; };
  const a = score(rMid), bb = score(rBoth);
  check(bb >= 15 && bb > a, `stereo-wide chords: full chords right ${bb}/16 with the side channel (${a}/16 from the middle only)`);
}
// 4. audio that does not follow the project's tempo is reported
{
  const c = { prog: G.P([0, 4], [5, 4], [3, 4], [4, 4]), bpm: 122, startBeat: 0, beats: 64, root: 0, scaleIdx: 1, drums: true, seed: 14 };
  const s = G.song(c);
  const r = Listen.analyse(s.samples, s.sr, { gen: Gen, grid: { bpm: 130, firstBeatSec: 0, startBeat: 0, beatsPerBar: 4 } });
  check(r.audioBpm > 0 && Math.abs(r.audioBpm - 122) < 1.5 && r.notes.some(n => n.includes('not the project')), `122 BPM audio in a 130 BPM project: reported as ${r.audioBpm} BPM`);
  const r2 = Listen.analyse(s.samples, s.sr, { gen: Gen, grid: { bpm: 122, firstBeatSec: 0, startBeat: 0, beatsPerBar: 4 } });
  check(r2.audioBpm === 0, 'the same audio in a 122 BPM project: no warning');
}
// 5. a song that starts with 12 bars of drums only: the drums alone give no chords; once the song plays, the drums-only
//    bars are left out (a 4-bar progression, not 8)
{
  const c = { prog: G.P([0, 4], [5, 4], [2, 4], [6, 4]), bpm: 150, startBeat: 0, beats: 200, root: 9, scaleIdx: 0, drums: true, seed: 7, intro: 48 };
  const s = G.song(c), grid = { bpm: c.bpm, firstBeatSec: 0, startBeat: 0, beatsPerBar: 4 };
  const upTo = beat => s.samples.slice(0, Math.round(beat * 60 / c.bpm * s.sr));
  for (const end of [17.8, 35.3, 47.5]) { const r = Listen.analyse(upTo(end), s.sr, { gen: Gen, grid }); check(r.pitched === false, `drums only, ${end} beats: no chords (tonality ${r.tonality}, key ${r.keyConfidence})`); }
  for (const end of [70.3, 105.3]) {
    const r = Listen.analyse(upTo(end), s.sr, { gen: Gen, grid }), p = played(r), L = p.length; let ok = 0; for (let b = 48; b < 64; b++) if (p[mod(b, L)].root === s.rootAt(b)) ok++;
    check(r.pitched && L === 16 && ok === 16 && !r.notes.some(n => /longer than/.test(n)), `drums intro then the song, ${end} beats: ${L} beats read, ${ok}/16 roots right`);
  }
}
// 6. a tuned kick (an 808) on every beat and nothing else: one note, so the key and chords are flagged as a guess
{
  const sr = 22050, bpm = 140, beats = 48, n = Math.ceil(beats * 60 / bpm * sr), x = new Float32Array(n);
  for (let b = 0; b < beats; b++) { const i0 = Math.round(b * 60 / bpm * sr); for (let i = 0; i < sr && i0 + i < n; i++) { const t = i / sr, f = 55 * (1 + 1.5 * Math.exp(-t * 40)); x[i0 + i] += 0.8 * Math.tanh(2.5 * Math.sin(2 * Math.PI * f * t) * Math.exp(-t / 0.5)); } }
  const r = Listen.analyse(x, sr, { gen: Gen, grid: { bpm, firstBeatSec: 0, startBeat: 0, beatsPerBar: 4 } });
  check(r.oneNote === true && r.keyConfidence <= 0.3 && r.notes.some(m => /only one note/.test(m)), `808 on every beat: one note (${r.oneNote}), key confidence ${r.keyConfidence}`);
  const c = { prog: G.P([0, 16]), bpm: 140, startBeat: 0, beats: 48, root: 5, scaleIdx: 1, drums: true, seed: 140 };
  const v = G.song(c), rv = Listen.analyse(v.samples, v.sr, { gen: Gen, grid: { bpm: c.bpm, firstBeatSec: 0, startBeat: 0, beatsPerBar: 4 } });
  check(rv.pitched && !rv.oneNote, `a one-chord vamp with bass and drums is still read as chords`);
}
console.log(failures ? `${failures} failed` : 'all passed');
process.exitCode = failures ? 1 : 0;
