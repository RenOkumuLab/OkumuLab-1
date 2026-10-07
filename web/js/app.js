/* OkumuLab 1 — application: wiring, state, analysis, frame loop */
window.OKL = window.OKL || {};
(function () {
  'use strict';
  const E = OKL.E, S1 = window.OKL_STEP1;
  const $ = (id) => document.getElementById(id);

  /* ------------------------------------------------------------------ state */
  const G = Object.assign({}, OKL.params.defaults);  // live performance parameters (plugin-only ones included)
  const padA = new Float32Array(8);
  const padB = new Float32Array(8);
  const ctx = { G, geom: null };                       // what the side panels display
  const app = {
    mode: 'live', engaged: false, focusMidi: 60, focusId: null, kbOct: 0,
    held: new Set(), activeNotes: new Set(), sustain: false, hold: false,
    gDirty: true, tel: null, cpu: 0, nActive: 0, maxVoices: 8, lastResets: 0, resetFlash: 0, played: false,
    noteOnAt: 0, replayActive: new Map(), replayG: null, midiHit: 0,
  };
  OKL.app = app;

  /* ------------------------------------------------------------------ stage scaling */
  const stage = $('stage');
  stage.style.position = 'absolute'; stage.style.left = '50%'; stage.style.top = '50%';
  let PR = 1;
  function fit() {
    const s = Math.min(window.innerWidth / 1600, window.innerHeight / 1000);
    stage.style.transform = 'translate(-50%,-50%) scale(' + s + ')';
    OKL.stageScale = s;
    PR = Math.min(3, Math.max(1, (window.devicePixelRatio || 1) * s));
    if (OKL.pipe3d && app.threeReady) OKL.pipe3d.resize(868, 594, PR);
    OKL.hud.resize(868, 594, PR);
    OKL.inst.sizeAll(PR);
  }

  /* debugging aid: show the stage region at (x, y) unscaled, e.g. OKL.peek(336, 62) */
  OKL.peek = function (x, y, s) {
    s = s || 1;
    stage.style.left = '0'; stage.style.top = '0'; stage.style.transformOrigin = '0 0';
    stage.style.transform = 'translate(' + (-x * s) + 'px,' + (-y * s) + 'px) scale(' + s + ')';
    OKL.stageScale = s;
    PR = Math.max(1, (window.devicePixelRatio || 1) * s);
    if (app.threeReady) OKL.pipe3d.resize(868, 594, PR);
    OKL.hud.resize(868, 594, PR);
    OKL.inst.sizeAll(PR);
  };
  OKL.unpeek = function () { stage.style.left = '50%'; stage.style.top = '50%'; stage.style.transformOrigin = '50% 50%'; fit(); };

  /* ------------------------------------------------------------------ effective parameters */
  function effG() {
    const g = Object.assign({}, G);
    OKL.midi.PAD_A.forEach((m, i) => { if (padA[i] > 0) g[m.key] = m.mod(g[m.key], padA[i]); });
    return g;
  }
  function activeG() { return app.mode === 'replay' && app.replayG ? app.replayG : effG(); }

  /* ------------------------------------------------------------------ notes */
  function send(m) { if (app.engaged) OKL.audio.send(m); }
  function noteOn(midi, vel, src) {
    if (app.mode === 'replay') setMode('live');
    midi = Math.max(24, Math.min(108, midi));
    const id = 'k' + midi;
    send({ t: 'on', id, midi, vel });
    app.focusId = id; app.focusMidi = midi; app.played = true;
    app.held.add(midi); app.activeNotes.add(midi);
    app.noteOnAt = performance.now();
    resetFocusAnalysis();
  }
  function noteOff(midi) {
    midi = Math.max(24, Math.min(108, midi));
    app.held.delete(midi);
    send({ t: 'off', id: 'k' + midi });
    if (!app.sustain) app.activeNotes.delete(midi);
  }
  function setSustain(on) {
    app.sustain = on;
    send({ t: 'sustain', on });
    if (!on) app.activeNotes = new Set(app.held);
  }
  function panic() {
    send({ t: 'alloff', hard: true });
    app.held.clear(); app.activeNotes.clear();
    padA.fill(0); padB.fill(0);
    app.gDirty = true;
  }

  /* RESET: every sound parameter back to its default (OUT level and view settings are not parameters) */
  function resetParams() {
    if (OKL.host === 'plugin') OKL.audio.cmd('reset');        // the plugin resets its parameters (and tells the host)
    Object.assign(G, OKL.params.defaults);
    for (const d of OKL.params.defs) OKL.params.flash(d.key);
    if (app.engaged) OKL.audio.setReverb(G.reverb);
    app.gDirty = true;
    OKL.params.refresh();
  }

  function onPad(bank, i, p) {
    if (OKL.host === 'plugin') { if (app.engaged) OKL.audio.pad(bank, i, p); return; }   // pads are MIDI to the plugin's router
    if (bank === 'A') {
      padA[i] = p;
      app.gDirty = true;
    } else {
      const was = padB[i];
      padB[i] = p;
      const midi = OKL.midi.PAD_B_NOTES[i], id = 'pB' + i;
      if (p > 0 && !(was > 0)) {
        if (app.mode === 'replay') setMode('live');
        send({ t: 'on', id, midi, vel: 100, vo: { wind: p * 300 } });
        app.focusId = id; app.focusMidi = midi; app.played = true; app.noteOnAt = performance.now();
        app.activeNotes.add(midi);
        resetFocusAnalysis();
      } else if (p > 0) send({ t: 'vo', id, vo: { wind: p * 300 } });
      else { send({ t: 'off', id }); app.activeNotes.delete(midi); }
    }
  }

  /* ------------------------------------------------------------------ parameters */
  /* which part of the pipe lights up when a parameter moves */
  const PULSE = { cutup: 'mouth', y0b: 'mouth', mouthFrac: 'mouth', morph: 'top', gas: 'atmos', tempC: 'atmos' };
  OKL.params.onChange = (key, v, source) => {
    if (PULSE[key] && app.threeReady) OKL.pipe3d.pulse(PULSE[key]);
    if (OKL.host === 'plugin' && source !== 'host' && source !== 'init' && app.engaged) OKL.audio.param(key, v);
    if (key === 'reverb' && app.engaged) OKL.audio.setReverb(v);
    app.gDirty = true;
  };

  /* ------------------------------------------------------------------ focus analysis (main thread) */
  const RING = 16384, ring = new Float32Array(RING);
  let ringW = 0, ringN = 0, sinceAn = 0;
  const an = { f: 0, mode: 1, modeKey: 1, level: -99, harm: null, fres: 0 };
  function resetFocusAnalysis() { ringN = 0; ringW = 0; }
  function pushSignal(sig) {
    for (let i = 0; i < sig.length; i++) { ring[ringW] = sig[i]; ringW = (ringW + 1) % RING; }
    ringN = Math.min(RING, ringN + sig.length);
    sinceAn += sig.length;
  }
  function lastN(n, out) { for (let i = 0; i < n; i++) out[i] = ring[(ringW - n + i + RING) % RING]; return out; }

  function fft(re, im) {
    const n = re.length;
    for (let i = 1, j = 0; i < n; i++) {
      let bit = n >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) { let t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (let len = 2; len <= n; len <<= 1) {
      const ang = -2 * Math.PI / len, wr = Math.cos(ang), wi = Math.sin(ang), hl = len >> 1;
      for (let i = 0; i < n; i += len) {
        let cr = 1, ci = 0;
        for (let j = 0; j < hl; j++) {
          const a = i + j, b = a + hl;
          const xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
          re[b] = re[a] - xr; im[b] = im[a] - xi; re[a] += xr; im[a] += xi;
          const t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
        }
      }
    }
  }
  const AN_N = 4096, AN_F = 8192;
  const seg = new Float32Array(AN_N), re = new Float64Array(AN_F), im = new Float64Array(AN_F), ac = new Float64Array(AN_N);
  function analyze(fsOut, fres) {
    if (ringN < AN_N) return;
    lastN(AN_N, seg);
    let mean = 0, ss = 0;
    for (let i = 0; i < AN_N; i++) mean += seg[i];
    mean /= AN_N;
    for (let i = 0; i < AN_N; i++) { seg[i] -= mean; ss += seg[i] * seg[i]; }
    const rms = Math.sqrt(ss / AN_N);
    an.level = 20 * Math.log10(rms / 2e-5 + 1e-12);
    an.fres = fres;
    if (an.level < 30 || !(fres > 0)) { an.f = 0; an.harm = null; return; }
    // autocorrelation pitch (same rule as labium.est_pitch: shortest lag within 10 % of the best peak)
    re.fill(0); im.fill(0);
    for (let i = 0; i < AN_N; i++) re[i] = seg[i];
    fft(re, im);
    for (let i = 0; i < AN_F; i++) { re[i] = re[i] * re[i] + im[i] * im[i]; im[i] = 0; }
    fft(re, im);
    for (let i = 0; i < AN_N; i++) ac[i] = re[i] / re[0];
    const lagMin = Math.max(2, Math.floor(fsOut / (fres * 4.5))), lagMax = Math.min(Math.floor(fsOut / (fres * 0.4)), AN_N / 2);
    let best = -1;
    for (let i = lagMin; i < lagMax; i++) if (ac[i] > best) best = ac[i];
    let L = -1;
    for (let i = lagMin + 1; i < lagMax - 1; i++) if (ac[i] >= ac[i - 1] && ac[i] >= ac[i + 1] && ac[i] > 0.9 * best) { L = i; break; }
    if (L < 0 || best < 0.3) { an.f = 0; an.harm = null; return; }
    const a = ac[L - 1], b = ac[L], c = ac[L + 1], den = a - 2 * b + c;
    const f = fsOut / (L + (den !== 0 ? 0.5 * (a - c) / den : 0));
    an.f = an.f > 0 && Math.abs(f / an.f - 1) < 0.02 ? an.f * 0.6 + f * 0.4 : f;
    an.mode = Math.max(1, Math.round(an.f / fres));
    const ft = app.geomT ? app.geomT.ftarget : fres;
    an.modeKey = Math.max(1, Math.round(an.f / ft));
    // harmonics (Blackman window)
    re.fill(0); im.fill(0);
    for (let i = 0; i < AN_N; i++) {
      const w = 0.42 - 0.5 * Math.cos(2 * Math.PI * i / (AN_N - 1)) + 0.08 * Math.cos(4 * Math.PI * i / (AN_N - 1));
      re[i] = seg[i] * w;
    }
    fft(re, im);
    const H = [];
    for (let n = 1; n <= 16; n++) {
      const fh = n * an.f;
      if (fh > fsOut * 0.45) { H.push(NaN); continue; }
      const k0 = Math.max(1, Math.ceil((fh - 0.25 * an.f) * AN_F / fsOut)), k1 = Math.floor((fh + 0.25 * an.f) * AN_F / fsOut);
      let mx = 0; for (let k = k0; k <= k1; k++) mx = Math.max(mx, Math.hypot(re[k], im[k]));
      H.push(mx);
    }
    const h1 = H[0] || 1e-12;
    const hd = H.map((v) => 20 * Math.log10(v / h1 + 1e-12));
    if (!an.harm) an.harm = hd;
    else an.harm = an.harm.map((v, i) => (isFinite(v) && isFinite(hd[i]) ? v * 0.5 + hd[i] * 0.5 : hd[i]));
    // tell the engine the period, so its snapshots cover exactly one period of the sound
    const sec = 1 / an.f;
    if (!app.sentPeriod || Math.abs(app.sentPeriod / sec - 1) > 0.003) { app.sentPeriod = sec; send({ t: 'period', sec }); }
  }

  /* ------------------------------------------------------------------ periods (bore snapshots) */
  function makePeriod(K, NX, p, u, eta, inflow, fSound, extra) {
    const env = new Float32Array(NX);
    let pmax = 1e-9;
    for (let k = 0; k < K; k++) for (let i = 0; i < NX; i++) { const a = Math.abs(p[k * NX + i]); if (a > env[i]) env[i] = a; }
    for (let i = 0; i < NX; i++) if (env[i] > pmax) pmax = env[i];
    let xi = null;
    if (u) {
      // particle displacement: integral of u over the period (mean removed), normalised
      xi = new Float32Array(K * NX);
      const dt = 1 / (Math.max(1, fSound) * K);
      let xm = 1e-12;
      for (let i = 0; i < NX; i++) {
        let s = 0, mean = 0;
        for (let k = 0; k < K; k++) { s += u[k * NX + i] * dt; xi[k * NX + i] = s; mean += s; }
        mean /= K;
        for (let k = 0; k < K; k++) { xi[k * NX + i] -= mean; xm = Math.max(xm, Math.abs(xi[k * NX + i])); }
      }
      for (let j = 0; j < xi.length; j++) xi[j] /= xm;
    }
    return Object.assign({ K, NX, p, u, eta: Float32Array.from(eta), inflow: Float32Array.from(inflow), env, pmax, xi, f: fSound }, extra || {});
  }
  const per = { cur: null, prev: null, t0: 0, src: 'live', pmaxS: 0 };
  function acceptCapture(c) {
    if (app.mode !== 'live' && !(app.replay && app.replay.kind === 'audio')) return;
    const f = an.f > 0 ? an.f : c.fs / c.T;
    const pd = makePeriod(c.K, c.NX, c.p, c.u, c.eta, c.inflow, f, { midi: c.midi, id: c.id });
    if (per.cur && per.cur.id !== c.id) per.prev = null; else per.prev = per.cur;
    per.cur = pd; per.t0 = performance.now(); per.src = 'live';
  }
  function statePeriod(name) {
    const st = S1.vis[name];
    const K = st.period.p.length, NX = st.period.p[0].length;
    const p = new Float32Array(K * NX), u = new Float32Array(K * NX);
    for (let k = 0; k < K; k++) for (let i = 0; i < NX; i++) { p[k * NX + i] = st.period.p[k][i]; u[k * NX + i] = st.period.u[k][i]; }
    return makePeriod(K, NX, p, u, st.period.eta_over_b, st.period.inflow, st.f_sounding, { state: name });
  }

  /* ------------------------------------------------------------------ view model */
  const NXV = 48;
  const vm = {
    on: 0, jetOn: 0, f: 0, strobe: 2, phase: 0, pInst: new Float32Array(NXV), env: new Float32Array(NXV), xi: new Float32Array(NXV),
    pmax: 1, etaRel: 0, inflow: 0.5, b: 2.6e-4, Uj: 0, pf: 0, tauP: 0, uc: 0, A: 4, source: 'live', etaK: null, inK: null, attackT: 0,
    etaAt: () => 0,
  };
  let vmHasP = false;
  function lerpK(arr, K, NX, phase, i) {
    const fk = ((phase % 1) + 1) % 1 * K, k0 = Math.floor(fk) % K, k1 = (k0 + 1) % K, f = fk - Math.floor(fk);
    return arr[k0 * NX + i] * (1 - f) + arr[k1 * NX + i] * f;
  }
  function lerp1(arr, phase) {
    const K = arr.length, fk = ((phase % 1) + 1) % 1 * K, k0 = Math.floor(fk) % K, k1 = (k0 + 1) % K, f = fk - Math.floor(fk);
    return arr[k0] * (1 - f) + arr[k1] * f;
  }
  function resample(src, NX, i) {          // src period NX -> view NXV
    return (i / (NXV - 1)) * (NX - 1);
  }

  function buildVM(dt, g, tf) {
    const strobeN = +$('strobe').value;
    vm.strobe = 0.5 * Math.pow(12, strobeN);           // 0.5 .. 6 s per period
    $('strobeVal').textContent = vm.strobe.toFixed(1) + 's';
    const sc = app.replay;
    const W = g.W, h = g.h;
    vm.b = 0.4 * h;
    vm.A = Math.min(0.4 * W / h, g.ampcap || 4);
    if (sc && sc.kind === 'attack') return buildAttackVM(dt, g);
    vm.source = sc && sc.kind === 'state' ? 'snapshot' : 'live';
    const P = per.cur;
    // sounding frequency for the slow motion
    vm.f = (P && P.f) || an.f || g.fres;
    vm.phase = (vm.phase + dt / vm.strobe) % 1;
    if (tf) { vm.Uj = tf.Uj; vm.pf = tf.pf; } else { vm.Uj *= Math.exp(-dt * 3); vm.pf *= Math.exp(-dt * 3); }
    if (sc && sc.kind === 'state') {
      const st = S1.vis[sc.state];
      vm.Uj = st.wind.U_j; vm.pf = st.wind.p_foot;
      vm.on = 1;
    } else {
      const target = Math.min(1, Math.max(0, (an.level - 38) / 22));
      vm.on += (target - vm.on) * (1 - Math.exp(-dt * (target > vm.on ? 12 : 3)));
    }
    vm.jetOn = Math.min(1, vm.pf / (0.35 * (g.pfDesign || 500)));
    // jet transit delay
    vm.uc = vm.Uj > 0.05 ? g.kappa * Math.pow(vm.Uj, 2 / 3) * Math.cbrt(2 * Math.PI * Math.max(1, g.fres) * h) : 0;
    vm.tauP = vm.uc > 0 ? (W / vm.uc) * vm.f : 0;
    if (P) {
      const w = per.prev ? Math.min(1, (performance.now() - per.t0) / 160) : 1;
      const Q = per.prev;
      for (let i = 0; i < NXV; i++) {
        const si = resample(P, P.NX, i), i0 = Math.floor(si), i1 = Math.min(P.NX - 1, i0 + 1), fi = si - i0;
        const pa = lerpK(P.p, P.K, P.NX, vm.phase, i0) * (1 - fi) + lerpK(P.p, P.K, P.NX, vm.phase, i1) * fi;
        const ea = P.env[i0] * (1 - fi) + P.env[i1] * fi;
        let xa = P.xi ? lerpK(P.xi, P.K, P.NX, vm.phase, i0) * (1 - fi) + lerpK(P.xi, P.K, P.NX, vm.phase, i1) * fi : 0;
        if (Q && w < 1 && Q.NX === P.NX) {
          const pb = lerpK(Q.p, Q.K, Q.NX, vm.phase, i0) * (1 - fi) + lerpK(Q.p, Q.K, Q.NX, vm.phase, i1) * fi;
          const eb = Q.env[i0] * (1 - fi) + Q.env[i1] * fi;
          vm.pInst[i] = pb + (pa - pb) * w; vm.env[i] = eb + (ea - eb) * w;
        } else { vm.pInst[i] = pa; vm.env[i] = ea; }
        vm.xi[i] = xa;
      }
      const pm = Q && w < 1 ? Q.pmax + (P.pmax - Q.pmax) * w : P.pmax;
      per.pmaxS = per.pmaxS ? per.pmaxS + (pm - per.pmaxS) * (1 - Math.exp(-dt * (pm > per.pmaxS ? 10 : 2))) : pm;
      vm.pmax = Math.max(1e-6, per.pmaxS);
      vm.etaK = P.eta; vm.inK = P.inflow;
      vm.etaRel = lerp1(P.eta, vm.phase);
      vm.inflow = lerp1(P.inflow, vm.phase);
      vmHasP = true;
    } else {
      vm.pInst.fill(0); vm.env.fill(0); vm.xi.fill(0); vm.pmax = 1; vm.etaK = null; vm.inK = null; vm.etaRel = 0; vm.inflow = 0.5; vmHasP = false;
    }
    const eA = Math.exp(vm.A) - 1, b = vm.b, y0 = g.y0, on = vm.on;
    const etaArr = P ? P.eta : null;
    vm.etaAt = (yn) => {
      if (!etaArr) return 0;
      const gg = (Math.exp(vm.A * yn) - 1) / eA;
      const er = lerp1(etaArr, vm.phase + vm.tauP * (1 - yn));
      return gg * (er * b + y0) * on;
    };
  }

  /* attack sequence: frames every 4 ms, slowed down 20x */
  let ATT = null;
  function buildAttackVM(dt, g) {
    const A = S1.attack;
    if (!ATT) {
      let pm = 1e-9;
      for (const fr of A.p) for (const v of fr) pm = Math.max(pm, Math.abs(v));
      ATT = { pm, pfEnd: A.p_foot[A.p_foot.length - 1] };
    }
    vm.source = 'attack';
    const tms = Math.min(399, (OKL.replay.R.t / 8) * 400);
    vm.attackT = tms;
    const n = A.t_ms.length, fk = Math.min(n - 1.001, tms / 4), k0 = Math.floor(fk), k1 = Math.min(n - 1, k0 + 1), f = fk - k0;
    const NX = A.p[0].length;
    for (let i = 0; i < NXV; i++) {
      const si = i / (NXV - 1) * (NX - 1), i0 = Math.floor(si), i1 = Math.min(NX - 1, i0 + 1), fi = si - i0;
      const at = (k) => A.p[k][i0] * (1 - fi) + A.p[k][i1] * fi;
      vm.pInst[i] = at(k0) * (1 - f) + at(k1) * f;
      let e = 0;
      for (let k = Math.max(0, k0 - 2); k <= k0; k++) e = Math.max(e, Math.abs(at(k)));
      vm.env[i] = e;
      vm.xi[i] = 0;
    }
    vm.pmax = ATT.pm;
    vm.Uj = A.U_j[k0] * (1 - f) + A.U_j[k1] * f;
    vm.pf = A.p_foot[k0] * (1 - f) + A.p_foot[k1] * f;
    vm.jetOn = Math.min(1, vm.pf / (0.35 * ATT.pfEnd));
    let mx = 0; for (let i = 0; i < NXV; i++) mx = Math.max(mx, vm.env[i]);
    vm.on = Math.min(1, mx / ATT.pm * 1.3);
    vm.etaRel = A.eta_over_b[k0] * (1 - f) + A.eta_over_b[k1] * f;
    vm.inflow = A.inflow[k0] * (1 - f) + A.inflow[k1] * f;
    vm.f = 261.6; vm.phase = 0; vm.tauP = 0; vm.uc = 0;
    vm.etaK = null;
    const eA = Math.exp(vm.A) - 1, b = vm.b, y0 = g.y0, er = vm.etaRel;
    vm.etaAt = (yn) => ((Math.exp(vm.A * yn) - 1) / eA) * (er * b + y0);
    vmHasP = true;
  }

  /* ------------------------------------------------------------------ display geometry (smoothed) */
  let geomD = null;
  const GEO_KEYS = ['d', 'H', 'W', 'h', 'toe', 'footLen', 'L', 'Lphys', 'Leff', 'M', 'fres', 'morph', 'y0', 'c', 'rho', 'pchest', 'kappa', 'ampcap', 'ftarget', 'f0', 'Wdesign', 'pfDesign', 'ising'];
  function smoothGeom(target, dt) {
    if (!geomD || geomD.midi !== target.midi && !geomD.morphing) geomD = geomD || Object.assign({}, target);
    const k = 1 - Math.exp(-dt * 7);
    for (const key of GEO_KEYS) {
      const t = target[key], c = geomD[key];
      if (typeof t !== 'number') continue;
      geomD[key] = (typeof c === 'number' && isFinite(c)) ? c + (t - c) * k : t;
    }
    geomD.midi = target.midi; geomD.name = target.name;
    return geomD;
  }

  /* ------------------------------------------------------------------ ghosts (other sounding pipes) */
  const ghostCache = new Map();
  function ghostList(G2) {
    const out = [];
    if (!app.tel) return out;
    const key = JSON.stringify([G2.scaleHT, G2.glide, G2.pitchLock, G2.gas, G2.tempC, G2.cMult, G2.morph, G2.cutup, G2.mouthFrac]);
    for (const v of app.tel.vox) {
      if (app.tel.focus && v.id === app.tel.focus.id) continue;
      const ck = v.midi + '|' + key;
      let gm = ghostCache.get(ck);
      if (!gm) { gm = E.displayGeometry(v.midi, G2, 96000); ghostCache.set(ck, gm); if (ghostCache.size > 200) ghostCache.clear(); }
      out.push({ id: v.id, midi: v.midi, geom: gm, level: Math.min(1, v.peak / 0.4), label: OKL.noteLabel(v.midi) });
      if (out.length >= 7) break;
    }
    return out;
  }

  /* ------------------------------------------------------------------ mode */
  function setMode(m) {
    if (app.mode === m) return;
    if (OKL.host === 'plugin' && m === 'replay') return;      // Phase 1 recordings: browser prototype only
    app.mode = m;
    document.querySelectorAll('#modeSeg button').forEach((b) => b.classList.toggle('on', b.dataset.mode === m));
    $('replayStrip').hidden = m !== 'replay';
    if (app.threeReady) OKL.pipe3d.setPipBottom(m === 'replay' ? 446 : null);   // keep the mouth inset above the replay strip
    OKL.params.setReadOnly(m === 'replay');
    panic();
    OKL.replay.stop();
    for (const id of app.replayActive.keys()) send({ t: 'off', id });
    app.replayActive.clear();
    per.cur = per.prev = null;
    if (m === 'live') {
      ctx.G = G; OKL.params.ctx.G = G;
      app.replay = null; app.replayG = null;
      send({ t: 'mute', on: false });
      setMsg('Play with the computer keys (A–K) or a MIDI keyboard');
    } else {
      selectReplay($('rpSelect').value || OKL.replay.LIST[4].id);
    }
    app.gDirty = true;
  }
  async function selectReplay(id) {
    for (const rid of app.replayActive.keys()) send({ t: 'off', id: rid });
    app.replayActive.clear();
    send({ t: 'alloff', hard: true });
    per.cur = per.prev = null;
    await OKL.replay.select(id);
    const sc = OKL.replay.R.cur;
    app.replay = sc;
    send({ t: 'mute', on: sc.kind === 'audio' });
    if (sc.kind === 'state') { per.cur = statePeriod(sc.state); per.src = 'state'; }
    if (sc.kind === 'audio') send({ t: 'cap', sec: 0.06 });
    setMsg('<b>Phase 1</b> ' + sc.title + ' — ' + sc.desc);
    $('rpDesc').textContent = sc.desc;
    if (sc.kind !== 'audio') OKL.replay.play();
    updateRpButton();
  }
  function updateRpButton() {
    const b = $('rpPlay'), R = OKL.replay.R;
    b.textContent = R.playing ? '❚❚' : '▶';
    b.classList.toggle('on', R.playing);
  }
  function setMsg(html) { $('tbMsg').innerHTML = html; }

  function replayFrame() {
    const r = OKL.replay.tick();
    if (!r) return;
    app.replayG = Object.assign({}, E.DEFAULT_G, { reverb: G.reverb, master: 1, velSens: G.velSens }, r.G);
    ctx.G = app.replayG; OKL.params.ctx.G = app.replayG;
    const want = new Map(r.notes.map((n) => [n.id, n.midi]));
    for (const [id] of app.replayActive) if (!want.has(id)) { send({ t: 'off', id }); app.replayActive.delete(id); }
    for (const [id, midi] of want) {
      if (!app.replayActive.has(id)) {
        send({ t: 'G', G: app.replayG });
        send({ t: 'on', id, midi, vel: 100, vo: { tau: 0.012 } });
        app.replayActive.set(id, midi);
        app.focusId = id; app.focusMidi = midi;
        resetFocusAnalysis();
        if (per.src !== 'state') { per.cur = null; per.prev = null; }
      }
    }
    app.activeNotes = new Set([...app.replayActive.values()]);
    app.gDirty = true;
    const sc = app.replay;
    if (sc) {
      const R = OKL.replay.R;
      $('rpTime').textContent = sc.kind === 'state' ? 'steady' : R.t.toFixed(1) + ' / ' + (sc.dur || 0).toFixed(1) + ' s';
    }
  }

  /* ------------------------------------------------------------------ annunciators */
  function annunciate(g, Gx) {
    const A = OKL.inst.ann, now = performance.now();
    const sounding = vm.on > 0.5;
    A('sound', sounding ? 'g' : '');
    A('over', an.f > 0 && an.mode >= 2 && vm.on > 0.3 ? 'a' : '');
    const gate = app.tel && app.tel.focus && app.tel.focus.gate > 0.5;
    A('under', gate && vm.jetOn > 0.3 && vm.on < 0.35 && now - app.noteOnAt > 250 ? 'a' : '');
    A('stop', Gx.morph > 0.5 ? 'c' : '');
    A('gas', Math.abs(Gx.gas) > 0.005 ? 'c' : '');          // the Gas knob only, whenever it does not read AIR
    A('lock', Gx.pitchLock > 0.5 ? 'g' : '');
    A('reset', now < app.resetFlash ? 'r' : '');
    A('lim', app.gr < 0.89 ? 'a' : '');
  }

  /* ------------------------------------------------------------------ engine messages */
  function onEngine(m) {
    if (m.t !== 'tel') return;
    app.tel = m.tel; app.cpu = m.cpu; app.nActive = m.nActive; app.maxVoices = m.maxVoices;
    app.gr = Math.min(app.gr === undefined ? 1 : app.gr * 0.9 + 0.1, m.tel.gr);
    const f = m.tel.focus;
    if (OKL.host === 'plugin') {
      // the plugin's MIDI decides which pipe is in focus; pads come back as pressures
      if (f && f.id !== app.focusId) {
        app.focusId = f.id; app.focusMidi = f.midi; app.played = true; app.noteOnAt = performance.now();
        resetFocusAnalysis();
      }
      if (m.pads) {
        let moved = false;
        for (let i = 0; i < 8; i++) { if (padA[i] !== m.pads.a[i]) { padA[i] = m.pads.a[i]; moved = true; } padB[i] = m.pads.b[i]; }
        if (moved) app.gDirty = true;
      }
      if (m.lock) app.lock = m.lock;
    }
    if (f) {
      if (f.resets > app.lastResets && f.id === app.lastResetId) app.resetFlash = performance.now() + 1200;
      app.lastResets = f.resets; app.lastResetId = f.id;
      if (f.id !== app.sigId) { app.sigId = f.id; resetFocusAnalysis(); }
    }
    if (m.sig && m.sig.length) pushSignal(m.sig);
    if (m.caps) for (const c of m.caps) acceptCapture(c);
  }

  /* ------------------------------------------------------------------ frame loop */
  let tPrev = performance.now(), slowT = 0, harmT = 0;
  function frame(now) {
    const dt = Math.min(0.05, Math.max(0.001, (now - tPrev) / 1000));
    tPrev = now;
    if (app.mode === 'replay') replayFrame();
    const Gp = activeG();
    // P5 (plugin): the knobs as the matrix holds them in the focus pipe (drawing only: the plugin plays)
    const Gx = OKL.host === 'plugin' ? OKL.lab.modulate(Gp, app.tel) : Gp;
    OKL.lab.frame(G, Gx, app.tel);
    if (app.gDirty && app.engaged) {
      send({ t: 'G', G: Gp });
      app.gDirty = false;
      OKL.midi.PAD_A.forEach((pm, i) => OKL.params.setMod(pm.key, padA[i] > 0 && app.mode === 'live' ? pm.mod(G[pm.key], padA[i]) : null));
    }
    // MALLET (plugin): struck instead of blown; the struck pipe keeps its physical length (no pitch lock)
    const mal = OKL.host === 'plugin' && G.excite >= 0.5;
    if (mal !== !!app.mallet) { app.mallet = mal; OKL.mallet.setMode(mal); }
    const Gg = mal ? Object.assign({}, Gx, { pitchLock: 0 }) : Gx;
    const ml = mal && app.tel ? app.tel.mallet : null;
    // focus pipe geometry
    const tf = app.tel && app.tel.focus;
    const midi = app.focusMidi;
    const gT = E.displayGeometry(midi, Gg, 96000);
    app.geomT = gT;
    ctx.geom = gT; OKL.params.ctx.geom = gT;
    const g = smoothGeom(gT, dt);
    // analysis (~12 Hz)
    if (sinceAn >= 3200 && app.engaged) { sinceAn = 0; analyze(OKL.audio.ctx.sampleRate, tf ? tf.fres : gT.fres); }
    if (!tf && ringN === 0) { an.f = 0; an.level = -99; }
    buildVM(dt, g, tf);

    if (app.threeReady) {
      OKL.pipe3d.setGeometry(g);
      OKL.pipe3d.setGhosts(ghostList(Gg));
      OKL.pipe3d.setMallet(mal ? OKL.mallet.view3d(G, g, ml) : { on: false });
      OKL.pipe3d.update(dt, vm, Gx);
      OKL.pipe3d.render();
    }
    // ising number of the focus pipe (live foot pressure)
    const pfI = app.replay && app.replay.kind === 'state' ? S1.vis[app.replay.state].wind.p_foot : vm.pf;
    const ising = pfI > 1 ? Math.sqrt(2 * pfI * g.h / (g.rho * g.W * g.W * g.W)) / g.ftarget : 0;
    // HUD
    const sc = app.replay;
    let src = null, rec = null, srcColor = null;
    if (sc && sc.kind === 'state') {
      const st = S1.vis[sc.state];
      src = 'Phase 1 period data · ' + sc.state; rec = 'Phase 1: f ' + st.f_sounding.toFixed(1) + ' Hz · regime ' + st.regime.toFixed(2) + ' · ' + st.level_db.toFixed(1) + ' dB';
    } else if (sc && sc.kind === 'attack') { src = 'Phase 1 attack data · c1_attack_sequence'; }
    else if (sc) {
      src = 'Phase 1 recording · ' + sc.title;
      const r = OKL.replay.recAt(OKL.replay.R.t);
      if (r && r.f) rec = 'recorded f ' + r.f.toFixed(1) + ' Hz · ' + r.level.toFixed(1) + ' dB';
      srcColor = '#ffb547';
    }
    const anShow = sc && sc.kind === 'attack' ? null : an;
    const fres = tf ? tf.fres : g.fres;
    OKL.hud.draw({ vm, g, an: anShow, src, rec, srcColor, G: Gx, tel: tf, ising,
      mallet: mal ? { ml, fres, metal: OKL.mallet.metalName(G), head: OKL.mallet.headName(G) } : null,
      hint: !app.played && app.mode === 'live' ? (mal ? 'Strike with the computer keys (A–K) or a MIDI keyboard' : 'Play with the computer keys (A–K) or a MIDI keyboard') : null });
    if (mal) OKL.inst.drawWall({ ml, g, dt, headD: G.headD / 1000, headColor: OKL.mallet.headCss(G) });
    else OKL.inst.drawJet({ g, vm, G: Gx });
    // spectrogram (every frame) and slower instruments
    if (app.engaged) OKL.inst.drawSpec(dt, OKL.audio.analyser, OKL.audio.ctx.sampleRate, an.f > 0 && !mal ? an.f : 0, OKL.noteLabel(g.midi),
      mal && ml && ml.modes ? ml.modes.map((md) => ({ f: md[0], n: md[3], m: md[4] })) : null);
    slowT += dt; harmT += dt;
    if (slowT > 1 / 30) {
      slowT = 0;
      const vox = app.tel ? app.tel.vox.map((v) => ({ midi: v.midi, gate: v.gate, focus: tf && v.id === tf.id })) : [];
      if (mal) {
        OKL.inst.drawMalletGauges({ ml, g, an: anShow, vox, fres });
        OKL.mallet.annunciate(OKL.inst.ann, ml, tf, an, Gx, app.gr === undefined ? 1 : app.gr);
        OKL.inst.ann('reset', performance.now() < app.resetFlash ? 'r' : '');
      } else {
        OKL.inst.drawGauges({ vm, g, an: anShow, ising, vox });
        annunciate(g, Gx);
      }
    }
    if (harmT > 1 / 12) {
      harmT = 0;
      let ref = null;
      if (sc && sc.kind === 'state') ref = S1.vis[sc.state].harmonics_db;
      else ref = refHarm(g.midi);
      if (mal) OKL.inst.drawModes({ ml });
      else OKL.inst.drawHarm({ an: anShow, harmRef: ref });
      OKL.params.refresh();
      topbar();
      if (app.mode === 'replay') OKL.replay.drawTimeline($('rpTimeline'), PR);
    }
    requestAnimationFrame(frame);
  }

  function refHarm(midi) {
    const rows = S1.range;
    let best = rows[0];
    for (const r of rows) if (Math.abs(r.midi - midi) < Math.abs(best.midi - midi)) best = r;
    return Math.abs(best.midi - midi) <= 1 ? best.hm : null;
  }

  function topbar() {
    $('voxCount').textContent = (app.tel ? app.tel.vox.length : 0) + '/' + app.maxVoices;
    $('cpuVal').textContent = app.engaged ? (app.cpu * 100).toFixed(0) + '%' : '—';
    const led = $('midiLed');
    led.classList.toggle('hit', performance.now() - app.midiHit < 120);
  }

  /* ------------------------------------------------------------------ input wiring */
  const KEYMAP = { a: 0, w: 1, s: 2, e: 3, d: 4, f: 5, t: 6, g: 7, y: 8, h: 9, u: 10, j: 11, k: 12, o: 13, l: 14, p: 15, ';': 16 };
  const keyDown = new Set();
  const padKeys = new Map();
  function bindKeys() {
    window.addEventListener('keydown', (e) => {
      if (e.target && (e.target.tagName === 'SELECT' || e.target.tagName === 'INPUT')) { if (e.key !== 'Escape') return; }
      if (e.ctrlKey || e.metaKey || e.altKey) return;
      const k = e.key.toLowerCase();
      if (e.key === 'Escape') { panic(); e.preventDefault(); return; }
      if (e.repeat) { if (k in KEYMAP || e.key === ' ') e.preventDefault(); return; }
      if (k in KEYMAP) {
        const midi = 60 + 12 * app.kbOct + KEYMAP[k];
        keyDown.add(k);
        noteOn(midi, 100, 'kb');
        e.preventDefault();
      } else if (k === 'z' || k === 'x') {
        app.kbOct = Math.max(-3, Math.min(3, app.kbOct + (k === 'z' ? -1 : 1)));
        if (app.mode === 'live') setMsg('Computer keys: A = ' + OKL.noteLabel(60 + 12 * app.kbOct) + '  (Z / X: octave)');
      } else if (e.key === ' ') { setSustain(true); e.preventDefault(); }
      else if (/^Digit[1-8]$/.test(e.code)) {
        const i = +e.code.slice(5) - 1, bank = e.shiftKey ? 'B' : 'A';
        if (!padKeys.has(e.code)) {
          padKeys.set(e.code, { bank, i, t0: performance.now() });
          onPad(bank, i, 0.3);
        }
        e.preventDefault();
      }
    });
    window.addEventListener('keyup', (e) => {
      const k = e.key.toLowerCase();
      if (k in KEYMAP && keyDown.has(k)) { keyDown.delete(k); noteOff(60 + 12 * app.kbOct + KEYMAP[k]); }
      else if (e.key === ' ') setSustain(false);
      else if (padKeys.has(e.code)) { const pk = padKeys.get(e.code); padKeys.delete(e.code); onPad(pk.bank, pk.i, 0); }
    });
    // pad keys: pressure rises while held
    setInterval(() => {
      for (const pk of padKeys.values()) {
        const p = Math.min(1, 0.3 + (performance.now() - pk.t0) / 1500 * 0.7);
        onPad(pk.bank, pk.i, p);
      }
    }, 40);
    window.addEventListener('blur', () => { for (const k of keyDown) noteOff(60 + 12 * app.kbOct + KEYMAP[k]); keyDown.clear(); });
  }

  function bindUi() {
    document.querySelectorAll('#modeSeg button').forEach((b) => b.addEventListener('click', () => setMode(b.dataset.mode)));
    OKL.mallet.bind();
    OKL.lab.build();
    $('btnPanic').addEventListener('click', panic);
    $('btnReset').addEventListener('click', resetParams);
    $('btnHelp').addEventListener('click', () => { $('help').hidden = false; });
    $('helpClose').addEventListener('click', () => { $('help').hidden = true; });
    $('help').addEventListener('click', (e) => { if (e.target.id === 'help') $('help').hidden = true; });
    $('btnMon').addEventListener('click', () => {
      const m = $('midimon'); m.hidden = !m.hidden; $('btnMon').classList.toggle('on', !m.hidden);
      if (!m.hidden) $('monLog').textContent = OKL.midi.log.join('\n');
    });
    $('monClose').addEventListener('click', () => { $('midimon').hidden = true; $('btnMon').classList.remove('on'); });
    $('masterVol').addEventListener('input', (e) => { if (app.engaged) OKL.audio.setMaster(+e.target.value); });
    $('btnRotate').addEventListener('click', (e) => { const on = !e.currentTarget.classList.contains('on'); e.currentTarget.classList.toggle('on', on); OKL.pipe3d.setAutoRotate(on); });
    $('btnView').addEventListener('click', (e) => {
      const v = OKL.pipe3d.state.view === 'full' ? 'mouth' : 'full';
      OKL.pipe3d.setView(v); e.currentTarget.textContent = v === 'full' ? 'FULL' : 'MOUTH';
      e.currentTarget.classList.toggle('on', v === 'mouth');
    });
    $('xray').addEventListener('input', (e) => OKL.pipe3d.setXray(+e.target.value));
    // replay
    const sel = $('rpSelect');
    const groups = { audio: 'Phase 1 recordings (synced to sound)', state: 'One-period data (vis/*.json)', attack: 'Attack' };
    for (const kind of ['audio', 'state', 'attack']) {
      const og = document.createElement('optgroup'); og.label = groups[kind];
      OKL.replay.LIST.filter((x) => x.kind === kind).forEach((x) => { const o = document.createElement('option'); o.value = x.id; o.textContent = x.title; og.appendChild(o); });
      sel.appendChild(og);
    }
    sel.value = '05_wind_sweep';
    sel.addEventListener('change', () => selectReplay(sel.value));
    $('rpPlay').addEventListener('click', () => {
      const R = OKL.replay.R;
      if (R.playing) { OKL.replay.stop(); for (const id of app.replayActive.keys()) send({ t: 'off', id }); app.replayActive.clear(); }
      else OKL.replay.play();
      updateRpButton();
    });
    OKL.replay.R.onChange = updateRpButton;
    const tl = $('rpTimeline');
    tl.addEventListener('pointerdown', (e) => {
      const sc = OKL.replay.R.cur;
      if (!sc || !sc.dur) return;
      const r = tl.getBoundingClientRect();
      const t = (e.clientX - r.left - 8 * OKL.stageScale) / (r.width - 16 * OKL.stageScale) * sc.dur;
      for (const id of app.replayActive.keys()) send({ t: 'off', id });
      app.replayActive.clear();
      send({ t: 'alloff', hard: true });
      OKL.replay.seek(t);
    });
  }

  /* ------------------------------------------------------------------ boot */
  const bootList = $('bootList');
  function bootLine(label, state, value) {
    let li = bootList.querySelector('[data-k="' + label + '"]');
    if (!li) { li = document.createElement('li'); li.dataset.k = label; bootList.appendChild(li); }
    li.innerHTML = label + '<span class="' + state + '">' + value + '</span>';
  }

  function initScreens() {
    OKL.params.build(ctx);
    OKL.hud.init($('hud'));
    OKL.inst.init();
    fit();
    window.addEventListener('resize', fit);
    bindUi();
    bindKeys();
  }

  function onThree() {
    if (app.threeReady) return;
    try {
      OKL.pipe3d.init($('gl'));
      app.threeReady = true;
      fit();
      bootLine('3D  Three.js r160 / WebGL2', 'ok', 'OK');
    } catch (err) {
      bootLine('3D  Three.js / WebGL', 'ng', 'NG');
      console.error(err);
    }
    maybeReady();
  }
  function maybeReady() {
    if (!app.threeReady) return;
    $('btnEngage').disabled = false;
    if (OKL.host === 'plugin' && !app.engaged) engage();       // no click needed: the plugin plays the sound
  }

  async function engage() {
    $('btnEngage').disabled = true;
    bootLine('AUDIO', 'wait', 'starting…');
    try {
      const mode = await OKL.audio.init((s) => console.warn(s));
      OKL.audio.on(onEngine);
      const sr = OKL.audio.ctx.sampleRate;
      if (mode === 'plugin') {
        const os = OKL.audio.os || 1;
        bootLine('HOST  ' + OKL.audio.hostName, 'ok', (sr / 1000).toFixed(1) + ' kHz');
        bootLine('ENGINE  C++ core (plugin)' + (OKL.audio.simd ? ' · ' + OKL.audio.simd : ''), 'ok', (os * sr / 1000).toFixed(0) + ' kHz ×' + os + ' OS');
        $('srVal').textContent = (sr / 1000).toFixed(0) + 'k/' + (os * sr / 1000).toFixed(0) + 'k';
        app.maxVoices = OKL.audio.maxVoices;
      } else {
        bootLine('AUDIO  ' + (mode === 'worklet' ? 'AudioWorklet' : 'ScriptProcessor (fallback)'), 'ok', (sr / 1000).toFixed(1) + ' kHz');
        bootLine('ENGINE  Phase 1 core (JS port)', 'ok', (2 * sr / 1000).toFixed(0) + ' kHz ×2 OS');
        $('srVal').textContent = (sr / 1000).toFixed(0) + 'k/' + (2 * sr / 1000).toFixed(0) + 'k';
      }
      app.engaged = true;
      app.gDirty = true;
      if (mode !== 'plugin') { OKL.audio.setReverb(G.reverb); OKL.audio.setMaster(+$('masterVol').value); }
    } catch (err) {
      bootLine('AUDIO', 'ng', err.message);
      $('btnEngage').disabled = false;
      return;
    }
    bootLine('MIDI', 'wait', 'checking…');
    const res = await OKL.midi.init({
      noteOn: (n, v) => { app.midiHit = performance.now(); noteOn(n, v, 'midi'); },
      noteOff: (n) => { app.midiHit = performance.now(); noteOff(n); },
      pad: (b, i, p) => { app.midiHit = performance.now(); onPad(b, i, p); },
      cc: (key, n) => { app.midiHit = performance.now(); OKL.params.setNorm(key, n, 'midi'); },
      mod: (n) => { app.midiHit = performance.now(); OKL.params.set('fmDepth', n * 0.8, 'midi'); },
      pb: (v) => { app.midiHit = performance.now(); OKL.params.set('glide', v * 12, 'midi'); },
      expr: (n) => { OKL.params.setNorm('bellows', n, 'midi'); },          // the Wind pressure knob's own travel
      sustain: (on) => setSustain(on),
      allOff: () => panic(),
      onActivity: () => { app.midiHit = performance.now(); },
      onDevices: (names) => {
        const el = $('midiName'), led = $('midiLed');
        if (names === null) { el.textContent = 'n/a'; led.classList.remove('on'); return; }
        el.textContent = names.length ? names[0] : 'none';
        el.title = names.join(' / ');
        led.classList.toggle('on', names.length > 0);
      },
    });
    bootLine('MIDI', res === 'ok' ? 'ok' : 'wait', res === 'ok' ? (OKL.host === 'plugin' ? 'from the host' : $('midiName').textContent) : (res === 'unsupported' ? 'not supported by this browser' : 'permission denied'));
    setTimeout(() => $('boot').classList.add('gone'), 450);
    setTimeout(() => { $('boot').style.display = 'none'; }, 1100);
  }

  function boot() {
    initScreens();
    const nS = Object.keys(S1.vis).length, nW = Object.keys(S1.sweeps).length, nK = Object.keys(S1.kcal).length;
    bootLine('DATA  Phase 1', 'ok', nS + ' states · ' + nW + ' sweeps · ' + nK + ' keys');
    bootLine('3D  Three.js r160 / WebGL2', 'wait', 'loading…');
    if (window.THREE) onThree(); else window.addEventListener('okl-three', onThree);
    setTimeout(() => { if (!app.threeReady) bootLine('3D  Three.js r160 / WebGL2', 'ng', 'cannot load (check the network)'); }, 12000);
    $('btnEngage').addEventListener('click', engage);
    requestAnimationFrame(frame);
  }
  boot();
})();
