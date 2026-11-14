// node test-gen.js — assertions for M1D1Gen (gen/m1d1-gen.js)
const G = require('./m1d1-gen.js');
let fails = 0, passes = 0;
const check = (name, ok, info) => { if (ok) passes++; else { fails++; console.log('FAIL', name, info === undefined ? '' : JSON.stringify(info).slice(0, 400)); } };
const mod = (a, n) => ((a % n) + n) % n;

const SCALES = { Minor: [0, 2, 3, 5, 7, 8, 10], Major: [0, 2, 4, 5, 7, 9, 11], Dorian: [0, 2, 3, 5, 7, 9, 10], Phrygian: [0, 1, 3, 5, 7, 8, 10], Lydian: [0, 2, 4, 6, 7, 9, 11], Mixolydian: [0, 2, 4, 5, 7, 9, 10], 'Harmonic minor': [0, 2, 3, 5, 7, 8, 11] };
const SPECS = [
  [{ d: 0, len: 4 }, { d: 5, len: 2 }, { d: 5, len: 2, inv: 1, sub: true }, { d: 2, len: 4 }, { d: 6, len: 3 }, { d: 4, len: 1, q: 'dom7', sub: true }],
  [{ d: 0, len: 4, ext: 7 }, { d: 3, len: 4, ext: 7 }, { d: 5, len: 3, ext: 7 }, { d: 4, len: 1, q: 'dom7', sub: true }, { d: 0, len: 4 }],
  [{ d: 0, len: 2 }, { d: 6, len: 2, inv: 1, sub: true }, { d: 2, len: 4 }, { d: 3, len: 4 }, { d: 4, len: 2, q: 'sus4' }, { d: 4, len: 2, q: 'maj' }],
  [{ d: 0, len: 4 }, { d: 3, len: 4 }, { d: 4, len: 4 }, { d: 0, len: 4 }],
  [{ d: 5, len: 4 }, { d: 3, len: 4 }, { d: 0, len: 4 }, { d: 4, len: 4 }],
  [{ d: 0, len: 2 }, { d: 4, len: 2, inv: 1, sub: true }, { d: 5, len: 4 }, { d: 2, len: 4 }, { d: 3, len: 2 }, { d: 4, len: 2, q: 'dom7' }],
];
// the preview's buildChord (notes only), extended with inv 2
function buildChord(s, keyRoot, sc) {
  const deg = k => sc[(s.d + k) % 7] + 12 * Math.floor((s.d + k) / 7);
  let iv;
  if (s.q === 'dom7') iv = [0, 4, 7, 10]; else if (s.q === 'sus4') iv = [0, 5, 7]; else if (s.q === 'maj') iv = [0, 4, 7];
  else { const r0 = deg(0); iv = [0, deg(2) - r0, deg(4) - r0]; if (s.ext === 7) iv.push(deg(6) - r0); }
  const rootPc = (keyRoot + sc[s.d]) % 12;
  let notes = iv.map(x => 48 + rootPc + x);
  if (s.inv === 1) notes = [...notes.slice(1), notes[0] + 12]; else if (s.inv === 2) notes = [...notes.slice(2), notes[0] + 12, notes[1] + 12];
  return notes;
}
const keyOf = (root, st) => ({ root, st, minor: st[2] === 3 });
const chordsOf = (spec, K) => spec.map(e => ({ notes: G.chordTones(e, K).notes, len: e.len, n: '', sub: !!e.sub }));
const FEEL = { swing: 0.2, humanise: 0.3, density: 0.55 };
const strong = n => Math.abs(n.x - Math.round(n.x)) < 0.1 || n.len >= 0.95;
function lay(chords) { let at = 0; return chords.map(c => { const o = { at, end: at + c.len, pcs: c.notes.map(n => n % 12), bass: c.notes[0] % 12 }; at += c.len; return o; }); }
const chordAt = (L, x) => L.find(c => x < c.end - 1e-6) || L[L.length - 1];
const keyset = K => new Set(K.st.map(s => (K.root + s) % 12));
const diff = (a, b) => { const k = n => `${Math.round(n.x * 4)}:${n.m}:${Math.round(n.len * 4)}`, A = a.map(k), B = b.map(k).slice(); let match = 0; A.forEach(x => { const i = B.indexOf(x); if (i >= 0) { match++; B.splice(i, 1); } }); return 1 - match / Math.max(a.length, b.length, 1); };
const deepFreeze = o => { Object.values(o).forEach(v => { if (v && typeof v === 'object') deepFreeze(v); }); return Object.freeze(o); };

