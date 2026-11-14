/* M1D1Gen: the part generators and the keep-aware chord solver of the M1D1 preview.
   A part is a pattern (style) played over a list of chords with a seed. The solver writes new chords that fit the
   parts the user keeps. Everything is pure and deterministic: the same inputs always give the same MIDI. */
(function (root) {
  'use strict';

  // ---------- constants ----------
  const RANGES = [[33, 52], [48, 74], [64, 86]];           // bass A1-E3, chords C3-D5, lead E4-D6
  const STYLES = [
    [
      { id: 'pulse', label: 'Pulse', hint: 'Steady notes on the chord’s bass note, with octave pops on the off-beats.' },
      { id: 'octave', label: 'Octaves', hint: 'Bounces between the bass note and its octave, disco and house style.' },
      { id: 'walk', label: 'Walking', hint: 'A quarter-note line through the chord tones that steps into each next chord.' },
      { id: 'sync', label: 'Synco', hint: 'Syncopated: pushes and rests between the beats, for house, garage and trap.' },
      { id: 'sub', label: 'Sub', hint: 'Long held sub notes, struck once or twice per chord.' },
    ],
    [
      { id: 'block', label: 'Block', hint: 'Held chords, voiced close together, one strike per change.' },
      { id: 'stabs', label: 'Stabs', hint: 'Short chord stabs, mostly on the off-beats.' },
      { id: 'pulse', label: 'Pulse', hint: 'Repeated eighth-note chords with accents.' },
      { id: 'broken', label: 'Broken', hint: 'Each chord played one note at a time, like a picked or arpeggiated part.' },
      { id: 'wide', label: 'Wide', hint: 'Open, spread voicings held long, with the bass note set low.' },
    ],
    [
      { id: 'arp', label: 'Arp', hint: 'An arpeggio up and down the chord tones.' },
      { id: 'melody', label: 'Melody', hint: 'A singable top line: chord tones on the beat, steps in between, question and answer.' },
      { id: 'hook', label: 'Hook', hint: 'A short motif repeated and bent to fit each chord.' },
      { id: 'sparse', label: 'Sparse', hint: 'A few long notes floating over the chords.' },
    ],
  ];
  const EPS = 1e-6;

  // ---------- small helpers ----------
  const mod = (a, n) => ((a % n) + n) % n;
  const clamp01 = v => (Number.isFinite(+v) ? Math.min(1, Math.max(0, +v)) : 0);
  const uniq = a => a.filter((v, i) => a.indexOf(v) === i);
  function sanitize(seed) {
    let s = Math.floor(Number(seed));
    if (!Number.isFinite(s)) s = 1;
    return mod(s, 2147483646) + 1;                           // 1 .. 2147483646
  }
  // the preview's Park-Miller generator
  // the first draws of a small seed are tiny, so a few are thrown away before any are used
  function rngOf(seed) { let s = sanitize(seed); for (let k = 0; k < 3; k++) s = (s * 16807) % 2147483647; return () => (s = (s * 16807) % 2147483647) / 2147483647; }
  const pick = (r, a) => a[Math.min(a.length - 1, Math.floor(r() * a.length))];
  const isStrong = n => Math.abs(n.x - Math.round(n.x)) < 0.1 || n.len >= 0.95;

  function normKey(key) {
    const k = key || {};
    const st = Array.isArray(k.st) && k.st.length === 7 && k.st.every(Number.isFinite) ? k.st.map(x => mod(Math.round(x), 12)) : [0, 2, 3, 5, 7, 8, 10];
    return { root: mod(Math.round(Number(k.root) || 0), 12), st, minor: typeof k.minor === 'boolean' ? k.minor : st[2] === 3 };
  }
  const keySet = K => new Set(K.st.map(s => mod(K.root + s, 12)));
  function fold(m, lo, hi) { m = Math.round(m); while (m > hi) m -= 12; while (m < lo) m += 12; return m; }
  function ladder(pcs, lo, hi) { const out = []; for (let m = lo; m <= hi; m++) if (pcs.includes(mod(m, 12))) out.push(m); return out; }
  function nearIn(arr, ref) { let best = arr[0]; for (const a of arr) if (Math.abs(a - ref) < Math.abs(best - ref) - EPS) best = a; return best === undefined ? ref : best; }
  function nearPc(pc, ref, lo, hi) { const l = ladder([mod(pc, 12)], lo, hi); return l.length ? nearIn(l, ref) : fold(pc, lo, hi); }
  function stepFrom(m, dir, KS) { let x = m + dir; for (let g = 0; g < 12 && !KS.has(mod(x, 12)); g++) x += dir; return x; }

  // ---------- chords ----------
  // the chord of one progression element, with the exact maths of the preview's buildChord (plus inv 2)
  function chordTones(s, key) {
    const K = normKey(key), sc = K.st, el = s || {}, d = mod(Math.round(Number(el.d) || 0), 7);
    const deg = k => sc[(d + k) % 7] + 12 * Math.floor((d + k) / 7);
    let iv;
    if (el.q === 'dom7') iv = [0, 4, 7, 10]; else if (el.q === 'sus4') iv = [0, 5, 7]; else if (el.q === 'maj') iv = [0, 4, 7];
    else { const r0 = deg(0); iv = [0, deg(2) - r0, deg(4) - r0]; if (el.ext === 7) iv.push(deg(6) - r0); }
    const rootPc = (K.root + sc[d]) % 12;
    let notes = iv.map(x => 48 + rootPc + x);
    if (el.inv === 1) notes = [...notes.slice(1), notes[0] + 12];
    else if (el.inv === 2) notes = [...notes.slice(2), notes[0] + 12, notes[1] + 12];
    const tones = { root: rootPc, third: (rootPc + iv[1]) % 12, fifth: (rootPc + iv[2]) % 12 };
    if (iv[3] !== undefined) tones.seventh = (rootPc + iv[3]) % 12;
    return { pcs: notes.map(n => n % 12), notes, tones };
  }
  function quality(t) {
    const th = mod(t.third - t.root, 12), fi = mod(t.fifth - t.root, 12);
    return th === 3 ? (fi === 6 ? 'dim' : 'min') : th === 4 ? (fi === 8 ? 'aug' : 'maj') : 'sus';
  }
  // lay a chord list out in time: where each chord starts, its tones, its bass and its tones that are safe on top
  function lay(chords) {
    let at = 0; const out = [];
    (Array.isArray(chords) ? chords : []).forEach((c, ci) => {
      const len = c && Number.isFinite(+c.len) ? Math.max(0, +c.len) : 0; if (len <= 0) return;
      let notes = c && Array.isArray(c.notes) ? c.notes.filter(Number.isFinite).map(Math.round) : [];
      if (!notes.length) notes = [48];
      const pcs = uniq(notes.map(n => mod(n, 12)));
      // a tone a semitone above another chord tone (the root over a maj7) clashes on top, so the lead avoids it on the beat
      const safe = pcs.filter(p => !pcs.includes(mod(p - 1, 12)));
      out.push({ at, len, end: at + len, ci, notes, pcs, bass: mod(notes[0], 12), safe: safe.length ? safe : pcs, inv: typeof c.n === 'string' && c.n.includes('/') });
      at += len;
    });
    return out;
  }
  function chordAt(L, x) { for (const ch of L) if (x < ch.end - EPS) return ch; return L[L.length - 1]; }

  // ---------- note helpers shared by the patterns ----------
  // give each slot a length up to the next slot (or the chord end), scaled by frac, or a fixed length
  function legato(slots, end, frac, out, fixed) {
    slots.sort((a, b) => a.x - b.x);
    slots.forEach((s, j) => { const nx = j + 1 < slots.length ? slots[j + 1].x : end; out.push({ x: s.x, len: fixed ? Math.min(fixed, nx - s.x) : (nx - s.x) * frac, m: s.m, v: s.v, c: s.c }); });
  }
  // a note that leads into target: a semitone away (chromatic) or the next scale tone (diatonic), from the side we come from
  function lead_in(target, from, chroma, KS) {
    let dir = from <= target ? -1 : 1;
    const go = d => (chroma ? target + d : stepFrom(target, d, KS));
    if (go(dir) < 33 || go(dir) > 52) dir = -dir;           // stay inside the bass range rather than jump an octave
    return go(dir);
  }
  function popUp(m, ch) { if (m + 12 <= 52) return m + 12; const up = ladder(ch.pcs, m + 3, 52); return up.length ? up[up.length - 1] : m; }
  function bassReg(L) { let prev = 40; return L.map(ch => (prev = nearPc(ch.bass, prev, 33, 47))); }

  // ---------- bass patterns ----------
  const BASS = {
    pulse(ctx) {
      const { L, r, KS } = ctx, reg = bassReg(L), out = [];
      const pats = [[1, 1, 1, 1], [0, 1, 0, 1], [1, 0, 1, 0], [0, 0, 1, 1], [0, 0, 0, 1], [1, 0, 0, 1]];
      const chroma = r() < 0.5;
      L.forEach((ch, k) => {
        const m0 = reg[k], last = k === L.length - 1, nb = last ? null : reg[k + 1], pat = pick(r, pats), useApp = r() < 0.6, slots = [];
        for (let b = 0, s = 0; b < ch.len - EPS; b += 0.5, s++) {
          const on = s % 2 === 0, pr = b === 0 ? 0 : on ? 0.12 + r() * 0.2 : 0.3 + r() * 0.45, pop = r();
          if (!ctx.incl(pr)) continue;
          let m = m0, v = b === 0 ? 108 : on ? 96 : 82;
          if (!on && pat[(s >> 1) % 4]) { m = popUp(m0, ch); v += 6; } else if (on && b > 0 && pop < 0.18) m = popUp(m0, ch);
          slots.push({ x: ch.at + b, m, v, c: ch.ci, on });
        }
        const tail = slots[slots.length - 1];
        if (nb !== null && useApp && slots.length > 1 && !tail.on && ch.end - tail.x <= 0.5 + EPS) { tail.m = lead_in(nb, m0, chroma, KS); tail.v = 86; }
        legato(slots, ch.end, 0.85, out);
      });
      return out;
    },
    octave(ctx) {
      const { L, r } = ctx, reg = bassReg(L), out = [];
      const pats = [[0, 1, 0, 1], [0, 1, 1, 0], [0, 1, 0, 0], [0, 0, 1, 1], [0, 1, 1, 1], [1, 0, 0, 1]];
      const fifthy = r() < 0.4;
      L.forEach((ch, k) => {
        const lo = reg[k], hi = popUp(lo, ch), pat = pick(r, pats), fif = ladder(ch.pcs, lo + 5, lo + 8)[0], slots = [];
        for (let b = 0, s = 0; b < ch.len - EPS; b += 0.5, s++) {
          const on = s % 2 === 0, pr = b === 0 ? 0 : on ? 0.1 + r() * 0.2 : 0.2 + r() * 0.35, alt = r();
          if (!ctx.incl(pr)) continue;
          let m = b === 0 ? lo : pat[s % 4] ? hi : lo;
          if (m === hi && fifthy && fif !== undefined && alt < 0.3) m = fif;
          slots.push({ x: ch.at + b, m, v: m === lo ? (on ? 104 : 88) : (on ? 92 : 100), c: ch.ci });
        }
        legato(slots, ch.end, 0.7, out);
      });
      return out;
    },
    walk(ctx) {
      const { L, r, KS } = ctx, reg = bassReg(L), out = [];
      const chroma = r() < 0.55;
      L.forEach((ch, k) => {
        const lad = ladder(ch.pcs, 33, 52), m0 = reg[k], nb = reg[(k + 1) % L.length], beats = Math.ceil(ch.len - EPS);
        let dir = r() < 0.5 ? 1 : -1, j = Math.max(0, lad.indexOf(m0));
        // walk the chord tones, turning at the edges, and aim the last beat at the next chord's bass
        const line = [m0];
        for (let b = 1; b < beats; b++) {
          const turn = r();
          if (b === beats - 1) { const c2 = lad.filter(m => m !== line[b - 1]); line.push(nearIn(c2.length ? c2 : lad, nb)); j = lad.indexOf(line[b]); continue; }
          if (turn < 0.25 || j + dir < 0 || j + dir >= lad.length) dir = -dir;
          j = Math.min(lad.length - 1, Math.max(0, j + dir)); line.push(lad[j]);
        }
        const slots = [];
        for (let b = 0; b < beats; b++) {
          const pr = b === 0 ? 0 : (b % 2 === 0 ? 0.12 : 0.25) + r() * 0.2;
          if (ctx.incl(pr)) slots.push({ x: ch.at + b, m: line[b], v: b === 0 ? 104 : b % 2 ? 88 : 96, c: ch.ci });
        }
        // an eighth-note pickup leads into the next chord
        const pr = 0.35 + r() * 0.3;
        if (ch.len >= 1 && ctx.incl(pr)) slots.push({ x: ch.end - 0.5, m: lead_in(nb, line[line.length - 1], chroma, KS), v: 80, c: ch.ci });
        legato(slots, ch.end, 0.92, out);
      });
      return out;
    },
    sync(ctx) {
      const { L, r, KS } = ctx, reg = bassReg(L), out = [];
      // one bar of eighths: 1 a hit, 2 a hit that comes in as density rises, 0 a rest
      const PATS = [[1, 0, 0, 1, 0, 0, 1, 0], [1, 0, 0, 1, 0, 2, 0, 2], [1, 0, 2, 1, 0, 0, 1, 0], [1, 0, 0, 2, 1, 0, 0, 1], [1, 0, 0, 1, 0, 1, 0, 2], [1, 2, 0, 1, 0, 0, 2, 1]];
      const stacc = r() < 0.45, chroma = r() < 0.5;
      L.forEach((ch, k) => {
        const m0 = reg[k], hi = popUp(m0, ch), pat = pick(r, PATS), nb = reg[(k + 1) % L.length], appRoll = r(), slots = [];
        for (let b = 0, s = 0; b < ch.len - EPS; b += 0.5, s++) {
          const p = pat[s % 8], pr = b === 0 ? 0 : p === 1 ? 0.1 + r() * 0.15 : p === 2 ? 0.4 + r() * 0.4 : 2, up = r();
          if (!ctx.incl(pr)) continue;
          const on = s % 2 === 0;
          slots.push({ x: ch.at + b, m: !on && up < 0.45 ? hi : m0, v: b === 0 ? 110 : on ? 96 : 104, c: ch.ci, on });
        }
        const tail = slots[slots.length - 1];
        if (slots.length > 1 && !tail.on && ch.end - tail.x <= 0.5 + EPS && appRoll < 0.5) tail.m = lead_in(nb, m0, chroma, KS);
        legato(slots, ch.end, 0.9, out, stacc ? 0.3 : 0);
        // a legato off-beat note that reaches past a beat must be a chord tone; the lead-in is always short
        if (!stacc) out.forEach(n => { if (n.c === ch.ci && n.m !== m0 && n.m !== hi && n.len > 0.45) n.len = 0.45; });
      });
      return out;
    },
    sub(ctx) {
      const { L, r } = ctx, out = [];
      const band = r() < 0.5 ? [33, 44] : [36, 47];
      let prev = (band[0] + band[1]) / 2;
      L.forEach(ch => {
        const m0 = (prev = nearPc(ch.bass, prev, band[0], band[1]));
        const spots = [2, 2.5, 3, 1.5, 3.5].map(p => ({ p, pr: 0.3 + r() * 0.55, oct: r() < 0.3 }));
        const cand = spots.filter(s => s.p < ch.len - 0.4).sort((a, b) => a.pr - b.pr).slice(0, 2);
        const slots = [{ x: ch.at, m: m0, v: 112, c: ch.ci }];
        cand.forEach(s => { if (ctx.incl(s.pr)) slots.push({ x: ch.at + s.p, m: s.oct && m0 + 12 <= 52 ? m0 + 12 : m0, v: 96, c: ch.ci }); });
        const pump = r() < 0.35;
        legato(slots, ch.end, pump ? 0.75 : 0.97, out);
      });
      return out;
    },
  };

  // ---------- chord patterns ----------
  function voicingsOf(ch) {
    const pcs = ch.pcs, out = [];
    for (let rot = 0; rot < pcs.length; rot++) {
      for (let base = 48; base <= 62; base++) {
        if (mod(base, 12) !== pcs[rot]) continue;
        const v = [base];
        for (let q = 1; q < pcs.length; q++) { let m = v[v.length - 1] + 1; while (mod(m, 12) !== pcs[(rot + q) % pcs.length]) m++; v.push(m); }
        if (v[v.length - 1] <= 74) out.push(v);
      }
    }
    return out;
  }
  const avg = a => a.reduce((s, x) => s + x, 0) / a.length;
  // close voicings that move little from the last chord; the seed decides between near-equal choices
  function pickVoicing(ch, center, r, slashBonus) {
    let vs = voicingsOf(ch); if (!vs.length) return ch.notes.map(m => fold(m, 48, 74));
    if (ch.inv) { const keepBass = vs.filter(v => mod(v[0], 12) === ch.bass); if (keepBass.length) vs = keepBass; }   // an inversion keeps its named bass at the bottom
    let best = vs[0], bs = Infinity;
    vs.forEach(v => { let s = Math.abs(avg(v) - center) + r() * 2.2; if (slashBonus && mod(v[0], 12) !== ch.bass) s += slashBonus; if (s < bs) { bs = s; best = v; } });
    return best;
  }
  function wideVoicing(ch, r) {
    const low = nearPc(ch.bass, 50, 48, 59), others = ch.pcs.filter(p => p !== ch.bass);
    const rot = Math.floor(r() * Math.max(1, others.length)), order = others.slice(rot).concat(others.slice(0, rot)), v = [low];
    let floor = low + 6;
    order.forEach(pc => {
      let m = floor + 1; while (mod(m, 12) !== pc) m++;
      if (m <= 74) { v.push(m); floor = m + 2; } else { const mm = ladder([pc], low + 3, 74).pop(); if (mm !== undefined && !v.includes(mm)) v.push(mm); }
    });
    return v.sort((a, b) => a - b);
  }
  const CHORDS = {
    block(ctx) {
      const { L, r } = ctx, out = []; let center = 55 + r() * 11;
      // each seed has its own comping rhythm for long chords: a restrike on 3, on the and of 3, or a push on the and of 2 and 4
      const RH = [[2], [2.5], [1.5, 3], [3], [1, 2.5]], rh = pick(r, RH), slash = r() < 0.5 ? 1.5 : 0;
      L.forEach(ch => {
        const v = pickVoicing(ch, center, r, slash); center = avg(v);
        const strikes = [0];
        rh.forEach(b => { const pr = 0.45 + r() * 0.45; if (b < ch.len - 0.5 && ctx.incl(pr)) strikes.push(b); });
        strikes.forEach((b, j) => { const end = j + 1 < strikes.length ? strikes[j + 1] : ch.len; v.forEach(m => out.push({ x: ch.at + b, len: end - b - 0.1, m, v: j ? 84 : 96, c: ch.ci })); });
      });
      return out;
    },
    stabs(ctx) {
      const { L, r } = ctx, out = []; let center = 58 + r() * 10;
      const PATS = [[0, 1, 0, 1, 0, 1, 0, 1], [0, 0, 1, 0, 0, 1, 0, 0], [1, 0, 0, 1, 0, 0, 1, 0], [0, 1, 0, 0, 1, 0, 1, 0], [0, 1, 0, 1, 0, 0, 1, 1]];
      const pat = pick(r, PATS), short = 0.18 + r() * 0.25;
      L.forEach(ch => {
        const v = pickVoicing(ch, center, r, 0); center = avg(v);
        let first = -1; for (let b = 0, s = 0; b < ch.len - EPS; b += 0.5, s++) if (pat[s % 8]) { first = s; break; }
        if (first < 0) first = 0;
        for (let b = 0, s = 0; b < ch.len - EPS; b += 0.5, s++) {
          const pr = s === first ? 0 : pat[s % 8] ? 0.1 + r() * 0.3 : 0.7 + r() * 0.3;
          if (!ctx.incl(pr)) continue;
          v.forEach(m => out.push({ x: ch.at + b, len: short, m, v: s % 2 ? 100 : 88, c: ch.ci }));
        }
      });
      return out;
    },
    pulse(ctx) {
      const { L, r } = ctx, out = []; let center = 61;
      const ACC = [[1, 0, 0, 1, 0, 0, 1, 0], [1, 0, 1, 0, 1, 0, 1, 0], [1, 0, 0, 0, 1, 0, 0, 1], [0, 1, 0, 1, 0, 1, 0, 1]], acc = pick(r, ACC);
      L.forEach(ch => {
        const v = pickVoicing(ch, center, r, 0.8); center = avg(v);
        const slots = [];
        for (let b = 0, s = 0; b < ch.len - EPS; b += 0.5, s++) {
          const pr = b === 0 ? 0 : s % 2 === 0 ? 0.1 + r() * 0.2 : 0.3 + r() * 0.4;
          if (ctx.incl(pr)) slots.push({ x: ch.at + b, s });
        }
        slots.forEach((sl, j) => { const nx = j + 1 < slots.length ? slots[j + 1].x : ch.end; v.forEach(m => out.push({ x: sl.x, len: Math.min(0.8, (nx - sl.x) * 0.8), m, v: acc[sl.s % 8] ? 104 : 76, c: ch.ci })); });
      });
      return out;
    },
    broken(ctx) {
      const { L, r } = ctx, out = []; let center = 60;
      const PATS = [[0, 1, 2, 3], [0, 1, 2, 1], [0, 2, 1, 2], [0, 1, 2, 3, 2, 1], [2, 1, 0, 1], [0, 2, 3, 1]];
      const pat = pick(r, PATS), pedal = r() < 0.5;
      L.forEach(ch => {
        const v = pickVoicing(ch, center, r, 1); center = avg(v);
        const tones = v.slice(); if (tones.length < 4) tones.push(v[0] + 12 <= 74 ? v[0] + 12 : v[v.length - 1]);
        const slots = [];
        for (let b = 0, s = 0; b < ch.len - EPS; b += 0.5, s++) {
          const pr = b === 0 ? 0 : s % 2 === 0 ? 0.1 + r() * 0.2 : 0.3 + r() * 0.4;
          if (ctx.incl(pr)) slots.push({ x: ch.at + b, m: tones[pat[s % pat.length] % tones.length], v: s % 2 ? 78 : 90, c: ch.ci });
        }
        if (pedal) slots.forEach(sl => out.push({ ...sl, len: ch.end - sl.x - 0.05 })); else legato(slots, ch.end, 0.9, out);
      });
      return out;
    },
    wide(ctx) {
      const { L, r } = ctx, out = [];
      L.forEach(ch => {
        const v = wideVoicing(ch, r), pr = 0.5 + r() * 0.4, strikes = [0];
        if (ch.len >= 4 && ctx.incl(pr)) strikes.push(2);
        strikes.forEach((b, j) => { const end = j + 1 < strikes.length ? strikes[j + 1] : ch.len; v.forEach(m => out.push({ x: ch.at + b, len: end - b - 0.1, m, v: j ? 78 : 90, c: ch.ci })); });
      });
      return out;
    },
  };

  // ---------- lead patterns ----------
  const leadLadder = ch => ladder(ch.safe, 64, 86);
  const CELLS = [
    [[0, 2, 1], [2, 1, 2], [3, 1, 1], [4, 4, 1]],
    [[0, 1, 2], [1, 1, 1], [2, 2, 1], [4, 2, 1], [6, 2, 2]],
    [[0, 3, 1], [3, 1, 2], [4, 1, 1], [5, 1, 2], [6, 2, 1]],
    [[1, 1, 1], [2, 1, 2], [3, 1, 1], [4, 3, 1], [7, 1, 2]],
    [[0, 2, 1], [2, 2, 1], [4, 2, 2], [6, 2, 1]],
    [[0, 1, 1], [1, 1, 2], [2, 1, 1], [3, 1, 2], [4, 2, 1], [6, 1, 2], [7, 1, 2]],
  ];
  const ENDS = [[[0, 2, 1], [2, 1, 2], [3, 1, 2], [4, 4, 1]], [[0, 4, 1], [4, 4, 1]], [[0, 2, 1], [2, 2, 2], [4, 4, 1]], [[0, 1, 1], [1, 1, 2], [2, 2, 1], [4, 4, 1]]];
  const HALF = [[[0, 1, 1], [1, 1, 2], [2, 2, 1]], [[0, 2, 1], [2, 1, 1], [3, 1, 2]], [[1, 1, 1], [2, 1, 2], [3, 1, 1]], [[0, 1, 1], [1, 1, 1], [2, 1, 2], [3, 1, 1]]];
  const LEAD = {
    arp(ctx) {
      const { L, r } = ctx, out = [];
      const PATS = [[0, 1, 2, 1], [0, 1, 2, 3], [3, 2, 1, 0], [0, 2, 1, 3], [1, 2, 3, 2], [0, 1, 2, 3, 4, 3, 2, 1]];
      const pat = pick(r, PATS); let ref = 70 + Math.floor(r() * 5);
      L.forEach(ch => {
        const lad = leadLadder(ch), start = Math.max(0, lad.indexOf(nearIn(lad, ref)) - 1), slots = [];
        let lastM = ref;
        for (let b = 0, s = 0; b < ch.len - EPS; b += 0.5, s++) {
          const pr = b === 0 ? 0 : s % 2 === 0 ? 0.1 + r() * 0.25 : 0.45 + r() * 0.4;
          const m = lad[Math.min(lad.length - 1, Math.max(0, start + pat[s % pat.length]))]; lastM = m;
          if (ctx.incl(pr)) slots.push({ x: ch.at + b, m, v: b === 0 ? 100 : s % 2 ? 80 : 92, c: ch.ci });
        }
        ref = lastM;
        legato(slots, ch.end, 0.9, out);
      });
      return out;
    },
    melody(ctx) {
      const { L, r, KS, K, total } = ctx;
      const a = pick(r, CELLS), b = pick(r, CELLS), e = pick(r, ENDS), base = 69 + Math.floor(r() * 5), bars = Math.ceil(total / 4 - EPS);
      const plan = i => (i % 4 === 3 || i === bars - 1 ? e : i % 2 === 0 ? a : b);
      const notes = [];
      for (let bar = 0; bar < bars; bar++) {
        plan(bar).forEach(([s, n, core]) => {
          const x = bar * 4 + s * 0.5, pr = core === 1 ? 0.05 + r() * 0.25 : 0.45 + r() * 0.45, nb = r(), tilt = r();
          if (x >= total - EPS || !ctx.incl(pr)) return;
          const ch = chordAt(L, x + 0.1), len = Math.min(n * 0.5, ch.end - x, total - x) * 0.92;
          notes.push({ x, len, c: ch.ci, ch, nb, tilt, v: core === 1 ? 96 : 82, end: (bar % 2 === 1) && s + n >= 8 });
        });
      }
      // the phrase arc: rise to the middle of every two bars, fall to its end; the second phrase answers and comes home
      const target = x => { const pos = (x % 8) / 8; return base + 6 * Math.sin(Math.PI * Math.min(1, pos * 1.15)) - (pos > 0.8 ? 3 : 0); };
      let prev = base;
      notes.forEach(n => { n.strong = isStrong(n); });
      notes.forEach((n, j) => {
        if (!n.strong) return;
        const lad = leadLadder(n.ch), want = prev + (target(n.x) - prev) * 0.55;
        let m = nearIn(lad, want);
        const lastStrong = !notes.slice(j + 1).some(q => q.strong);
        if (lastStrong && n.ch.safe.includes(K.root)) m = nearIn(ladder([K.root], 64, 86), prev);
        else if (m === prev && n.tilt < 0.5) { const k = lad.indexOf(m) + (target(n.x) >= prev ? 1 : -1); if (lad[k] !== undefined) m = lad[k]; }
        n.m = m; prev = m;
      });
      // the notes between: passing tones toward the next beat note, or a neighbour tone and back
      prev = base;
      notes.forEach((n, j) => {
        if (n.strong) { prev = n.m; return; }
        const nx = notes.slice(j + 1).find(q => q.strong), goal = nx ? nx.m : prev;
        n.m = Math.abs(goal - prev) >= 3 ? stepFrom(prev, goal > prev ? 1 : -1, KS) : stepFrom(prev, n.nb < 0.5 ? 1 : -1, KS);
        prev = n.m;
      });
      return notes.map(n => ({ x: n.x, len: n.len, m: n.m, v: n.v, c: n.c }));
    },
    hook(ctx) {
      const { L, r, KS, total } = ctx;
      const unit = r() < 0.5 ? 8 : 4, cell = unit === 8 ? pick(r, CELLS) : pick(r, HALF), center = 69 + Math.floor(r() * 6);
      // each motif note: chord-tone steps from the anchor on the beat, scale steps in between
      const motif = cell.map(([s, n, core]) => ({ s, n, core, off: pick(r, [0, 1, -1, 2, 0, 1]), dir: r() < 0.5 ? 1 : -1 }));
      const out = []; let prev = center;
      for (let u = 0; u < total - EPS; u += unit * 0.5) {
        motif.forEach((mn, j) => {
          const x = u + mn.s * 0.5, pr = j === 0 ? 0.02 : mn.core === 1 ? 0.08 + r() * 0.2 : 0.45 + r() * 0.4;
          if (x >= total - EPS || !ctx.incl(pr)) return;
          const ch = chordAt(L, x + 0.1), len = Math.min(mn.n * 0.5, ch.end - x, total - x) * 0.9, n = { x, len, c: ch.ci };
          let m;
          if (isStrong(n)) { const lad = leadLadder(ch), a = lad.indexOf(nearIn(lad, center)); m = lad[Math.min(lad.length - 1, Math.max(0, a + mn.off))]; }
          else m = stepFrom(prev, mn.dir, KS);
          prev = m; out.push({ ...n, m, v: mn.core === 1 ? 96 : 80 });
        });
      }
      return out;
    },
    sparse(ctx) {
      const { L, r } = ctx, out = []; let prev = 70 + Math.floor(r() * 6);
      L.forEach(ch => {
        const colour = ch.safe.filter(p => p !== ch.bass), lad = ladder(colour.length ? colour : ch.safe, 64, 86);
        const delay = ch.len >= 2 && r() < 0.3 ? 0.5 : 0, pr = 0.35 + r() * 0.5, mv = r();
        const split = ch.len >= 2 && ctx.incl(pr), half = Math.max(1, Math.floor(ch.len / 2));
        let m = nearIn(lad, prev);
        if (m === prev && mv < 0.5) { const k = lad.indexOf(m) + (mv < 0.25 ? 1 : -1); if (lad[k] !== undefined) m = lad[k]; }
        out.push({ x: ch.at + delay, len: (split ? half : ch.len) - delay - 0.1, m, v: 84, c: ch.ci });
        prev = m;
        if (split) { const k = lad.indexOf(m) + (mv < 0.5 ? 1 : -1), m2 = lad[k] !== undefined ? lad[k] : m; out.push({ x: ch.at + half, len: ch.len - half - 0.1, m: m2, v: 78, c: ch.ci }); prev = m2; }
      });
      return out;
    },
  };

  // ---------- finishing: feel, ranges, overlaps ----------
  function tameLeaps(raw) {
    raw.sort((a, b) => a.x - b.x || a.m - b.m);
    let prev = null;
    raw.forEach(n => {
      n.m = fold(n.m, 64, 86);
      if (prev !== null) { while (n.m - prev > 12 && n.m - 12 >= 64) n.m -= 12; while (prev - n.m > 12 && n.m + 12 <= 86) n.m += 12; }
      prev = n.m;
    });
    return raw;
  }
  // bass and lead always play at least one note in every chord, even at zero density
  function ensureEach(raw, ctx, i) {
    if (i === 1) return raw;
    ctx.L.forEach(ch => {
      if (raw.some(n => n.c === ch.ci)) return;
      const m = i === 0 ? nearPc(ch.bass, 40, 33, 47) : nearIn(leadLadder(ch), 72);
      raw.push({ x: ch.at, len: Math.min(ch.len, 1) * 0.9, m, v: 90, c: ch.ci });
    });
    return raw;
  }
  function finish(i, raw, feel, total, seed) {
    const [lo, hi] = RANGES[i], sw = feel.swing * 0.33 * 0.5, hu = feel.humanise * 0.09 * (i === 1 ? 0.5 : 1), jr = rngOf(sanitize(seed) * 7 + 13);
    raw.sort((a, b) => a.x - b.x || a.m - b.m);
    const out = raw.map(n => {
      let x = n.x;
      if (Math.abs(mod(x, 1) - 0.5) < EPS) x += sw;
      const j = jr(), jv = jr();
      x = Math.min(Math.max(0, x + (j - 0.5) * 2 * hu), total - 0.05);
      const len = Math.max(0.04, Math.min(n.len, total - x));
      return { x, len, m: fold(n.m, lo, hi), v: Math.max(1, Math.min(127, Math.round(n.v + (jv - 0.5) * 12 * feel.humanise))), c: n.c };
    });
    out.sort((a, b) => a.x - b.x || a.m - b.m);
    // the same pitch never overlaps itself: an earlier note is cut where the next one starts
    const last = new Map(), keep = [];
    for (const n of out) {
      const p = last.get(n.m);
      if (p && p.x + p.len > n.x - 0.01) {
        if (Math.abs(p.x - n.x) < 0.04) continue;
        p.len = Math.max(0.04, n.x - 0.01 - p.x);
      }
      last.set(n.m, n); keep.push(n);
    }
    return keep;
  }

  // ---------- one part ----------
  function part(i, chords, recipe, feel, key) {
    i = i === 1 || i === 2 ? i : 0;
    const L = lay(chords); if (!L.length) return [];
    const K = normKey(key), f = { swing: clamp01(feel && feel.swing), humanise: clamp01(feel && feel.humanise), density: clamp01(feel && feel.density) };
    const list = STYLES[i], style = recipe && list.some(s => s.id === recipe.style) ? recipe.style : list[0].id;
    const seed = sanitize(recipe && recipe.seed), thr = 0.1 + 0.9 * f.density;
    const ctx = { L, K, KS: keySet(K), r: rngOf(seed), total: L[L.length - 1].end, incl: pr => pr <= thr + EPS };
    let raw = (i === 0 ? BASS : i === 1 ? CHORDS : LEAD)[style](ctx).filter(n => Number.isFinite(n.m) && Number.isFinite(n.x));
    raw = ensureEach(raw, ctx, i);
    if (i === 2) raw = tameLeaps(raw);
    return finish(i, raw, f, ctx.total, seed);
  }

  // ---------- how well a part sits on chords ----------
  function fit(notes, chords) {
    const L = lay(chords); if (!L.length || !Array.isArray(notes)) return 1;
    let tot = 0, good = 0;
    notes.forEach(n => {
      if (!n || !Number.isFinite(n.x) || !Number.isFinite(n.m)) return;
      const len = Number.isFinite(n.len) ? n.len : 0.5;
      if (n.x < -0.2 || n.x >= L[L.length - 1].end || !isStrong({ x: n.x, len })) return;
      const w = Math.max(0.05, Math.min(len, 4)), ch = chordAt(L, n.x + 0.1);
      tot += w; if (ch.pcs.includes(mod(Math.round(n.m), 12))) good += w;
    });
    return tot ? good / tot : 1;
  }

  // ---------- the chord solver ----------
  // how naturally one scale degree moves to the next (rows: from, columns: to)
  const W_MAJ = [
    [0.3, 2, 1, 3, 3, 3, 0.5],
    [1, 0.3, 0.5, 1, 4, 0.8, 1.5],
    [0.8, 1, 0.3, 2.5, 0.6, 3, 0.2],
    [3, 1.5, 0.8, 0.3, 3, 1.5, 1],
    [4, 0.5, 0.8, 1.5, 0.3, 3, 0.3],
    [1.5, 2.5, 1.5, 3.5, 2, 0.3, 0.3],
    [3.5, 0.3, 1.5, 0.5, 0.5, 1, 0.2],
  ];
  const W_MIN = [
    [0.3, 0.8, 1.5, 3, 2, 3, 2.5],
    [1, 0.2, 0.5, 0.8, 3.5, 0.5, 1],
    [1.5, 0.3, 0.3, 2, 0.8, 3, 2.5],
    [3, 1, 1, 0.3, 3, 1.5, 2.5],
    [4, 0.3, 1, 1.5, 0.3, 2.5, 1],
    [2, 1, 2.5, 2.5, 2, 0.3, 3.5],
    [3.5, 0.3, 3, 1.5, 1, 2, 0.3],
  ];
  const MODES = { dorian: '0,2,3,5,7,9,10', phrygian: '0,1,3,5,7,8,10', lydian: '0,2,4,6,7,9,11', mixolydian: '0,2,4,5,7,9,10', harmonic: '0,2,3,5,7,8,11' };
  function weights(K) {
    const minorish = K.st[2] === 3, W = (minorish ? W_MIN : W_MAJ).map(row => row.slice()), id = K.st.join(',');
    if (id === MODES.dorian) { W[0][3] = 3.5; W[3][0] = 3; }                       // the major IV of Dorian
    if (id === MODES.phrygian) { W[0][1] = 3; W[1][0] = 3.5; }                     // the bII of Phrygian
    if (id === MODES.lydian) { W[0][1] = 3.5; W[1][0] = 3; }                       // the II of Lydian
    if (id === MODES.mixolydian) { W[0][6] = 3.5; W[6][0] = 3.5; W[6][3] = 2.5; } // the bVII of Mixolydian
    if (id === MODES.harmonic) for (let a = 0; a < 7; a++) W[a][2] *= 0.15;          // the augmented III
    return W;
  }
  const START_MAJ = [0.8, -0.4, -0.6, 0.2, -0.3, 0.4, -1.2], START_MIN = [0.8, -1, -0.2, 0.1, -0.4, 0.4, -0.2];
  const TEMPLATES = [
    [[4], [4], [4], [4]], [[4], [4], [4], [2], [2, 1]], [[4], [2], [2, 1], [4], [4]], [[8], [4], [4]], [[4], [4], [8]],
    [[2], [2], [4], [2], [2], [4]], [[4], [3], [1, 1], [4], [3], [1, 1]], [[4], [2], [2, 1], [4], [3], [1, 1]], [[2], [2, 1], [4], [4], [4]], [[4], [4], [4], [3], [1, 1]],
  ];
  const TEMPLATE_W = [3, 2, 2, 0.8, 0.8, 1.2, 1, 1.5, 1, 1.5];
  const sameEl = (a, b) => a.d === b.d && (a.q || '') === (b.q || '') && (a.ext || 0) === (b.ext || 0) && (a.inv || 0) === (b.inv || 0);
  // a progression is 4 or 8 bars: 16 or 32 beats in 4/4, 12 or 24 in 3/4 (up to 8 chords for 4 bars, 16 for 8)
  function validLens(spec) {
    if (!Array.isArray(spec) || spec.length < 2 || spec.length > 16) return null;
    const lens = spec.map(e => (e ? Number(e.len) : NaN)), total = lens.reduce((a, b) => a + b, 0);
    if (!lens.every(l => Number.isInteger(l) && l > 0) || total < 8 || total > 48 || total % 2 !== 0 || (total <= 16 && spec.length > 8)) return null;
    return { lens, subs: spec.map((e, s) => !!e.sub && lens[s] <= 2) };
  }
  function cleanNotes(a) {
    if (!Array.isArray(a)) return null;
    const ns = a.filter(n => n && Number.isFinite(n.x) && Number.isFinite(n.m) && n.x >= -0.12 && n.x < 32)
      .map(n => ({ x: n.x, len: Number.isFinite(n.len) && n.len > 0 ? n.len : 0.5, m: Math.round(n.m) })).sort((p, q) => p.x - q.x || p.m - q.m);
    return ns.length ? ns : null;
  }

  function progression(opts) {
    const o = opts || {}, K = normKey(o.key), r = rngOf(o.seed), minorish = K.st[2] === 3, W = weights(K);
    const keep = { bass: cleanNotes(o.keep && o.keep.bass), lead: cleanNotes(o.keep && o.keep.lead) }, kept = !!(keep.bass || keep.lead);
    const cur = validLens(o.current) ? o.current.map(e => ({ d: mod(Math.round(e.d) || 0, 7), q: e.q, ext: e.ext, inv: e.inv, len: e.len })) : null;
    // the harmonic rhythm: the one the kept parts were written over, or a fresh one when nothing is kept
    let rh = kept ? validLens(o.frame) || validLens(o.current) : null;
    if (!rh) {
      // a fresh harmonic rhythm, as long as the current progression (8 bars: two 4-bar templates)
      const want = cur ? cur.reduce((a, e) => a + e.len, 0) : 16;
      rh = { lens: [], subs: [] };
      if (want % 16 !== 0) {   // not 4/4 (a 3/4 progression): one chord a bar, or two to a bar where the bar allows
        const bars = want >= 24 ? 8 : 4, per = want / bars, half = per % 2 === 0 && r() < 0.35;
        for (let b = 0; b < bars; b++) { if (half && b % 2 === 1) { rh.lens.push(per / 2, per / 2); rh.subs.push(false, false); } else { rh.lens.push(per); rh.subs.push(false); } }
      }
      while (rh.lens.reduce((a, b) => a + b, 0) < want) {
        const tot = TEMPLATE_W.reduce((a, b) => a + b, 0); let x = r() * tot, t = 0;
        while (t < TEMPLATES.length - 1 && (x -= TEMPLATE_W[t]) > 0) t++;
        TEMPLATES[t].forEach(c => { rh.lens.push(c[0]); rh.subs.push(!!c[1]); });
      }
    } else r();
    const n = rh.lens.length, segs = []; let at = 0;
    rh.lens.forEach(len => { segs.push({ at, end: at + len, len }); at += len; });
    const sevens = r() < 0.35;

    // every chord a segment could take, with what it costs and how it fits the kept notes
    const base = [];
    for (let d = 0; d < 7; d++) {
      const tri = chordTones({ d }, K), q0 = quality(tri.tones), vars = [{}, { ext: 7 }];
      if (d === 4 && q0 === 'min') vars.push({ q: 'maj' }, { q: 'dom7' });
      if (d === 0 || d === 4) vars.push({ q: 'sus4' });
      vars.forEach(v => [0, 1, 2].forEach(inv => {
        const el = { d, ...v }; if (inv) el.inv = inv;
        const ct = chordTones(el, K), ql = quality(ct.tones);
        const prior = (v.ext ? (sevens ? 0.1 : -0.9) : 0) + (v.q === 'maj' ? 0.2 : v.q === 'dom7' ? (sevens ? 0.4 : 0.1) : v.q === 'sus4' ? -1.6 : 0) + (ql === 'dim' ? -0.6 : ql === 'aug' ? -2.5 : 0);
        base.push({ el, pcs: ct.pcs, bass: ct.pcs[0], prior, invPrior: inv === 0 ? 0 : inv === 1 ? -0.9 : -1.8, dom: d === 4 && (v.q === 'maj' || v.q === 'dom7' || q0 === 'maj') });
      }));
    }
    // borrowed major and dominant chords, used only when a kept bass note fits nothing in the key
    const extra = [];
    for (let d = 0; d < 7; d++) [{ q: 'maj' }, { q: 'dom7' }].forEach(v => [0, 1, 2].forEach(inv => {
      const el = { d, ...v }; if (inv) el.inv = inv;
      if (base.some(b => sameEl(b.el, el))) return;
      const ct = chordTones(el, K); extra.push({ el, pcs: ct.pcs, bass: ct.pcs[0], prior: -1.5, invPrior: 0, dom: false });
    }));
    function segFit(op, seg) {
      let sc = 0, ok = true;
      if (keep.bass) {
        const ns = keep.bass.filter(q => q.x >= seg.at - 0.12 && q.x < seg.end - 0.05);
        if (ns.length) {
          if (mod(ns[0].m, 12) !== op.bass) ok = false;
          ns.slice(1).forEach(q => { if (isStrong(q)) { const w = Math.min(q.len, seg.end - q.x, 4); sc += op.pcs.includes(mod(q.m, 12)) ? 0.4 * w : -3 * w; } });
        }
      }
      if (keep.lead) {
        keep.lead.forEach(q => {
          if (q.x + 0.1 < seg.at || q.x + 0.1 >= seg.end || !isStrong(q)) return;
          const w = Math.max(0.1, Math.min(q.len, seg.end - q.x, 4)), pc = mod(q.m, 12);
          sc += op.pcs.includes(pc) ? 1 * w : op.pcs.includes(mod(pc - 1, 12)) ? -4 * w : -1.5 * w;
        });
      }
      return { ok, sc };
    }
    const opts2 = segs.map(seg => {
      const build = (pool, soft) => pool.map(op => {
        const f2 = segFit(op, seg); if (!f2.ok && !soft) return null;
        return { ...op, base: op.prior + (keep.bass ? op.invPrior * 0.3 : op.invPrior) + f2.sc * 1.6 + (f2.ok ? 0 : -6) };
      }).filter(Boolean);
      let list = build(base, false);
      if (!list.length) list = build(base.concat(extra), false);
      if (!list.length) list = build(base, true);
      return list.map(op => ({ ...op, noise: r() * 1.6 }));
    });
    const bdist = (a, b) => { const x = mod(a - b, 12); return Math.min(x, 12 - x); };
    const trans = (p, q) => {
      let w = Math.log(W[p.el.d][q.el.d]);
      if (p.dom && q.el.d === 0) w += 0.7;
      if (!keep.bass) w -= 0.06 * bdist(p.bass, q.bass);
      if (sameEl(p.el, q.el)) w -= 0.8;
      return w;
    };
    const START = minorish ? START_MIN : START_MAJ;

    // a seeded beam search over the segments, then the loop back to the first chord
    let beams = [{ seq: [], sc: 0 }];
    for (let s = 0; s < n; s++) {
      const cand = [];
      beams.forEach(b => {
        const p = b.seq[b.seq.length - 1], pp = b.seq[b.seq.length - 2];
        opts2[s].forEach(op => {
          if (p && pp && sameEl(p.el, op.el) && sameEl(pp.el, op.el)) return;     // never three of the same chord
          cand.push({ seq: b.seq.concat(op), sc: b.sc + op.base + op.noise + (p ? trans(p, op) : START[op.el.d] * (kept ? 0.4 : 1)) });
        });
      });
      cand.sort((a, b) => b.sc - a.sc);
      beams = cand.slice(0, 64);
    }
    const sameLens = cur && cur.length === n && cur.every((e, s) => e.len === rh.lens[s]);
    const finals = beams.map(b => {
      let tot = b.sc + trans(b.seq[n - 1], b.seq[0]), same = false;
      if (sameLens) { const c = b.seq.filter((op, s) => sameEl(op.el, cur[s])).length; tot -= 0.55 * c; same = c === n; }
      if (n >= 3 && sameEl(b.seq[n - 1].el, b.seq[0].el) && sameEl(b.seq[n - 2].el, b.seq[0].el)) tot -= 5;
      return { seq: b.seq, tot, same };
    }).sort((a, b) => b.tot - a.tot);
    const best = finals.find(f2 => !f2.same) || finals[0];
    const spec = best.seq.map((op, s) => {
      const e = { d: op.el.d, len: rh.lens[s] };
      if (op.el.q) e.q = op.el.q; if (op.el.ext) e.ext = 7; if (op.el.inv) e.inv = op.el.inv; if (rh.subs[s] && rh.lens[s] <= 2) e.sub = true;
      return e;
    });
    const changed = !cur || cur.length !== spec.length || spec.some((e, s) => !sameEl(e, cur[s]) || e.len !== cur[s].len);
    return { spec, changed, score: Math.round(best.tot * 1000) / 1000 };
  }

  const deepFreeze = o => { Object.values(o).forEach(v => { if (v && typeof v === 'object') deepFreeze(v); }); return Object.freeze(o); };
  const M1D1Gen = deepFreeze({ RANGES, STYLES, part, progression, chordTones, fit });
  root.M1D1Gen = M1D1Gen;
  if (typeof module !== 'undefined' && module.exports) module.exports = M1D1Gen;
})(typeof globalThis !== 'undefined' ? globalThis : this);
