/*
 * OkumuLab 1 — replay of the Phase 1 data
 *
 * kind 'audio' : a Phase 1 recording plays; the note events and parameter
 *                trajectories are the ones in render_baseline.py /
 *                render_sweeps.py / render_attacks.py, so the knobs and the
 *                pipe move with the sound. The screen's physics comes from the
 *                engine running the same trajectory (muted).
 * kind 'state' : the exact 1-period snapshots of vis/*.json are drawn; the
 *                engine plays the same condition so it can be heard and compared.
 * kind 'attack': c1_attack_sequence.json (first 400 ms, every 4 ms), slowed 20x.
 */
window.OKL = window.OKL || {};
OKL.replay = (function () {
  'use strict';
  const S1 = window.OKL_STEP1;
  const MMWS = 9.80665;

  function seg(pts) {
    return (t) => {
      if (t <= pts[0][0]) return pts[0][1];
      for (let i = 1; i < pts.length; i++) if (t <= pts[i][0]) { const f = (t - pts[i - 1][0]) / (pts[i][0] - pts[i - 1][0]); return pts[i - 1][1] + f * (pts[i][1] - pts[i - 1][1]); }
      return pts[pts.length - 1][1];
    };
  }
  function segLog(pts) { const f = seg(pts.map(([t, v]) => [t, Math.log(v)])); return (t) => Math.exp(f(t)); }

  /* base_params() of labium.py: pitch lock 1, voicing follow 0, pallet 12 ms */
  const BASE = { pitchLock: 1, vFollow: 0, wind: 75, cutup: 1, y0b: 0.5, mouthFrac: 0.25, noise: 0.008, toe: 1, scaleHT: -2, morph: 0, glide: 0,
    sideHole: 0, gas: 0, tempC: 20, cMult: 1, rhoMult: 1, fmDepth: 0, fmRatio: 1, jetGain: 1, edge: 0, kick: -0.5, loss: 2.5, kappa: 1.36, trem: 0, bellows: 1 };

  function events(list) { return (t) => list.filter((e) => t >= e.t0 && t < e.t1); }
  let uid = 0;
  const ev = (midi, t0, t1) => ({ id: 'r' + (uid++), midi, t0, t1 });

  // ---- 01 range: 6 notes, 2.4 s + 0.8 s tail, 0.25 s gap
  const evRange = [36, 48, 60, 72, 84, 96].map((m, i) => ev(m, i * 3.45, i * 3.45 + 2.4));
  // ---- 02 speech: C c c1 c3, 3 short notes each
  const evSpeech = [];
  [36, 48, 60, 84].forEach((m, j) => { for (let k = 0; k < 3; k++) { const t0 = j * 2.95 + k * 0.85; evSpeech.push(ev(m, t0, t0 + 0.45)); } });
  // ---- 03/04 chorale: four independent voices, repeated pitches tied
  const chords = [[76, 67, 60, 48], [77, 69, 60, 41], [74, 67, 59, 43], [72, 67, 64, 48], [72, 69, 64, 45], [74, 69, 65, 50], [74, 71, 65, 43], [72, 67, 64, 36]];
  const durs = [1.15, 1.15, 1.15, 1.15, 1.15, 1.15, 1.15, 3.0];
  const starts = durs.reduce((a, d, i) => { a.push(i ? a[i - 1] + durs[i - 1] : 0); return a; }, []);
  const evChorale = [];
  for (let v = 0; v < 4; v++) {
    const evs = [];
    chords.forEach((ch, i) => {
      const p = ch[v];
      if (evs.length && evs[evs.length - 1][0] === p) evs[evs.length - 1][2] += durs[i];
      else evs.push([p, starts[i], durs[i]]);
    });
    evs.forEach(([p, s0, d]) => evChorale.push(ev(p, s0, s0 + d - 0.01)));
  }
  // ---- 05b wind attacks (each a fresh pipe)
  const WA = [40, 75, 150, 300, 500, 700];
  const evWA = WA.map((p, i) => Object.assign(ev(60, i * 1.85, i * 1.85 + 1.2), { wind: p }));

  // ---- sweep trajectories (render_sweeps.py)
  const fW = segLog([[0, 15], [13, 700], [15, 700], [23, 15], [24, 15]]);
  const fC = segLog([[0, 1], [1, 1], [6, 0.5], [8, 0.5], [14, 2.0], [15.5, 2.0], [18, 1.0], [19, 1.0]]);
  const fY = seg([[0, 0.5], [1, 0.5], [6, -2.0], [14, 2.0], [17, 0.5], [18, 0.5]]);
  const fSemis = seg([[0, 0], [1.2, 0], [1.8, 4], [3.0, 4], [3.6, 7], [4.8, 7], [5.4, 12], [7.0, 12], [8.5, 7], [9.0, 7], [10.5, 0], [12, 0], [14, -12], [16, -12], [17, 0]]);
  const fGlide = (t) => fSemis(t) + (t > 17.5 ? 0.35 * Math.sin(2 * Math.PI * 5.5 * (t - 17.5)) * Math.min(1, Math.max(0, (t - 17.5) / 1.0)) : 0);
  const fM = seg([[0, 0], [1.5, 0], [8, 1], [10, 1], [15, 0], [16, 0]]);
  const fS = seg([[0, -2], [1, -2], [6, -14], [8, -14], [15, 10], [17, 10], [20, -2], [21, -2]]);
  const fHe = seg([[0, 0], [1.5, 0], [8, 1], [10, 1], [15, 0], [16, 0]]);
  const fCm = seg([[0, 1], [1.5, 1], [5, 1.5], [7, 1.5], [9, 1.0], [21, 1.0]]);
  const fRm = seg([[0, 1], [10, 1], [14, 0.25], [16, 0.25], [19, 1.0], [21, 1.0]]);
  const fT = seg([[0, 20], [1, 20], [5, -20], [6, -20], [12, 60], [13, 60], [16, 20], [17, 20]]);
  const fFr = segLog([[0, 0.25], [1, 0.25], [11, 3.0], [12, 1.5], [19, 1.5]]);
  const fFd = seg([[0, 0], [0.5, 0.25], [11, 0.25], [12, 0.0], [18, 0.6], [19, 0.6]]);

  const sweep = (dur, hold, G, plot, diag, extra) => Object.assign({
    kind: 'audio', dur, notes: events([ev(60, 0, hold === undefined ? dur - 0.9 : hold)]), G, plot, diag,
  }, extra || {});

  const LIST = [
    { id: '01_range_C-c4', title: '01 Range C–c⁴', desc: 'C, c, c¹, c², c³, c⁴, 2.4 s each. Lower pipes have more harmonics and speak more slowly', kind: 'audio', dur: 6 * 3.45, notes: events(evRange), G: () => ({}), evs: evRange },
    { id: '02_speech_C-c-c1-c3', title: '02 Repeated attacks', desc: 'C, c, c¹ and c³, three short notes each: the chiff and how long the fundamental takes to grow', kind: 'audio', dur: 4 * 2.95, notes: events(evSpeech), G: () => ({}), evs: evSpeech },
    { id: '03_chorale_dry', title: '03 Chorale (dry)', desc: 'Four-part progression; every voice is an independent pipe', kind: 'audio', dur: starts[7] + 3.0 + 2.0, notes: events(evChorale), G: () => ({}), evs: evChorale },
    { id: '04_chorale_reverb', title: '04 Chorale (simple reverb)', desc: 'The same progression with a preview reverb, T60 about 3 s', kind: 'audio', dur: starts[7] + 3.0 + 2.0 + 3.8, notes: events(evChorale), G: () => ({}), evs: evChorale },
    sweep(25, 24.2, (t) => ({ wind: fW(t) }), { key: 'wind', fn: fW, label: 'chest pressure mmWS', log: true, min: 15, max: 700 }, 'wind',
      { id: '05_wind_sweep', title: '05 Wind sweep', desc: 'c¹ wind 15 → 700 → 15 mmWS. Once it speaks on the fundamental it stays there even at 700 (hysteresis)' }),
    { id: '05b_wind_attacks_c1', title: '05b Wind restarts', desc: '40 / 75 / 150 / 300 / 500 / 700 mmWS, each a fresh attack. Restarted at 700 it jumps to the octave', kind: 'audio', dur: 6 * 1.85,
      notes: events(evWA), G: (t) => { const e = evWA.find((x) => t >= x.t0 - 0.05 && t < x.t0 + 1.8); return { wind: e ? e.wind : 75 }; }, evs: evWA },
    sweep(20, undefined, (t) => ({ cutup: fC(t) }), { key: 'cutup', fn: fC, label: 'cut-up ×', log: true, min: 0.5, max: 2 }, 'cutup',
      { id: '06_cutup_sweep', title: '06 Cut-up', desc: 'Cut-up ×1 → ×0.5 → ×2 → ×1' }),
    sweep(19, undefined, (t) => ({ y0b: fY(t) }), { key: 'y0b', fn: fY, label: 'labium offset y₀/b', min: -2, max: 2 }, 'labium',
      { id: '07_labium_offset_sweep', title: '07 Labium offset', desc: '0.5b → −2b → +2b → 0.5b. The even harmonics vanish as the jet passes the centred position (0)' }),
    sweep(22, undefined, (t) => ({ glide: fGlide(t), pitchLock: 0, vFollow: 1 }), { key: 'glide', fn: fGlide, label: 'length glide (semitones)', min: -12, max: 12 }, 'glide',
      { id: '08_length_glide', title: '08 Length glide', desc: 'The pipe itself stretches and shrinks (cut-up follows). Length vibrato at the end' }),
    sweep(17, undefined, (t) => ({ morph: fM(t), pitchLock: 0, vFollow: 1 }), { key: 'morph', fn: fM, label: 'open ↔ stopped', min: 0, max: 1 }, 'morph',
      { id: '09_open_stopped_morph', title: '09 Open ↔ stopped', desc: 'The top closes like an iris and the pitch drops about an octave' }),
    sweep(22, undefined, (t) => ({ scaleHT: fS(t) }), { key: 'scaleHT', fn: fS, label: 'scale HT', min: -14, max: 10 }, 'scale',
      { id: '10_scale_morph', title: '10 Scale', desc: '−2 → −14 (string-like) → +10 (flute-like) → −2 HT. Voicing kept, pitch lock on' }),
    sweep(17, undefined, (t) => ({ gas: fHe(t), pitchLock: 0 }), { key: 'gas', fn: fHe, label: 'helium', min: 0, max: 1 }, 'helium',
      { id: '11_helium', title: '11 Helium', desc: 'Air → helium → air. The speed of sound rises about 3×, and the resonance with it' }),
    sweep(21, undefined, (t) => ({ cMult: fCm(t), rhoMult: fRm(t), pitchLock: 0 }), { key: 'cMult', fn: fCm, label: 'sound speed × / density ×', min: 0.25, max: 1.5, fn2: fRm, key2: 'rhoMult' }, 'decoupled',
      { id: '12_decoupled_c_rho', title: '12 Sound speed and density apart', desc: 'Sound speed alone ×1.5, then density alone ×0.25 (outside physics)' }),
    sweep(18, undefined, (t) => ({ tempC: fT(t), pitchLock: 0 }), { key: 'tempC', fn: fT, label: 'temperature °C', min: -20, max: 60 }, 'temperature',
      { id: '13_temperature', title: '13 Temperature', desc: '20 → −20 → 60 → 20 °C' }),
    sweep(20, undefined, (t) => ({ fmRatio: fFr(t), fmDepth: fFd(t) }), { key: 'fmDepth', fn: fFd, label: 'FM depth / ratio', min: 0, max: 0.6, fn2: (t) => fFr(t) / 5, key2: 'fmRatio' }, null,
      { id: '14_physical_fm', title: '14 Wind FM', desc: 'Wind pressure modulated at audio rate. Ratio 0.25 → 3×, then depth 0 → 0.6' }),
  ];

  // ---- 1-period snapshots (vis/*.json)
  const STATE_G = {
    C_base: [36, {}], c1_base: [60, {}], c3_base: [84, {}],
    c1_overblown: [60, { wind: 700 }],
    c1_stopped: [60, { morph: 1, vFollow: 1, pitchLock: 0 }],
    c1_wide: [60, { scaleHT: 10 }],
    c1_narrow: [60, { scaleHT: -14 }],
    c1_jet_centred: [60, { y0b: 0 }],
    c1_helium: [60, { gas: 1, pitchLock: 0 }],
  };
  const STATE_LABEL = {
    C_base: "8' C reference", c1_base: 'c¹ reference', c3_base: 'c³ reference', c1_overblown: 'c¹ 700 mmWS (overblown)', c1_stopped: 'c¹ stopped',
    c1_wide: 'c¹ wide scale +10 HT', c1_narrow: 'c¹ narrow scale −14 HT', c1_jet_centred: 'c¹ jet centred (y₀ = 0)', c1_helium: 'c¹ helium',
  };
  for (const k of Object.keys(STATE_G)) {
    const [midi, G] = STATE_G[k];
    const st = S1.vis[k];
    LIST.push({ id: 'st:' + k, title: 'Period · ' + STATE_LABEL[k], kind: 'state', state: k, midi, dur: 0, Gs: G, notes: () => [{ id: 'st', midi }], G: () => G,
      desc: 'Phase 1 one-period data (48 frames). f ' + st.f_sounding.toFixed(1) + ' Hz · regime ' + st.regime.toFixed(2) + ' · ' + st.level_db.toFixed(1) + ' dB. The sound is this engine playing the same condition' });
  }
  LIST.push({ id: 'attack', title: 'Attack c¹ 0–400 ms', kind: 'attack', midi: 60, dur: 8, notes: (t) => (t < 0.9 ? [{ id: 'att', midi: 60 }] : []), G: () => ({}),
    desc: 'Phase 1 data: the first 400 ms after the key, every 4 ms, slowed down 20×. Pressure builds in the foot, then in the bore' });

  /* ------------------------------------------------------------- controller */
  const R = { cur: null, playing: false, t: 0, startWall: 0, loading: false, buf: null, onChange: null };

  async function select(id) {
    const sc = LIST.find((x) => x.id === id);
    if (!sc) return;
    stop();
    R.cur = sc; R.t = 0; R.buf = null;
    if (sc.kind === 'audio') {
      R.loading = true;
      if (R.onChange) R.onChange();
      try { R.buf = await OKL.audio.loadRecording(sc.id); } catch (e) { R.err = e.message; }
      R.loading = false;
      if (R.buf) sc.dur = R.buf.duration;
    }
    if (R.onChange) R.onChange();
  }
  function play() {
    if (!R.cur || R.loading) return;
    if (R.cur.kind === 'audio' && R.buf) {
      if (R.t >= R.cur.dur - 0.05) R.t = 0;
      OKL.audio.playBuffer(R.buf, R.t);
    }
    R.startWall = performance.now() / 1000 - R.t;
    R.playing = true;
    if (R.onChange) R.onChange();
  }
  function stop() {
    OKL.audio.stopBuffer();
    R.playing = false;
    if (R.onChange) R.onChange();
  }
  function seek(t) {
    if (!R.cur) return;
    R.t = Math.max(0, Math.min(R.cur.dur || 0, t));
    if (R.playing) { R.reset = true; play(); }
  }
  /* advance; returns {notes, G, t} */
  function tick() {
    const sc = R.cur;
    if (!sc) return null;
    if (R.playing) {
      if (sc.kind === 'audio') R.t = OKL.audio.bufferTime();
      else R.t = performance.now() / 1000 - R.startWall;
      if (sc.kind === 'audio' && R.t >= sc.dur) { R.t = sc.dur; stop(); }
      if (sc.kind === 'attack' && R.t >= sc.dur) { R.startWall += sc.dur; R.t -= sc.dur; R.loop = (R.loop || 0) + 1; }
    }
    const t = R.t;
    let notes = sc.kind === 'state' ? sc.notes(t) : (R.playing ? sc.notes(t) : []);
    if (sc.kind === 'attack' && R.playing) notes = sc.notes(t).map((n) => ({ id: n.id + (R.loop || 0), midi: n.midi }));
    return { t, notes, G: Object.assign({}, BASE, sc.G(t)) };
  }

  /* recorded pitch / level at time t (sweeps_diag) */
  function recAt(t) {
    const sc = R.cur;
    if (!sc || !sc.diag) return null;
    const d = S1.sweeps[sc.diag];
    if (!d) return null;
    const ts = d.t;
    let i = 1; while (i < ts.length - 1 && ts[i] < t) i++;
    const f = (t - ts[i - 1]) / (ts[i] - ts[i - 1]);
    const lerp = (a) => (a[i - 1] === null || a[i] === null) ? null : a[i - 1] + (a[i] - a[i - 1]) * Math.min(1, Math.max(0, f));
    return { f: lerp(d.f), level: lerp(d.level), f0: d.f0 };
  }

  /* ------------------------------------------------------------- timeline */
  function drawTimeline(cv, pr) {
    const cx = cv.getContext('2d');
    const w = cv.clientWidth, h = cv.clientHeight;
    if (cv.width !== Math.round(w * pr)) { cv.width = Math.round(w * pr); cv.height = Math.round(h * pr); }
    cx.setTransform(pr, 0, 0, pr, 0, 0);
    cx.clearRect(0, 0, w, h);
    const sc = R.cur;
    cx.fillStyle = 'rgba(0,0,0,0.3)'; cx.fillRect(0, 0, w, h);
    if (!sc) return;
    const MONO = '"JetBrains Mono", Consolas, monospace';
    if (sc.kind === 'state') {
      cx.font = '500 11px "Chakra Petch", sans-serif'; cx.fillStyle = '#93b8c7'; cx.textAlign = 'left';
      cx.fillText('One period of the steady state, repeated (no time axis). Bore and jet are drawn from the Phase 1 values as they are.', 10, 24);
      cx.fillStyle = '#ffb547';
      cx.fillText('Header values: Phase 1   /   Gauges: this engine playing the same condition', 10, 44);
      return;
    }
    const dur = sc.dur || 1;
    const X = (t) => 8 + (w - 16) * t / dur;
    // seconds grid
    cx.strokeStyle = 'rgba(147,184,199,0.12)';
    for (let s = 0; s <= dur; s += 1) { cx.beginPath(); cx.moveTo(X(s), 0); cx.lineTo(X(s), h); cx.stroke(); }
    // note events
    if (sc.evs) {
      const ms = sc.evs.map((e) => e.midi), lo = Math.min(...ms) - 2, hi = Math.max(...ms) + 2;
      for (const e of sc.evs) {
        const y = h - 6 - (e.midi - lo) / (hi - lo) * (h - 12);
        cx.fillStyle = 'rgba(70,227,255,0.55)';
        cx.fillRect(X(e.t0), y - 2, Math.max(2, X(e.t1) - X(e.t0)), 4);
      }
    }
    // parameter trajectory
    if (sc.plot) {
      const p = sc.plot;
      const nrm = (v) => p.log ? Math.log(v / p.min) / Math.log(p.max / p.min) : (v - p.min) / (p.max - p.min);
      const curve = (fn, col) => {
        cx.beginPath();
        for (let k = 0; k <= 300; k++) { const t = dur * k / 300, y = h - 6 - Math.min(1, Math.max(0, nrm(fn(t)))) * (h - 12); if (k) cx.lineTo(X(t), y); else cx.moveTo(X(t), y); }
        cx.strokeStyle = col; cx.lineWidth = 1.5; cx.stroke(); cx.lineWidth = 1;
      };
      curve(p.fn, '#ffb547');
      if (p.fn2) curve(p.fn2, 'rgba(255,181,71,0.5)');
      cx.font = '500 10px ' + MONO; cx.fillStyle = '#ffb547'; cx.textAlign = 'left';
      cx.fillText(p.label, 12, 12);
    }
    // recorded pitch (cents re key)
    if (sc.diag && S1.sweeps[sc.diag]) {
      const d = S1.sweeps[sc.diag];
      const cents = d.f.map((f) => (f ? 1200 * Math.log2(f / d.f0) : null));
      const vals = cents.filter((c) => c !== null);
      let lo = Math.min(-50, ...vals), hi = Math.max(50, ...vals);
      cx.beginPath();
      let pen = false;
      for (let i = 0; i < d.t.length; i++) {
        const c = cents[i];
        if (c === null) { pen = false; continue; }
        const y = h - 6 - (c - lo) / (hi - lo) * (h - 12);
        if (pen) cx.lineTo(X(d.t[i]), y); else { cx.moveTo(X(d.t[i]), y); pen = true; }
      }
      cx.strokeStyle = 'rgba(70,227,255,0.85)'; cx.stroke();
      cx.font = '500 10px ' + MONO; cx.fillStyle = '#46e3ff'; cx.textAlign = 'right';
      cx.fillText('recorded pitch ' + Math.round(lo) + ' to +' + Math.round(hi) + ' ¢', w - 10, 12);
    }
    // playhead
    const px = X(R.t);
    cx.strokeStyle = '#eafaff'; cx.lineWidth = 1.5;
    cx.beginPath(); cx.moveTo(px, 0); cx.lineTo(px, h); cx.stroke(); cx.lineWidth = 1;
    if (R.loading) { cx.font = '500 11px "Chakra Petch"'; cx.fillStyle = '#ffb547'; cx.textAlign = 'center'; cx.fillText('Loading audio…', w / 2, h / 2 + 4); }
    if (R.err) { cx.font = '500 11px "Chakra Petch"'; cx.fillStyle = '#ff4d5e'; cx.textAlign = 'center'; cx.fillText(R.err, w / 2, h / 2 + 4); }
  }

  return { LIST, BASE, R, select, play, stop, seek, tick, recAt, drawTimeline, STATE_G };
})();