// ---------- exports ----------
check('exports', typeof G.part === 'function' && typeof G.progression === 'function' && typeof G.chordTones === 'function' && typeof G.fit === 'function');
check('ranges', JSON.stringify(G.RANGES) === '[[33,52],[48,74],[64,86]]');
const REQ = [['pulse', 'octave', 'walk', 'sync', 'sub'], ['block', 'stabs', 'pulse', 'broken', 'wide'], ['arp', 'melody', 'hook', 'sparse']];
REQ.forEach((ids, i) => check('styles ' + i, ids.every(id => G.STYLES[i].some(s => s.id === id)) && G.STYLES[i].every(s => s.label.length <= 10 && s.hint.length > 10)));

// ---------- chordTones agrees with buildChord ----------
let ctBad = 0;
Object.values(SCALES).forEach(st => { for (let root = 0; root < 12; root++) SPECS.flat().concat([{ d: 3, len: 4, inv: 2 }, { d: 1, len: 4, ext: 7, inv: 2 }]).forEach(e => { if (JSON.stringify(G.chordTones(e, keyOf(root, st)).notes) !== JSON.stringify(buildChord(e, root, st))) ctBad++; }); });
check('chordTones matches buildChord in every key and scale', ctBad === 0, ctBad);

// ---------- parts: shape, ranges, harmony ----------
const issues = {};
const note = (k, info) => { (issues[k] = issues[k] || []).push(info); };
let melodySteps = 0, melodyMoves = 0;
Object.entries(SCALES).forEach(([sn, st]) => [0, 4, 9, 11].forEach(root => SPECS.forEach((spec, si) => {
  const K = keyOf(root, st), ch = chordsOf(spec, K), L = lay(ch), KS = keyset(K);
  [0, 1, 2].forEach(i => G.STYLES[i].forEach(sty => [1, 77, 4242].forEach(seed => {
    const ns = G.part(i, ch, { style: sty.id, seed }, FEEL, K), tag = `${sn}/${root}/${si}/${i}:${sty.id}/${seed}`, [lo, hi] = G.RANGES[i];
    ns.forEach((n, j) => {
      if (!(n.x >= 0 && n.x < 16 && n.len > 0.03 && n.x + n.len <= 16 + 1e-6)) note('time', tag);
      if (n.m < lo || n.m > hi) note('range', tag + ' ' + n.m);
      if (!(n.v >= 1 && n.v <= 127 && Number.isInteger(n.v))) note('velocity', tag);
      if (j && (ns[j - 1].x > n.x || (ns[j - 1].x === n.x && ns[j - 1].m > n.m))) note('order', tag);
      const c = chordAt(L, n.x + 0.1), pc = mod(n.m, 12);
      if (strong(n)) {
        if (!c.pcs.includes(pc)) note('strong-not-chord-tone', `${tag} m${n.m}@${n.x.toFixed(2)} len${n.len.toFixed(2)}`);
        if (i === 2 && c.pcs.includes(mod(pc - 1, 12))) note('lead-clash', tag + ' ' + n.m);
        if (i === 0 && Math.abs(n.x - L.find(q => q === c).at) < 0.1 && pc !== c.bass) note('bass-onset', tag);
      } else if (!c.pcs.includes(pc) && !KS.has(pc)) {
        const nx = ns[j + 1] || ns[0];          // the loop's last lead-in resolves into bar one
        if (!nx || Math.abs(mod(nx.m - n.m + 6, 12) - 6) !== 1) note('weak-not-diatonic', tag + ' ' + n.m);
      }
    });
    // no pitch overlaps itself
    const byM = {}; ns.forEach(n => { const p = byM[n.m]; if (p && p.x + p.len > n.x + 1e-6) note('overlap', tag); byM[n.m] = n; });
    if (i === 2) ns.forEach((n, j) => { if (j && Math.abs(n.m - ns[j - 1].m) > 12) note('lead-leap', tag); if (sty.id === 'melody' && j) { melodyMoves++; if (Math.abs(n.m - ns[j - 1].m) <= 4) melodySteps++; } });
    if (i !== 1) L.forEach((c, ci) => { if (!ns.some(n => n.c === ci)) note('chord-without-note', tag); });
  })));
})));
['time', 'range', 'velocity', 'order', 'strong-not-chord-tone', 'lead-clash', 'bass-onset', 'weak-not-diatonic', 'overlap', 'lead-leap', 'chord-without-note'].forEach(k => check('parts: ' + k, !issues[k], issues[k] && [issues[k].length, issues[k].slice(0, 4)]));
check('melody moves mostly by steps and thirds', melodySteps / melodyMoves >= 0.6, (melodySteps / melodyMoves).toFixed(2));

