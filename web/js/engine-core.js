/*
 * OkumuLab 1 — engine core
 *
 * JavaScript port of Labium Phase 1 `labium_core.c` (jet-drive flue pipe +
 * bidirectional waveguide + foot model) and of the Prinzipal 8' design rules
 * in `labium.py`. The whole file is one factory function so that the same
 * source runs in three places:
 *   - the AudioWorklet (real-time voices; its source is built from
 *     OKL_ENGINE_FACTORY.toString(), so it also works from file://)
 *   - the main thread (geometry for the screen, offline checks)
 *   - tools/verify.html (comparison against the Phase 1 numbers)
 *
 * Differences from the C core are marked "RT:" (real-time adaptations).
 */
function OKL_ENGINE_FACTORY() {
  'use strict';

  const PNAMES = ['gate', 'p_chest', 'pallet_tau', 'd', 'l_phys', 'mouth_w', 'cutup', 'flue', 'toe',
    'foot_vol', 'y0', 'noise', 'kappa', 'jet_gain', 'morph', 'c', 'rho', 'gamma', 'mu',
    'loss', 'fm_f', 'fm_d', 'pitch_lock', 'f_target', 'k_cal', 'amp_cap', 'edge', 'jet_bw', 'onset_b', 'onset_t', 'kick', 'v_follow', 'f_design'];
  const NPAR = PNAMES.length;
  const PI = {};
  PNAMES.forEach((n, i) => { PI[n] = i; });
  const P_GATE = 0, P_PCHEST = 1, P_PALLET_TAU = 2, P_D = 3, P_LPHYS = 4, P_MOUTHW = 5, P_CUTUP = 6, P_FLUE = 7,
    P_TOE = 8, P_FOOTVOL = 9, P_Y0 = 10, P_NOISE = 11, P_KAPPA = 12, P_JETGAIN = 13, P_MORPH = 14, P_C = 15,
    P_RHO = 16, P_GAMMA = 17, P_MU = 18, P_LOSS = 19, P_FMF = 20, P_FMD = 21, P_PLOCK = 22, P_FTARGET = 23,
    P_KCAL = 24, P_AMPCAP = 25, P_EDGE = 26, P_JETBW = 27, P_ONSET_B = 28, P_ONSET_T = 29, P_KICK = 30,
    P_VFOLLOW = 31, P_FDESIGN = 32;

  const MMWS = 9.80665;          // Pa per mm water column
  const R_GAS = 8.314462618;
  const P_ATM = 101325.0;
  const CTRL = 32;               // control-rate block (internal samples)
  const TWO_PI = 2 * Math.PI;

  /* ------------------------------------------------------------ gases ---- */
  const GASES = {
    //    M [kg/mol]  gamma   mu [Pa s]
    air: [0.028965, 1.400, 1.81e-5],
    he: [0.0040026, 1.667, 1.96e-5],
    co2: [0.04401, 1.289, 1.47e-5],
  };

  function gasProps(mix, Tc) {
    mix = mix || { air: 1 };
    if (Tc === undefined) Tc = 20.0;
    let tot = 0;
    for (const g in mix) tot += mix[g];
    const T = Tc + 273.15;
    let M = 0, Cv = 0, mu = 0;
    for (const g in mix) {
      const x = mix[g] / tot, G = GASES[g];
      M += x * G[0];
      Cv += x * R_GAS / (G[1] - 1.0);
      mu += x * G[2];
    }
    const gamma = (Cv + R_GAS) / Cv;
    return { c: Math.sqrt(gamma * R_GAS * T / M), rho: P_ATM * M / (R_GAS * T), gamma, mu };
  }

  /* gas knob: -1 = CO2, 0 = air, +1 = helium */
  function gasMix(g) {
    if (g > 1e-6) return { air: 1 - g, he: g };
    if (g < -1e-6) return { air: 1 + g, co2: -g };
    return { air: 1 };
  }

  function midiToHz(m) { return 440.0 * Math.pow(2, (m - 69) / 12); }

  const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'H'];
  /* organ (Helmholtz) names: MIDI 36 = C (8' C), 48 = c, 60 = c1 ... */
  function organName(m) {
    m = Math.round(m);
    const oct = Math.floor((m - 36) / 12);
    const nm = NOTE_NAMES[((m % 12) + 12) % 12];
    if (oct < 0) return nm + ','.repeat(-oct);
    if (oct === 0) return nm;
    if (oct === 1) return nm.toLowerCase();
    return nm.toLowerCase() + String(oct - 1);
  }

  function interp(x, xs, ys) {
    if (x <= xs[0]) return ys[0];
    for (let i = 1; i < xs.length; i++) {
      if (x <= xs[i]) {
        const f = (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
        return ys[i - 1] + f * (ys[i] - ys[i - 1]);
      }
    }
    return ys[ys.length - 1];
  }

  /* --------------------------------------------------- Prinzipal 8' ---- */
  function defaultIsing(midi) { return interp(midi, [36, 72], [2.0, 2.4]); }

  /* Dimensions of one Prinzipal 8' pipe from the literature rules (labium.py) */
  function designPrinzipal(midi, o) {
    o = o || {};
    const scaleHT = o.scaleHT !== undefined ? o.scaleHT : -2.0;
    const mouthFrac = o.mouthFrac !== undefined ? o.mouthFrac : 0.25;
    // v1.0 (plugin): voiced for the windchest's reference pressure, 500 Pa (src/dsp: designPrinzipal, Engine::kVoicingPa)
    const pChestMMWS = o.pChestMMWS !== undefined ? o.pChestMMWS : 500.0 / MMWS;
    const flue0 = o.flue !== undefined ? o.flue : 0.65e-3;
    const flueExp = 0.45, footRatio = 0.7, footLen = 0.17;
    const g = gasProps(o.gas, o.Tc);
    const f0 = midiToHz(midi);
    const k = midi - 36;
    const d = 0.1555 * Math.pow(2, -(k - scaleHT) / 16);       // Normalmensur, halving on the 17th
    const ising = o.ising !== undefined ? o.ising : defaultIsing(midi);
    const flue = flue0 * Math.pow(d / 0.0504, flueExp);
    const H = mouthFrac * Math.PI * d;
    const Sflue = flue * H;
    const Stoe = Sflue / Math.sqrt(1.0 / footRatio - 1.0);
    const toe = Math.sqrt(4.0 * Stoe / Math.PI);
    const toeConst = Math.pow(toe * 1e3, 2) / (4.0 * mouthFrac * d * 1e3);
    const pw = pChestMMWS * MMWS;
    const pf = pw / (1.0 + Math.pow(Sflue / Stoe, 2));
    const W = Math.pow(2.0 * pf * flue / (g.rho * ising * ising * f0 * f0), 1 / 3);  // Ising
    const M = 0.3 * d * d / W;
    const Leff = g.c / (2.0 * f0);
    const lPhys = Leff - M - 0.3 * d;
    const footVol = Math.PI / 12.0 * footLen * (d * d + d * toe + toe * toe);
    return {
      midi, name: organName(midi), f0, d, mouth_w: H, cutup: W, flue, toe, toe_const: toeConst, foot_vol: footVol,
      foot_len: footLen, l_phys: lPhys, L_eff: Leff, M, p_chest: pw, p_foot: pf, U_j: Math.sqrt(2 * pf / g.rho),
      ising, scale_ht: scaleHT, mouth_frac: mouthFrac, c: g.c, rho: g.rho, gamma: g.gamma, mu: g.mu,
    };
  }

  let KCAL_KEYS = null, KCAL_VALS = null;
  function setKcal(table) {
    const keys = Object.keys(table).map(Number).sort((a, b) => a - b);
    KCAL_KEYS = keys;
    KCAL_VALS = keys.map((k) => table[String(k)]);
  }
  function kcalFor(midi) { return KCAL_KEYS ? interp(midi, KCAL_KEYS, KCAL_VALS) : 1.0; }

  function baseParams(pd, kcal) {
    const v = new Float64Array(NPAR);
    v[P_GATE] = 1.0;
    v[P_PCHEST] = pd.p_chest;
    v[P_PALLET_TAU] = 0.012;
    v[P_D] = pd.d; v[P_LPHYS] = pd.l_phys; v[P_MOUTHW] = pd.mouth_w; v[P_CUTUP] = pd.cutup;
    v[P_FLUE] = pd.flue; v[P_TOE] = pd.toe; v[P_FOOTVOL] = pd.foot_vol;
    v[P_C] = pd.c; v[P_RHO] = pd.rho; v[P_GAMMA] = pd.gamma; v[P_MU] = pd.mu;
    v[P_Y0] = 0.5 * 0.4 * pd.flue;
    v[P_NOISE] = 0.008;
    v[P_KAPPA] = 1.36;
    v[P_JETGAIN] = 1.0;
    v[P_MORPH] = 0.0;
    v[P_LOSS] = 2.5;
    v[P_FMF] = 0.0; v[P_FMD] = 0.0;
    v[P_PLOCK] = 1.0;
    v[P_FTARGET] = pd.f0;
    v[P_KCAL] = kcal;
    v[P_AMPCAP] = 4.0;
    v[P_EDGE] = 0.0;
    v[P_JETBW] = 0.16;
    v[P_ONSET_B] = 0.0;
    v[P_ONSET_T] = 15.0;
    v[P_KICK] = -0.5;
    v[P_VFOLLOW] = 0.0;
    v[P_FDESIGN] = pd.f0;
    return v;
  }

  /*
   * Performance parameters (what the knobs move). Every voice is the canonical
   * Prinzipal 8' pipe of its key, built in air at 20 °C and voiced for 500 Pa (v1.0; Phase 2: 75 mmWS);
   * the knobs then deform that built pipe (they do not re-design it).
   */
  const DEFAULT_G = {
    wind: 75,          // windchest pressure [mmWS]
    bellows: 1.0,      // expression pedal multiplier
    trem: 0.0,         // tremulant depth 0..1 (±15 % wind)
    tremRate: 5.6,     // [Hz]
    velSens: 0.7,      // velocity -> pallet speed
    cutup: 1.0,        // cut-up factor
    y0b: 0.5,          // labium offset in jet half-widths b
    mouthFrac: 0.25,   // mouth width / circumference
    noise: 0.008,      // jet turbulence
    toe: 1.0,          // toe-hole diameter factor
    scaleHT: -2.0,     // scale (Normalmensur half-tones)
    morph: 0.0,        // 0 open .. 1 stopped
    glide: 0.0,        // length glide [semitones]
    sideHole: 0.0,     // side hole position (screen only for now)
    pitchLock: 1.0,
    vFollow: 1.0,      // voicing follow (cut-up tracks the resonance)
    gas: 0.0,          // -1 CO2 .. 0 air .. +1 He
    tempC: 20.0,
    cMult: 1.0,        // speed of sound multiplier (non-physical)
    rhoMult: 1.0,      // density multiplier (non-physical)
    fmDepth: 0.0,      // wind FM depth (fraction of pressure)
    fmRatio: 1.0,      // wind FM frequency / key frequency
    jetGain: 1.0,      // jet <- resonator feedback gain
    edge: 0.0,         // edge-tone feedback
    kick: -0.5,        // starting-vortex pulse
    loss: 2.5,         // wall-loss multiplier
    kappa: 1.36,       // jet wave-speed coefficient
    reverb: 0.25,      // (main thread)
    master: 1.0,       // engine output gain (the OUT knob is a gain node on the main thread)
  };

  /* per-voice overrides: vo.wind (mmWS, pad pipes), vo.tau (pallet time constant) */
  function voiceParams(midi, G, vo) {
    G = G || DEFAULT_G;
    const pd = designPrinzipal(midi);
    const v = baseParams(pd, kcalFor(midi));
    const k = midi - 36;
    const d = 0.1555 * Math.pow(2, -(k - G.scaleHT) / 16);
    v[P_D] = d;
    v[P_MOUTHW] = G.mouthFrac * Math.PI * d;
    v[P_CUTUP] = pd.cutup * G.cutup;
    v[P_Y0] = G.y0b * 0.4 * pd.flue;
    v[P_TOE] = pd.toe * G.toe;
    const wind = (vo && vo.wind !== undefined) ? vo.wind : G.wind * G.bellows;
    v[P_PCHEST] = Math.max(0, wind) * MMWS;
    if (vo && vo.tau) v[P_PALLET_TAU] = vo.tau;
    v[P_NOISE] = G.noise;
    v[P_MORPH] = Math.min(1, Math.max(0, G.morph));
    v[P_PLOCK] = Math.min(1, Math.max(0, G.pitchLock));
    v[P_VFOLLOW] = G.vFollow;
    const s = G.glide;
    const corr = pd.L_eff - pd.l_phys;
    v[P_LPHYS] = pd.L_eff * Math.pow(2, -s / 12) - corr;
    v[P_FTARGET] = pd.f0 * Math.pow(2, s / 12);
    const gp = gasProps(gasMix(G.gas), G.tempC);
    v[P_C] = gp.c * G.cMult;
    v[P_RHO] = gp.rho * G.rhoMult;
    v[P_GAMMA] = gp.gamma;
    v[P_MU] = gp.mu;
    v[P_FMF] = G.fmRatio * v[P_FTARGET];
    v[P_FMD] = G.fmDepth;
    v[P_JETGAIN] = G.jetGain;
    v[P_EDGE] = G.edge;
    v[P_KICK] = G.kick;
    v[P_LOSS] = G.loss;
    v[P_KAPPA] = G.kappa;
    return v;
  }

  /* ------------------------------------------------------ derived ---- */
  function lossGain(f, a, am, Leff, c, rho, gam, mu, loss, morph) {
    const Pr = 0.71;
    const w = TWO_PI * f;
    const nu = mu / rho;
    const alpha = Math.sqrt(w * nu / 2.0) / (a * c) * (1.0 + (gam - 1.0) / Math.sqrt(Pr));
    const k = w / c;
    const radTop = 0.5 * (k * a) * (k * a) * (1.0 - morph);
    const radMouth = 0.5 * (k * am) * (k * am);
    return Math.exp(-2.0 * alpha * Leff * loss - radTop - radMouth);
  }

  function newDerived() {
    return { dline: 3, lb0: 1, lb1: 0, la1: 0, lp_delay: 0, taum: 0, a_morph: 0, M: 0, Leff: 0, fres: 0, W: 0 };
  }

  const _Gt = new Float64Array(3), _cw = new Float64Array(3), _fa = new Float64Array(3);
  let _ru = 0, _rv = 0;
  /* residual of the third fit point for a given pole a1 (no allocation: audio thread) */
  function _resid(a1) {
    const Gt = _Gt, cw = _cw;
    const y0 = Gt[0] * Gt[0] * (1 + a1 * a1 + 2 * a1 * cw[0]);
    const y1 = Gt[1] * Gt[1] * (1 + a1 * a1 + 2 * a1 * cw[1]);
    const v = (y0 - y1) / (cw[0] - cw[1]);
    const u = y0 - v * cw[0];
    const y2 = Gt[2] * Gt[2] * (1 + a1 * a1 + 2 * a1 * cw[2]);
    _ru = u; _rv = v;
    return (u + v * cw[2]) - y2;
  }

  function derive(p, fs, dv) {
    const c = p[P_C];
    const d = p[P_D], a = 0.5 * d;
    const H = p[P_MOUTHW];
    let W = p[P_CUTUP];
    let morph = p[P_MORPH];
    if (morph < 0) morph = 0;
    if (morph > 1) morph = 1;

    let M = 0, Leff = 0, fres = 0;
    for (let pass = 0; pass < 2; ++pass) {
      M = 0.3 * d * d / W;
      const LphysEff = p[P_LPHYS] + M + 0.3 * d * (1.0 - morph);
      const Llock = c / (2.0 * p[P_FTARGET]) * p[P_KCAL];
      Leff = (1.0 - p[P_PLOCK]) * LphysEff + p[P_PLOCK] * Llock;
      if (Leff < 0.005) Leff = 0.005;
      fres = c / (2.0 * Leff) * (1.0 - 0.5 * morph);
      if (pass === 0 && p[P_VFOLLOW] > 0 && p[P_FDESIGN] > 0)
        W = W * Math.pow(fres / p[P_FDESIGN], -2.0 / 3.0 * p[P_VFOLLOW]);
      else
        break;
    }
    dv.W = W;

    const am = Math.sqrt(H * W / Math.PI);
    const f1 = c / (2.0 * Leff);
    _fa[0] = f1; _fa[1] = 3.0 * f1; _fa[2] = 9.0 * f1;
    for (let i = 0; i < 3; ++i) if (_fa[i] > 0.4 * fs) _fa[i] = 0.4 * fs * (0.6 + 0.2 * i);
    for (let i = 0; i < 3; ++i) {
      _Gt[i] = lossGain(_fa[i], a, am, Leff, c, p[P_RHO], p[P_GAMMA], p[P_MU], p[P_LOSS], morph);
      _cw[i] = Math.cos(TWO_PI * _fa[i] / fs);
    }
    const Gt = _Gt, cw = _cw;
    let bestA1 = 0.0, bestU = Gt[0] * Gt[0], bestV = 0.0, bestR = 1e30;
    const lo = -0.9995, hi = 0.9995;
    let rlo = 0, rhi = 0;
    {
      let prevA = lo, prevR = 0, found = false;
      for (let k = 0; k <= 200 && !found; ++k) {
        const a1 = lo + (hi - lo) * k / 200.0;
        const r = _resid(a1), u = _ru, v = _rv;
        if (u >= Math.abs(v) && Math.abs(r) < bestR) { bestR = Math.abs(r); bestA1 = a1; bestU = u; bestV = v; }
        if (k > 0 && prevR * r < 0) { rlo = prevA; rhi = a1; found = true; }
        prevA = a1; prevR = r;
      }
      if (found) {
        for (let it = 0; it < 50; ++it) {
          const a1 = 0.5 * (rlo + rhi);
          const r = _resid(a1), u = _ru, v = _rv;
          const rl = _resid(rlo);
          if (rl * r <= 0) rhi = a1; else rlo = a1;
          if (u >= Math.abs(v)) { bestA1 = a1; bestU = u; bestV = v; bestR = Math.abs(r); }
        }
      }
    }
    const sp = Math.sqrt(Math.max(bestU + bestV, 0.0)), sm = Math.sqrt(Math.max(bestU - bestV, 0.0));
    dv.lb0 = 0.5 * (sp + sm);
    dv.lb1 = 0.5 * (sp - sm);
    dv.la1 = bestA1;
    {
      const w = TWO_PI * f1 / fs;
      const nr = dv.lb0 + dv.lb1 * Math.cos(w), ni = -dv.lb1 * Math.sin(w);
      const dr = 1.0 + dv.la1 * Math.cos(w), di = -dv.la1 * Math.sin(w);
      const ph = Math.atan2(ni, nr) - Math.atan2(di, dr);
      dv.lp_delay = -ph / w;
    }
    dv.taum = M / c;
    const fx = 0.5 * fs * Math.pow(5.0 / (0.5 * fs), morph);
    const t = Math.tan(Math.min(Math.PI * fx / fs, Math.PI / 2.0 - 1e-9));
    dv.a_morph = (t - 1.0) / (t + 1.0);
    let D = ((2.0 * Leff - 2.0 * M) * fs / c - dv.lp_delay) / 2.0;
    if (D < 3.0) D = 3.0;
    dv.dline = D;
    dv.M = M;
    dv.Leff = Leff;
    dv.fres = fres;
    return dv;
  }

  /* --------------------------------------------------- delay line ---- */
  class DL {
    constructor(minlen) {
      let n = 1;
      while (n < minlen) n <<= 1;
      this.buf = new Float64Array(n);
      this.mask = n - 1;
      this.w = 0;
    }
    clear() { this.buf.fill(0); this.w = 0; }
  }
  /* 3rd-order Lagrange read, D samples ago (before this sample's write) */
  function dlRead(dl, D) {
    const pos = dl.w - D;
    const fl = Math.floor(pos);
    const f = pos - fl;
    const b = dl.buf, m = dl.mask;
    const ym1 = b[(fl - 1) & m], y0 = b[fl & m], y1 = b[(fl + 1) & m], y2 = b[(fl + 2) & m];
    const cm1 = -f * (f - 1.0) * (f - 2.0) / 6.0;
    const c0 = (f + 1.0) * (f - 1.0) * (f - 2.0) / 2.0;
    const c1 = -(f + 1.0) * f * (f - 2.0) / 2.0;
    const c2 = (f + 1.0) * f * (f - 1.0) / 6.0;
    return cm1 * ym1 + c0 * y0 + c1 * y1 + c2 * y2;
  }
  /* linear read relative to the most recent write (snapshots) */
  function dlReadAfter(dl, D) {
    const pos = (dl.w - 1) - D;
    const fl = Math.floor(pos);
    const f = pos - fl;
    return dl.buf[fl & dl.mask] * (1.0 - f) + dl.buf[(fl + 1) & dl.mask] * f;
  }

  /* xorshift32 (the C core uses xorshift64; any white source will do) */
  function makeRng(seed) {
    let s = (seed * 2654435761) >>> 0 || 1;
    return function () {
      s ^= s << 13; s >>>= 0;
      s ^= s >>> 17;
      s ^= s << 5; s >>>= 0;
      return s / 4294967296;
    };
  }

  /* ----------------------------------------------------------- voice ---- */
  const DL_LEN = 32768;       // RT: fixed buffers (≈ 0.34 s at 96 kHz)
  const K_SNAP = 48, NX_SNAP = 48;

  class Voice {
    constructor(fs, seed) {
      this.fs = fs;
      this.up = new DL(DL_LEN);
      this.lo = new DL(DL_LEN);
      this.vml = new DL(DL_LEN);
      this.maxTau = Math.min(fs * 0.25, DL_LEN - 8);
      this.A = new Float64Array(NPAR);     // control frame i
      this.B = new Float64Array(NPAR);     // control frame i+1
      this.T = new Float64Array(NPAR);     // target (RT: smoothed toward)
      this.dv0 = newDerived();
      this.dv1 = newDerived();
      this.rnd = makeRng(seed || 1);
      this.k_dq = 1.0 - Math.exp(-TWO_PI * 12000.0 / fs);
      this.k_nz = 1.0 - Math.exp(-TWO_PI * 4000.0 / fs);
      this.smooth = 1.0 - Math.exp(-CTRL / (fs * 0.012));   // RT: ~12 ms parameter glide
      this.capK = new Float32Array(0);
      this.active = false;
      this.reset();
    }

    reset() {
      this.up.clear(); this.lo.clear(); this.vml.clear();
      this.pg = 0; this.pf = 0;
      this.qin_prev = 0; this.dq_lp = 0; this.vm = 0; this.lp_y = 0;
      this.lsh_x1 = 0; this.lsh_y1 = 0; this.dc_x1 = 0; this.dc_y1 = 0;
      this.apm_x1 = 0; this.apm_y1 = 0; this.mq_prev = 0; this.mu_prev = 0;
      this.vj = 0; this.vj2 = 0; this.gate_on = 0; this.onset_t = 0; this.kick_t = -1;
      this.um_prev = 0; this.ut_prev = 0; this.nz = 0; this.phi = 0;
      this.resets = 0; this.fpos = 0; this.started = false; this.tau = -1;
      this.trem = 1.0;
      // telemetry (last sample)
      this.eta = 0; this.etaN = 0; this.inflow = 0.5; this.Uj = 0; this.om = 0; this.ot = 0;
      this.peak = 0; this.quiet = 0;
      this.capState = 0;
    }

    /* start from silence (fresh pallet) */
    start(target) {
      this.reset();
      this.T.set(target);
      this.A.set(target);
      this.B.set(target);
      derive(this.A, this.fs, this.dv0);
      derive(this.B, this.fs, this.dv1);
      this.started = true;
      this.active = true;
    }

    setTarget(target) { this.T.set(target); }

    /* advance to the next control frame (RT: targets are glided, derive() only when something moved) */
    _frame() {
      const A = this.A, B = this.B, T = this.T;
      A.set(B);
      const dv = this.dv0; this.dv0 = this.dv1; this.dv1 = dv;
      let moved = false;
      const a = this.smooth;
      for (let j = 0; j < NPAR; j++) {
        const tj = T[j];
        const bo = B[j];
        let bj;
        if (j === P_GATE || j === P_PALLET_TAU || j === P_FMF) bj = tj;      // the pallet model smooths the gate
        else {
          bj = bo + (tj - bo) * a;
          if (Math.abs(tj - bj) <= 1e-9 * Math.abs(tj) + 1e-15) bj = tj;
        }
        if (bj !== bo) { moved = true; B[j] = bj; }
      }
      if (moved) {
        derive(B, this.fs, this.dv1);
      } else {
        const d0 = this.dv0, d1 = this.dv1;
        d1.dline = d0.dline; d1.lb0 = d0.lb0; d1.lb1 = d0.lb1; d1.la1 = d0.la1; d1.lp_delay = d0.lp_delay;
        d1.taum = d0.taum; d1.a_morph = d0.a_morph; d1.M = d0.M; d1.Leff = d0.Leff; d1.fres = d0.fres; d1.W = d0.W;
      }
    }

    /*
     * Render n internal samples, adding the stereo mix (gain applied by caller)
     * into mixL/mixR. If `focus` is given, the mouth signal is written there and
     * period snapshots are taken when requested.
     *
     * RT: quantities that only depend on slowly varying parameters (areas, filter
     * coefficients, jet gain, jet delay, ...) are computed once per control frame
     * (32 samples); the delay-line lengths are still interpolated per sample. With
     * constant parameters this is the same computation as the C core.
     */
    render(n, mixL, mixR, focus) {
      const fs = this.fs, up = this.up, lo = this.lo, vml = this.vml;
      const rnd = this.rnd;
      const k_dq = this.k_dq, k_nz = this.k_nz;
      const maxTau = this.maxTau;
      let pg = this.pg, pf = this.pf, qin_prev = this.qin_prev, dq_lp = this.dq_lp, vm = this.vm;
      let lsh_x1 = this.lsh_x1, lsh_y1 = this.lsh_y1, dc_x1 = this.dc_x1, dc_y1 = this.dc_y1;
      let apm_x1 = this.apm_x1, apm_y1 = this.apm_y1, mq_prev = this.mq_prev, mu_prev = this.mu_prev;
      let vj = this.vj, vj2 = this.vj2, gate_on = this.gate_on, onset_t = this.onset_t, kick_t = this.kick_t;
      let um_prev = this.um_prev, ut_prev = this.ut_prev, nz = this.nz, phi = this.phi;
      let fpos = this.fpos;
      let peak = this.peak;
      let tau = this.tau;
      const trem = this.trem;
      const capOn = this.capState === 1 || this.capState === 2;
      let eta = 0, qin = 0, Uj = this.Uj, om = 0, ot = 0;
      const sigOut = focus ? focus.sig : null;
      let sigPos = focus ? focus.sigPos : 0;
      // frame constants
      let c = 343, rho = 1.2, zc = 412, Spipe = 0, SpSm = 0, Sflue = 0, Stoe = 0, kf = 0, k2r = 0, kp = 0, target = 0;
      let fmd = 0, dphi = 0, kj = 0, hG = 0, b = 1e-4, invb = 1e4, bH = 0, noiseA = 0, y0c = 0, H = 0, pjK = 0, pvK = 0;
      let kick = 0, kickArm = 0, kickStep = 0, edgeK = 0, radK = 0, Km = 1, lb0 = 1, lb1 = 0, la1 = 0, am = 0;
      let dline = 3, dlineStep = 0, tauStep = 0, dv0 = this.dv0;

      for (let t = 0; t < n; ++t) {
        if (fpos === 0) {
          this._frame();
          const A = this.A, dv1 = this.dv1;
          dv0 = this.dv0;
          c = A[P_C]; rho = A[P_RHO]; zc = rho * c;
          const d = A[P_D], h = A[P_FLUE], W = dv0.W;
          H = A[P_MOUTHW];
          Spipe = Math.PI * d * d / 4.0;
          SpSm = Spipe / (H * W);
          Sflue = h * H;
          Stoe = Math.PI * A[P_TOE] * A[P_TOE] / 4.0;
          kf = (1.0 / (fs * 4)) * rho * c * c / A[P_FOOTVOL];
          k2r = 2.0 / rho;
          const tau_p = A[P_PALLET_TAU] > 1e-4 ? A[P_PALLET_TAU] : 1e-4;
          kp = 1.0 - Math.exp(-1.0 / (tau_p * fs));
          const gate = A[P_GATE];
          target = gate * A[P_PCHEST] * trem;                 // RT: tremulant on the wind
          fmd = A[P_FMD]; dphi = TWO_PI * A[P_FMF] / fs;
          // jet
          const fres = dv0.fres;
          const uc = A[P_KAPPA] * Math.pow(Uj, 2.0 / 3.0) * Math.cbrt(TWO_PI * fres * h);
          let tauN = W / uc * fs;
          if (tauN < 3.0) tauN = 3.0;
          if (tauN > maxTau) tauN = maxTau;
          if (tau < 0) tau = tauN;
          tauStep = (tauN - tau) / CTRL;
          let fjet = A[P_JETBW] * Uj / h;
          if (fjet > 0.4 * fs) fjet = 0.4 * fs;
          kj = 1.0 - Math.exp(-TWO_PI * fjet / fs);
          let ampexp = 0.4 * W / h;
          if (ampexp > A[P_AMPCAP]) ampexp = A[P_AMPCAP];
          if (gate > 0.5) {
            if (!gate_on) { gate_on = 1; onset_t = 0.0; }
            onset_t += fres * CTRL / fs;
          } else gate_on = 0;
          if (gate_on && A[P_ONSET_T] > 0) ampexp += A[P_ONSET_B] * Math.exp(-onset_t / A[P_ONSET_T]);
          hG = A[P_JETGAIN] * Math.exp(ampexp) * h;
          b = 0.4 * h; invb = 1 / b; bH = b * H;
          noiseA = A[P_NOISE] * h * 3.0;
          y0c = A[P_Y0];
          pjK = -rho * (4.0 / Math.PI * Math.sqrt(2.0 * h * W)) / (W * H);
          pvK = -0.5 * rho / 0.36;
          kick = gate_on ? A[P_KICK] : 0; kickArm = 0.5 * A[P_PCHEST] * 0.3; kickStep = fres / fs;
          if (!gate_on) kick_t = -1.0;
          edgeK = A[P_EDGE] / (H * W);
          radK = rho / (4.0 * Math.PI) * fs;
          Km = 2.0 * dv0.taum * fs; lb0 = dv0.lb0; lb1 = dv0.lb1; la1 = dv0.la1; am = dv0.a_morph;
          dline = dv0.dline; dlineStep = (dv1.dline - dv0.dline) / CTRL;
        }

        /* ---- wind: pallet + foot ---- */
        pg += (target - pg) * kp;
        let pge = pg;
        if (fmd !== 0) {
          pge = pg * (1.0 + fmd * Math.sin(phi));
          phi += dphi;
          if (phi > TWO_PI) phi -= TWO_PI;
        }
        for (let s = 0; s < 4; ++s) {
          const dp = pge - pf;
          const qt = dp > 0 ? Stoe * Math.sqrt(k2r * dp) : (dp < 0 ? -Stoe * Math.sqrt(-k2r * dp) : 0);
          const qf = pf > 0 ? Sflue * Math.sqrt(k2r * pf) : 0;
          pf += kf * (qt - qf);
        }
        Uj = Math.sqrt(pf > 0 ? k2r * pf : 0) + 1e-3;

        /* ---- jet ---- */
        tau += tauStep;
        const vdel_raw = dlRead(vml, tau);
        vj += (vdel_raw - vj) * kj;
        vj2 += (vj - vj2) * kj;
        nz += ((2.0 * rnd() - 1.0) - nz) * k_nz;
        eta = -hG / Uj * vj2 + noiseA * nz;
        qin = bH * Uj * (1.0 + Math.tanh((eta - y0c) * invb));
        const dq = (qin - qin_prev) * fs;
        qin_prev = qin;
        dq_lp += (dq - dq_lp) * k_dq;
        let ps = pjK * dq_lp + pvK * vm * (vm < 0 ? -vm : vm);
        if (kick !== 0.0) {
          if (kick_t < 0.0 && pf > kickArm) kick_t = 0.0;
          if (kick_t >= 0.0 && kick_t < 0.25) {
            ps += kick * pf * Math.sin(Math.PI * kick_t / 0.25);
            kick_t += kickStep;
          }
        }

        /* ---- waveguide ---- */
        dline += dlineStep;
        const pplus_end = dlRead(up, dline);
        const pminus_m = dlRead(lo, dline);
        const lsh = lb0 * pplus_end + lb1 * lsh_x1 - la1 * lsh_y1;
        lsh_x1 = pplus_end; lsh_y1 = lsh;
        const dcb = lsh - dc_x1 + 0.99995 * dc_y1;
        dc_x1 = lsh; dc_y1 = dcb;
        const apm = am * dcb + apm_x1 - am * apm_y1;
        apm_x1 = dcb;
        apm_y1 = apm;
        const r_top = -apm;

        const uin = ps - 2.0 * pminus_m;
        const mq = ((Km - 1.0) * mq_prev + uin + mu_prev) / (Km + 1.0);
        mq_prev = mq;
        mu_prev = uin;
        const pplus_new = mq + pminus_m;

        lo.buf[lo.w] = r_top; lo.w = (lo.w + 1) & lo.mask;
        up.buf[up.w] = pplus_new; up.w = (up.w + 1) & up.mask;

        const um = (pplus_new - pminus_m) / zc;
        vm = um * SpSm;
        vml.buf[vml.w] = vm + edgeK * (qin - bH * Uj); vml.w = (vml.w + 1) & vml.mask;
        const ut = (pplus_end - r_top) / zc;

        /* ---- radiation (1 m, monopole) ---- */
        const Um = um * Spipe, Ut = ut * Spipe;
        om = -radK * (Um - um_prev);
        ot = radK * (Ut - ut_prev);
        um_prev = Um;
        ut_prev = Ut;

        /* ---- stability guard ---- */
        if (!(Math.abs(pplus_new) <= 1e6) || !(pf === pf)) {
          up.buf.fill(0); lo.buf.fill(0); vml.buf.fill(0);
          apm_x1 = apm_y1 = mq_prev = mu_prev = vj = vj2 = 0.0;
          lsh_x1 = lsh_y1 = dc_x1 = dc_y1 = 0.0;
          vm = dq_lp = 0.0;
          qin_prev = 0.0;
          if (!(pf === pf) || !isFinite(pf)) pf = 0.0;
          om = ot = 0.0;
          this.resets++;
        }

        mixL[t] += 0.85 * om + 0.35 * ot;
        mixR[t] += 0.35 * om + 0.85 * ot;
        const ao = om < 0 ? -om : om;
        if (ao > peak) peak = ao;

        if (sigOut) { sigOut[sigPos++] = om; }

        /* ---- period capture (focus voice only) ---- */
        if (capOn && this.capState < 3) this._capStep(eta, y0c, b, qin, H, Uj, vm, dline, rho, c, dv0);

        fpos++;
        if (fpos === CTRL) fpos = 0;
      }

      this.pg = pg; this.pf = pf; this.qin_prev = qin_prev; this.dq_lp = dq_lp; this.vm = vm;
      this.lsh_x1 = lsh_x1; this.lsh_y1 = lsh_y1; this.dc_x1 = dc_x1; this.dc_y1 = dc_y1;
      this.apm_x1 = apm_x1; this.apm_y1 = apm_y1; this.mq_prev = mq_prev; this.mu_prev = mu_prev;
      this.vj = vj; this.vj2 = vj2; this.gate_on = gate_on; this.onset_t = onset_t; this.kick_t = kick_t;
      this.um_prev = um_prev; this.ut_prev = ut_prev; this.nz = nz; this.phi = phi;
      this.fpos = fpos; this.tau = tau;
      this.Uj = Uj; this.om = om; this.ot = ot;
      this.eta = eta; this.etaN = (eta - y0c) * invb;
      this.inflow = Uj > 0 ? qin / (2.0 * bH * Uj) : 0;
      this.peak = peak;
      if (focus) focus.sigPos = sigPos;
    }

    /* ---------------- period snapshots: K frames over one period ---------------- */
    requestCapture(periodSamples) {
      if (this.capState) return;
      this.capT = periodSamples;
      this.capState = 1;
      this.capWait = 0;
      this.capPrev = this.etaN;
      if (this.capP === undefined) {
        this.capP = new Float32Array(K_SNAP * NX_SNAP);
        this.capU = new Float32Array(K_SNAP * NX_SNAP);
        this.capEta = new Float32Array(K_SNAP);
        this.capIn = new Float32Array(K_SNAP);
        this.capVm = new Float32Array(K_SNAP);
      }
    }

    _capStep(eta, y0c, b, qin, H, Uj, vm, dline, rho, c, dv0) {
      const etaN = (eta - y0c) / b;
      if (this.capState === 1) {
        this.capWait++;
        // start on an upward crossing of the jet deflection (phase-aligned captures)
        if ((this.capPrev < 0 && etaN >= 0) || this.capWait > this.fs * 0.08) {
          this.capState = 2; this.capN = 0; this.capK = 0;
        }
        this.capPrev = etaN;
        if (this.capState !== 2) return;
      }
      const k = this.capK;
      if (this.capN >= Math.round(k * this.capT / K_SNAP)) {
        const P = this.capP, U = this.capU, o = k * NX_SNAP, zc = rho * c;
        for (let i = 0; i < NX_SNAP; i++) {
          const xf = i / (NX_SNAP - 1);
          const pp = dlReadAfter(this.up, xf * dline);
          const pm = dlReadAfter(this.lo, (1.0 - xf) * dline);
          P[o + i] = pp + pm;
          U[o + i] = (pp - pm) / zc;
        }
        this.capEta[k] = etaN;
        this.capIn[k] = Uj > 0 ? qin / (2.0 * b * H * Uj) : 0;
        this.capVm[k] = vm;
        this.capK = k + 1;
        if (this.capK >= K_SNAP) {
          this.capState = 3;     // done: engine collects it
          this.capDline = dline; this.capLeff = dv0.Leff;
        }
      }
      this.capN++;
    }
  }

  /* --------------------------------------------- halfband decimator ---- */
  function makeHalfband(ntaps) {
    // windowed-sinc halfband lowpass (cutoff fs/4), odd length
    const N = ntaps, M = (N - 1) / 2, h = new Float64Array(N);
    let sum = 0;
    for (let i = 0; i < N; i++) {
      const x = i - M;
      const s = x === 0 ? 0.5 : Math.sin(Math.PI * x / 2) / (Math.PI * x);
      const w = 0.42 - 0.5 * Math.cos(TWO_PI * i / (N - 1)) + 0.08 * Math.cos(4 * Math.PI * i / (N - 1)); // Blackman
      h[i] = s * w; sum += h[i];
    }
    for (let i = 0; i < N; i++) h[i] /= sum;
    return h;
  }

  class Decimator {
    constructor(taps) {
      this.h = makeHalfband(taps || 47);
      this.N = this.h.length;
      this.buf = new Float64Array(this.N * 2);
      this.pos = 0;
    }
    /* in: 2n samples -> out: n samples */
    process(inp, out, n) {
      const h = this.h, N = this.N, buf = this.buf;
      let pos = this.pos;
      for (let i = 0; i < n; i++) {
        for (let k = 0; k < 2; k++) {
          const x = inp[2 * i + k];
          buf[pos] = x; buf[pos + N] = x;
          pos = (pos + 1) % N;
        }
        let acc = 0;
        // buf[pos .. pos+N-1] holds the last N samples oldest -> newest
        for (let j = 0; j < N; j++) {
          const hj = h[j];
          if (hj !== 0) acc += hj * buf[pos + j];
        }
        out[i] = acc;
      }
      this.pos = pos;
    }
  }

  /* 2nd-order Butterworth high-pass (DC / subsonic removal, 18 Hz like stereo_mix) */
  class HP2 {
    constructor(fc, fs) {
      const w = Math.tan(Math.PI * fc / fs), k = Math.SQRT2;
      const n = 1 / (1 + k * w + w * w);
      this.b0 = n; this.b1 = -2 * n; this.b2 = n;
      this.a1 = 2 * (w * w - 1) * n; this.a2 = (1 - k * w + w * w) * n;
      this.x1 = this.x2 = this.y1 = this.y2 = 0;
    }
    run(x) {
      const y = this.b0 * x + this.b1 * this.x1 + this.b2 * this.x2 - this.a1 * this.y1 - this.a2 * this.y2;
      this.x2 = this.x1; this.x1 = x; this.y2 = this.y1; this.y1 = y;
      return y;
    }
  }

  /* -------------------------------------------------------- engine ---- */
  const GAIN = 0.9;     // full scale per Pa at 1 m (same as labium.py)

  class Engine {
    /* fsOut: output rate; internal rate = 2 x fsOut (as in Phase 1: 96 kHz -> 48 kHz) */
    constructor(fsOut, maxVoices) {
      this.fsOut = fsOut;
      this.fs = 2 * fsOut;
      this.maxVoices = maxVoices || 10;
      this.voices = [];
      for (let i = 0; i < this.maxVoices; i++) {
        const v = new Voice(this.fs, 17 + i * 7919);
        v.id = null; v.midi = 60; v.vo = null; v.age = 0; v.gate = 0;
        this.voices.push(v);
      }
      this.G = Object.assign({}, DEFAULT_G);
      this.mixL = new Float64Array(256);
      this.mixR = new Float64Array(256);
      this.decL = new Decimator(47);
      this.decR = new Decimator(47);
      this.decS = new Decimator(47);
      this.hpL = new HP2(18, fsOut);
      this.hpR = new HP2(18, fsOut);
      this.outL = new Float64Array(128);
      this.outR = new Float64Array(128);
      this.env = 0;              // limiter envelope
      this.gr = 1;               // limiter gain
      this.grMin = 1;
      this.mute = false;
      this.focusId = null;
      this.ageCounter = 0;
      this.tremPhase = 0;
      this.sustain = false;
      // focus signal (decimated to fsOut) for pitch/level/harmonics on the main thread
      this.focus = { sig: new Float64Array(512), sigPos: 0 };
      this.sigOut = new Float32Array(8192);
      this.sigN = 0;
      this.sigDec = new Float64Array(256);
      this.capTimer = 0;
      this.capInterval = 0.09;   // seconds between period captures
      this.focusPeriodSec = 0;
      this.pending = [];         // finished captures
    }

    setGlobals(G) {
      Object.assign(this.G, G);
      for (const v of this.voices) if (v.active) v.setTarget(this._target(v));
    }

    _target(v) {
      const p = voiceParams(v.midi, this.G, v.vo);
      p[P_GATE] = v.gate;
      return p;
    }

    findVoice(id) {
      for (const v of this.voices) if (v.active && v.id === id) return v;
      return null;
    }

    noteOn(id, midi, vel, vo) {
      vel = vel === undefined ? 100 : vel;
      vo = Object.assign({}, vo || {});
      if (!vo.tau) {
        // velocity -> pallet speed: 0.012 s at mid velocity, 4..40 ms range
        const s = this.G.velSens;
        const x = (vel / 127 - 0.6);
        vo.tau = 0.012 * Math.pow(10, -x * 1.3 * s);
      }
      let v = this.findVoice(id);
      if (v && v.midi === midi) {
        v.gate = 1; v.vo = vo; v.held = false; v.age = ++this.ageCounter;
        v.setTarget(this._target(v));
        this.focusId = id;
        return v;
      }
      if (v) { v.active = false; v.id = null; }
      v = this.voices.find((x) => !x.active);
      if (!v) {
        // steal: released voices first (quietest), otherwise the oldest
        let best = null, bestScore = Infinity;
        for (const x of this.voices) {
          const score = (x.gate ? 1e9 : 0) + (x.gate ? x.age : x.peakLevel || 0);
          if (score < bestScore) { bestScore = score; best = x; }
        }
        v = best;
      }
      v.id = id; v.midi = midi; v.vo = vo; v.gate = 1; v.held = false; v.age = ++this.ageCounter;
      v.start(this._target(v));
      this.focusId = id;
      return v;
    }

    noteOff(id) {
      const v = this.findVoice(id);
      if (!v) return;
      if (this.sustain) { v.held = true; return; }
      v.gate = 0;
      v.setTarget(this._target(v));
    }

    setSustain(on) {
      this.sustain = on;
      if (!on) for (const v of this.voices) if (v.active && v.held) { v.held = false; v.gate = 0; v.setTarget(this._target(v)); }
    }

    voiceOverride(id, vo) {
      const v = this.findVoice(id);
      if (!v) return;
      Object.assign(v.vo, vo);
      v.setTarget(this._target(v));
    }

    allOff(hard) {
      for (const v of this.voices) {
        if (!v.active) continue;
        if (hard) { v.active = false; v.id = null; }
        else { v.gate = 0; v.held = false; v.setTarget(this._target(v)); }
      }
    }

    focusVoice() {
      if (this.focusId === null) return null;
      return this.findVoice(this.focusId);
    }

    /* render one block of n output samples (n <= 128) into outL/outR (Float32Array) */
    process(oL, oR, n) {
      const n2 = 2 * n, mixL = this.mixL, mixR = this.mixR;
      mixL.fill(0, 0, n2); mixR.fill(0, 0, n2);
      const G = this.G;
      // tremulant (shared wind): per block is fine at 2.7 ms
      this.tremPhase += TWO_PI * G.tremRate * n / this.fsOut;
      if (this.tremPhase > TWO_PI) this.tremPhase -= TWO_PI;
      const trem = 1 + 0.15 * G.trem * Math.sin(this.tremPhase);
      const fv = this.focusVoice();
      const F = this.focus;
      F.sigPos = 0;
      let nActive = 0;
      for (const v of this.voices) {
        if (!v.active) continue;
        nActive++;
        v.trem = trem;
        v.peak = 0;
        v.render(n2, mixL, mixR, v === fv ? F : null);
        // free silent released voices
        if (!v.gate) {
          if (v.pg < 0.02 * MMWS && v.peak < 2e-5) v.quiet += n2; else v.quiet = 0;
          if (v.quiet > this.fs * 0.15) { v.active = false; v.id = null; }
        }
        v.peakLevel = v.peak;
      }
      this.nActive = nActive;

      // focus signal -> fsOut
      if (fv && F.sigPos === n2) {
        this.decS.process(F.sig, this.sigDec, n);
        for (let i = 0; i < n; i++) {
          if (this.sigN < this.sigOut.length) this.sigOut[this.sigN++] = this.sigDec[i];
        }
        // period captures
        this.capTimer += n / this.fsOut;
        if (fv.capState === 3) {
          this.pending.push(this._collect(fv));
          fv.capState = 0;
        } else if (!fv.capState && this.capTimer >= this.capInterval) {
          this.capTimer = 0;
          // one period of the sounding frequency (measured on the main thread), else the resonance
          const T = this.focusPeriodSec > 0 ? this.focusPeriodSec * this.fs : this.fs / Math.max(10, fv.dv0.fres);
          fv.requestCapture(T);
        }
      }

      this.decL.process(mixL, this.outL, n);
      this.decR.process(mixR, this.outR, n);
      const g = GAIN * G.master * (this.mute ? 0 : 1);
      // limiter: instant attack, 150 ms release, -1 dBFS ceiling
      const rel = Math.exp(-1 / (0.15 * this.fsOut));
      const thr = 0.89;
      let env = this.env, grMin = 1;
      for (let i = 0; i < n; i++) {
        let l = this.hpL.run(this.outL[i]) * g;
        let r = this.hpR.run(this.outR[i]) * g;
        const a = Math.max(Math.abs(l), Math.abs(r));
        env = a > env ? a : env * rel + a * (1 - rel);
        const gr = env > thr ? thr / env : 1;
        if (gr < grMin) grMin = gr;
        l *= gr; r *= gr;
        oL[i] = l > 1 ? 1 : (l < -1 ? -1 : l);
        oR[i] = r > 1 ? 1 : (r < -1 ? -1 : r);
      }
      this.env = env;
      this.grMin = Math.min(this.grMin, grMin);
    }

    _collect(v) {
      return {
        id: v.id, midi: v.midi, K: K_SNAP, NX: NX_SNAP,
        p: v.capP.slice(), u: v.capU.slice(), eta: v.capEta.slice(), inflow: v.capIn.slice(), vm: v.capVm.slice(),
        T: v.capT, fs: this.fs, dline: v.capDline, Leff: v.capLeff,
      };
    }

    /* telemetry for the screen */
    telemetry() {
      const vox = [];
      for (const v of this.voices) {
        if (!v.active) continue;
        vox.push({ id: v.id, midi: v.midi, gate: v.gate, peak: v.peakLevel || 0, pf: v.pf, age: v.age });
      }
      const fv = this.focusVoice();
      let f = null;
      if (fv) {
        const dv = fv.dv0, p = fv.A;
        f = {
          id: fv.id, midi: fv.midi, gate: fv.gate, pg: fv.pg, pf: fv.pf, Uj: fv.Uj, etaN: fv.etaN, inflow: fv.inflow,
          vm: fv.vm, resets: fv.resets, fres: dv.fres, Leff: dv.Leff, M: dv.M, W: dv.W, dline: dv.dline,
          d: p[P_D], H: p[P_MOUTHW], h: p[P_FLUE], toe: p[P_TOE], rho: p[P_RHO], c: p[P_C], kappa: p[P_KAPPA],
          y0: p[P_Y0], morph: p[P_MORPH], ftarget: p[P_FTARGET], ampcap: p[P_AMPCAP], pchest: p[P_PCHEST],
        };
      }
      const gr = this.grMin; this.grMin = 1;
      return { vox, focus: f, gr };
    }

    takeSignal() {
      const out = this.sigOut.slice(0, this.sigN);
      this.sigN = 0;
      return out;
    }
  }

  /* ------------------------------------------------------------ host ---- */
  /*
   * Message protocol shared by the AudioWorklet and the ScriptProcessor fallback.
   * main -> host: {t:'G'|'on'|'off'|'vo'|'focus'|'period'|'mute'|'sustain'|'alloff'}
   * host -> main every ~16 ms: {t:'tel', tel, sig (focus mouth signal), caps (period snapshots), cpu}
   * In Phase 4 the C++ processor sends the same 'tel' / caps objects to the WebView.
   */
  class Host {
    constructor(fsOut, opts, post, now) {
      opts = opts || {};
      if (opts.kcal) setKcal(opts.kcal);
      this.eng = new Engine(fsOut, opts.maxVoices || 8);
      this.post = post;
      this.now = now;
      this.blk = 0; this.busy = 0; this.span = 0; this.cpu = 0;
      this.postEvery = Math.max(1, Math.round(fsOut * 0.016 / 128));
    }
    onMessage(m) {
      const e = this.eng;
      switch (m.t) {
        case 'G': e.setGlobals(m.G); break;
        case 'on': e.noteOn(m.id, m.midi, m.vel, m.vo); break;
        case 'off': e.noteOff(m.id); break;
        case 'vo': e.voiceOverride(m.id, m.vo); break;
        case 'focus': if (e.findVoice(m.id)) e.focusId = m.id; break;
        case 'period': e.focusPeriodSec = m.sec; break;
        case 'mute': e.mute = !!m.on; break;
        case 'sustain': e.setSustain(!!m.on); break;
        case 'alloff': e.allOff(!!m.hard); break;
        case 'cap': e.capInterval = m.sec; break;
      }
    }
    process(L, R, n) {
      const t0 = this.now();
      this.eng.process(L, R, n);
      this.busy += this.now() - t0;
      this.span += n / this.eng.fsOut * 1000;
      if (++this.blk >= this.postEvery) {
        this.blk = 0;
        if (this.span >= 500) { this.cpu = this.busy / this.span; this.busy = 0; this.span = 0; }
        const tel = this.eng.telemetry();
        const sig = this.eng.takeSignal();
        const caps = this.eng.pending.splice(0);
        const tr = [sig.buffer];
        for (const c of caps) tr.push(c.p.buffer, c.u.buffer);
        this.post({ t: 'tel', tel, sig, caps, cpu: this.cpu, nActive: this.eng.nActive || 0, maxVoices: this.eng.maxVoices }, tr);
      }
    }
  }

  /* --------------------------------------------- display geometry ---- */
  /* the pipe as the screen should draw it for (midi, G): derived lengths included */
  function displayGeometry(midi, G, fs) {
    const p = voiceParams(midi, G);
    const dv = derive(p, fs || 96000, newDerived());
    const pd = designPrinzipal(midi);
    const morph = p[P_MORPH];
    // drawn body length: the pipe as built in air for its key (length glide moves the key), with the
    // end corrections of its current scale. Cut-up, stopping, gas, temperature and pitch lock change
    // the acoustics (dv.Leff) but do not saw the pipe, so they do not change the drawing.
    const d = p[P_D];
    const L = Math.max(0.004, pd.c / (2 * p[P_FTARGET]) - 0.3 * d * d / pd.cutup - 0.3 * d);
    return {
      midi, name: organName(midi), f0: pd.f0, ftarget: p[P_FTARGET],
      d: p[P_D], H: p[P_MOUTHW], W: dv.W, h: p[P_FLUE], toe: p[P_TOE], footLen: pd.foot_len,
      L, Lphys: p[P_LPHYS], Leff: dv.Leff, M: dv.M, fres: dv.fres, morph, y0: p[P_Y0],
      c: p[P_C], rho: p[P_RHO], pchest: p[P_PCHEST], kappa: p[P_KAPPA], ampcap: p[P_AMPCAP],
      Wdesign: pd.cutup, ising: pd.ising, pfDesign: pd.p_foot, lDesign: pd.l_phys, dDesign: pd.d,
    };
  }

  return {
    PNAMES, NPAR, PI, MMWS, CTRL, K_SNAP, NX_SNAP, GASES, DEFAULT_G,
    gasProps, gasMix, midiToHz, organName, designPrinzipal, setKcal, kcalFor, baseParams, voiceParams,
    derive, newDerived, Voice, Engine, Host, Decimator, displayGeometry,
  };
}

if (typeof window !== 'undefined') window.OKL_ENGINE_FACTORY = OKL_ENGINE_FACTORY;