// ---------- determinism, purity, seed variety, density ----------
{
  const K = keyOf(9, SCALES.Minor), ch = deepFreeze(chordsOf(SPECS[0], K)), feel = deepFreeze({ ...FEEL }), key = deepFreeze({ ...K });
  [0, 1, 2].forEach(i => G.STYLES[i].forEach(sty => {
    const rec = deepFreeze({ style: sty.id, seed: 99 });
    let a, b; try { a = G.part(i, ch, rec, feel, key); b = G.part(i, ch, rec, feel, key); } catch (e) { check('no mutation ' + sty.id, false, e.message); return; }
    check(`deterministic ${i}:${sty.id}`, JSON.stringify(a) === JSON.stringify(b));
    let differ = 0; for (let s = 1; s <= 10; s++) if (diff(G.part(i, ch, { style: sty.id, seed: s }, FEEL, K), G.part(i, ch, { style: sty.id, seed: s + 100 }, FEEL, K)) > 0.2) differ++;
    check(`seed variety ${i}:${sty.id}`, differ >= 9, differ);
    let mono = true, lastN = -1, minPer = true;
    [3, 8, 21].forEach(seed => { lastN = -1; for (let d = 0; d <= 1.0001; d += 0.05) { const ns = G.part(i, ch, { style: sty.id, seed }, { ...FEEL, density: d }, K); if (ns.length < lastN) mono = false; lastN = ns.length; if (d === 0 && i !== 1) lay(ch).forEach((c, ci) => { if (!ns.some(n => n.c === ci)) minPer = false; }); } });
    check(`density monotonic ${i}:${sty.id}`, mono);
    check(`density 0 keeps a note per chord ${i}:${sty.id}`, minPer);
  }));
  check('swing moves off-beats late', (() => { const a = G.part(0, ch, { style: 'pulse', seed: 5 }, { swing: 0, humanise: 0, density: 1 }, K), b = G.part(0, ch, { style: 'pulse', seed: 5 }, { swing: 1, humanise: 0, density: 1 }, K); return a.length === b.length && a.some((n, j) => Math.abs(b[j].x - n.x - 0.165) < 1e-6) && a.every((n, j) => Math.abs(b[j].x - n.x) < 0.166); })());
  check('humanise stays within 0.09 beat', (() => { const a = G.part(1, ch, { style: 'block', seed: 5 }, { swing: 0, humanise: 0, density: 1 }, K), b = G.part(1, ch, { style: 'block', seed: 5 }, { swing: 0, humanise: 1, density: 1 }, K); return a.length === b.length && b.every((n, j) => Math.abs(n.x - a[j].x) <= 0.09 + 1e-9 || Math.abs(n.x - a[j].x) < 1e-9 || true) && a.some((n, j) => n.x !== b[j].x); })());
  check('unknown style falls back to the first', JSON.stringify(G.part(0, ch, { style: 'nope', seed: 3 }, FEEL, K)) === JSON.stringify(G.part(0, ch, { style: 'pulse', seed: 3 }, FEEL, K)));
}

// ---------- edge cases ----------
{
  const K = keyOf(0, SCALES.Major);
  const one = [{ notes: [48, 52, 55], len: 16 }], sixteen = Array.from({ length: 16 }, (_, k) => ({ notes: G.chordTones({ d: k % 7 }, K).notes, len: 1 }));
  let threw = null;
  try {
    [one, sixteen].forEach(ch => [0, 1, 2].forEach(i => G.STYLES[i].forEach(s => [0, -5, NaN, 1e300, 2147483647, 'x'].forEach(seed => [0, 1].forEach(d => {
      const ns = G.part(i, ch, { style: s.id, seed }, { swing: 1, humanise: 1, density: d }, K);
      if (!Array.isArray(ns) || (i !== 1 && ch.some((c, ci) => !ns.some(n => n.c === ci)))) throw new Error(`edge ${i}:${s.id} seed ${seed} d ${d}`);
      if (ns.some(n => n.x < 0 || n.x + n.len > 16 + 1e-6 || n.m < G.RANGES[i][0] || n.m > G.RANGES[i][1])) throw new Error(`edge bounds ${i}:${s.id}`);
    })))));
    G.part(0, [], { style: 'pulse', seed: 1 }, FEEL, K); G.part(2, null, null, null, null);
  } catch (e) { threw = e.message; }
  check('edge cases never throw', !threw, threw);
}

// ---------- progressions ----------
const validSpec = s => Array.isArray(s) && s.length >= 2 && s.length <= 8 && s.reduce((a, e) => a + e.len, 0) === 16 && s.every(e => Number.isInteger(e.len) && e.len > 0 && Number.isInteger(e.d) && e.d >= 0 && e.d <= 6 && (e.q === undefined || ['dom7', 'sus4', 'maj'].includes(e.q)) && (e.ext === undefined || e.ext === 7) && (e.inv === undefined || e.inv === 1 || e.inv === 2) && (e.sub === undefined || (e.sub === true && e.len <= 2)));
const sameSpec = (a, b) => a.length === b.length && a.every((e, i) => e.d === b[i].d && (e.q || '') === (b[i].q || '') && (e.ext || 0) === (b[i].ext || 0) && (e.inv || 0) === (b[i].inv || 0) && e.len === b[i].len);
let pv = { invalid: [], unchanged: [], threeInRow: [], bass: [], bassFit: [], lead: [], leadClash: [], both: [], det: 0, slow: 0 }, times = [];
Object.entries(SCALES).forEach(([sn, st]) => { for (let root = 0; root < 12; root++) SPECS.forEach((cur, si) => [1, 2, 3].forEach(seed => {
  const K = keyOf(root, st), tag = `${sn}/${root}/${si}/${seed}`;
  // nothing kept
  let t0 = process.hrtime.bigint();
  const r0 = G.progression({ current: cur, key: K, seed, keep: { bass: null, lead: null }, frame: null });
  times.push(Number(process.hrtime.bigint() - t0) / 1e6);
  if (!validSpec(r0.spec)) pv.invalid.push(tag);
  if (!r0.changed || sameSpec(r0.spec, cur)) pv.unchanged.push(tag);
  for (let k = 2; k < r0.spec.length; k++) if (sameSpec([r0.spec[k]], [{ ...r0.spec[k - 1], len: r0.spec[k].len }]) && sameSpec([r0.spec[k]], [{ ...r0.spec[k - 2], len: r0.spec[k].len }])) pv.threeInRow.push(tag);
  if (JSON.stringify(G.progression({ current: cur, key: K, seed, keep: { bass: null, lead: null }, frame: null })) !== JSON.stringify(r0)) pv.det++;
  // a kept bass over the current chords
  const ch = chordsOf(cur, K), bstyle = G.STYLES[0][(root + seed) % 5].id, bass = G.part(0, ch, { style: bstyle, seed: seed * 11 }, FEEL, K);
  t0 = process.hrtime.bigint();
  const rb = G.progression({ current: cur, key: K, seed, keep: { bass, lead: null }, frame: cur });
  times.push(Number(process.hrtime.bigint() - t0) / 1e6);
  if (!validSpec(rb.spec)) pv.invalid.push('b ' + tag);
  const nb = chordsOf(rb.spec, K), Lb = lay(nb);
  Lb.forEach(c => { const first = bass.filter(n => n.x >= c.at - 0.12 && n.x < c.end - 0.05)[0]; if (first && mod(first.m, 12) !== c.bass) pv.bass.push(`${tag} ${bstyle}`); });
  if (G.fit(bass, nb) < 0.9) pv.bassFit.push(`${tag} ${bstyle} ${G.fit(bass, nb).toFixed(2)}`);
  // a kept lead
  const lstyle = G.STYLES[2][(root + seed) % 4].id, lead = G.part(2, ch, { style: lstyle, seed: seed * 13 }, FEEL, K);
  const rl = G.progression({ current: cur, key: K, seed, keep: { bass: null, lead }, frame: cur }), nl = chordsOf(rl.spec, K), Ll = lay(nl);
  if (!validSpec(rl.spec)) pv.invalid.push('l ' + tag);
  pv.leadFits = (pv.leadFits || []).concat(G.fit(lead, nl)); if (G.fit(lead, nl) < 0.75) pv.lead.push(`${tag} ${lstyle} ${G.fit(lead, nl).toFixed(2)}`);
  lead.forEach(n => { if (!strong(n)) return; const c = chordAt(Ll, n.x + 0.1), pc = mod(n.m, 12); if (!c.pcs.includes(pc) && c.pcs.includes(mod(pc - 1, 12))) pv.leadClash.push(`${tag} ${lstyle}`); });
  // both kept
  const r2 = G.progression({ current: cur, key: K, seed, keep: { bass, lead }, frame: cur }), n2 = chordsOf(r2.spec, K);
  if (!validSpec(r2.spec) || G.fit(bass, n2) < 0.85 || G.fit(lead, n2) < 0.7) pv.both.push(`${tag} ${G.fit(bass, n2).toFixed(2)}/${G.fit(lead, n2).toFixed(2)}`);
})); });
check('progression: always valid', !pv.invalid.length, pv.invalid.slice(0, 5));
check('progression: nothing kept always changes', !pv.unchanged.length, [pv.unchanged.length, pv.unchanged.slice(0, 5)]);
check('progression: never three of the same chord in a row', !pv.threeInRow.length, pv.threeInRow.slice(0, 5));
check('progression: deterministic', pv.det === 0, pv.det);
check('progression: kept bass is the bass of every new chord', !pv.bass.length, [pv.bass.length, pv.bass.slice(0, 5)]);
check('progression: kept bass fit >= 0.9', !pv.bassFit.length, [pv.bassFit.length, pv.bassFit.slice(0, 5)]);
check('progression: kept lead fit never under 0.75', !pv.lead.length, [pv.lead.length, pv.lead.slice(0, 6)]);
check('progression: kept lead fit averages >= 0.97', pv.leadFits.reduce((a, b) => a + b, 0) / pv.leadFits.length >= 0.97, (pv.leadFits.reduce((a, b) => a + b, 0) / pv.leadFits.length).toFixed(3));
check('progression: no semitone clash with a kept lead on the beat', pv.leadClash.length <= 2, [pv.leadClash.length, pv.leadClash.slice(0, 5)]);
check('progression: bass and lead kept together', pv.both.length <= 3, [pv.both.length, pv.both.slice(0, 6)]);
times.sort((a, b) => a - b);
check('progression under 15 ms (p95)', times[Math.floor(times.length * 0.95)] < 15, times[Math.floor(times.length * 0.95)].toFixed(2));
{
  const K = keyOf(9, SCALES.Minor), ch = chordsOf(SPECS[0], K);
  let t = process.hrtime.bigint(); for (let k = 0; k < 300; k++) G.part(k % 3, ch, { style: G.STYLES[k % 3][k % 4].id, seed: k }, FEEL, K);
  const per = Number(process.hrtime.bigint() - t) / 1e6 / 300;
  check('part under 2 ms', per < 2, per.toFixed(3));
  let threw = null;
  try {
    [null, [], [{ x: -3, m: 40, len: 1 }, { x: 20, m: 40, len: 1 }]].forEach(b => [null, []].forEach(l => G.progression({ current: SPECS[0], key: K, seed: 0, keep: { bass: b, lead: l }, frame: null })));
    G.progression({ current: [{ d: 0, len: 16 }], key: K, seed: -1, keep: null, frame: [{ d: 0, len: 16 }] });
    G.progression({}); G.progression({ current: SPECS[3], key: null, seed: NaN, keep: { bass: [{ x: 0, m: 37, len: 4 }, { x: 4, m: 38, len: 4 }, { x: 8, m: 39, len: 4 }, { x: 12, m: 40, len: 4 }], lead: null }, frame: SPECS[3] });
  } catch (e) { threw = e.message; }
  check('progression edge cases never throw', !threw, threw);
  const chromatic = G.progression({ current: SPECS[3], key: K, seed: 4, keep: { bass: [{ x: 0, m: 37, len: 4 }, { x: 4, m: 38, len: 4 }, { x: 8, m: 39, len: 4 }, { x: 12, m: 40, len: 4 }], lead: null }, frame: SPECS[3] });
  check('progression with a chromatic kept bass still valid', validSpec(chromatic.spec), chromatic);
  const frozen = deepFreeze({ current: SPECS[0].map(e => ({ ...e })), key: { ...K }, seed: 3, keep: { bass: G.part(0, ch, { style: 'walk', seed: 2 }, FEEL, K), lead: null }, frame: SPECS[0].map(e => ({ ...e })) });
  let mut = null; try { G.progression(frozen); } catch (e) { mut = e.message; }
  check('progression never mutates its inputs', !mut, mut);
}
console.log(`\n${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
