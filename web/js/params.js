/* OkumuLab 1 — parameter registry and the slider rows of the side panels */
window.OKL = window.OKL || {};
(function () {
  'use strict';
  const E = OKL_ENGINE_FACTORY();
  E.setKcal(window.OKL_STEP1.kcal);
  OKL.E = E;

  const sgn = (v, d) => (v > 0 ? '+' : (v < 0 ? '−' : '±')) + Math.abs(v).toFixed(d);
  const pct = (v) => (v * 100).toFixed(0);

  /* steady foot pressure and Ising number of the focus pipe for a cut-up factor */
  function isingOf(ctx, cutupFactor) {
    const g = ctx.geom;
    if (!g) return NaN;
    const Sflue = g.h * g.H, Stoe = Math.PI * g.toe * g.toe / 4;
    const pf = g.pchest / (1 + Math.pow(Sflue / Stoe, 2));
    const W = g.W * (cutupFactor / Math.max(1e-6, ctx.G.cutup));
    return Math.sqrt(2 * pf * g.h / (g.rho * W * W * W)) / g.ftarget;
  }
  function cutupForIsing(ctx, I) {
    const g = ctx.geom;
    const Sflue = g.h * g.H, Stoe = Math.PI * g.toe * g.toe / 4;
    const pf = g.pchest / (1 + Math.pow(Sflue / Stoe, 2));
    const W = Math.pow(2 * pf * g.h / (g.rho * I * I * g.ftarget * g.ftarget), 1 / 3);
    return W / (g.W / Math.max(1e-6, ctx.G.cutup));
  }
  OKL.isingOf = isingOf;

  /* MALLET (plugin): the material tables come from the plugin (OKL.audio.metals / heads); names as fallback */
  const METAL = ['Common 30%', 'Spotted 50%', 'Tin 75%', 'Zinc', 'Copper'];
  const HEAD = ['Hard rubber', 'Acrylic', 'Boxwood', 'Brass', 'Yarn'];
  const WOOD = ['White oak', 'Red pine', 'Sitka spruce', 'W. red cedar'];      // V2 (Mallet.cpp kWoods)
  // v2.0: the temperaments (Tuning.cpp kTemperamentNames), what each one is
  const TEMPER = ['Equal', 'Pythagorean', 'Meantone 1/4', 'Werckmeister III', 'Kirnberger III', 'Vallotti', 'Young', 'Custom (.scl)'];
  const TEMPER_SUB = ['12 equal semitones', 'pure fifths, wolf G#–E♭', 'Aron 1523: pure thirds', 'Werckmeister 1691', 'Kirnberger 1779',
    'Vallotti: six fifths 1/6 P.c.', 'Young 1800: six fifths 1/6 P.c.'];
  function temperSub(v) {
    const i = Math.round(v);
    if (i < 7) return TEMPER_SUB[i] || '';
    const t = OKL.tuning && OKL.audio && OKL.audio.tuning;
    return t && t.scl ? t.scl : 'no .scl loaded';
  }
  const wallOf = (d) => 0.25e-3 + 0.009 * d;                      // default wall thickness (Mallet.cpp)
  const woodWallOf = (w) => w * 10.5 / 61.8;                       // V2: a wooden pipe's boards (Mallet.cpp woodWallThickness)
  const gedackt = (c) => Math.round((c && c.G && c.G.stop) || 0) === 1;
  function metalSub(v) {
    const m = OKL.audio && OKL.audio.metals && OKL.audio.metals[Math.round(v)];
    return m ? 'E ' + (m.E / 1e9).toFixed(0) + ' GPa · ρ ' + (m.rho / 1000).toFixed(2) + ' · η ' + m.eta.toExponential(0) : '';
  }
  function woodSub(v) {
    const w = OKL.audio && OKL.audio.woods && OKL.audio.woods[Math.round(v)];
    return w ? 'E ' + (w.EL / 1e9).toFixed(1) + '/' + (w.ET / 1e9).toFixed(2) + ' GPa · ρ ' + (w.rho / 1000).toFixed(2) : 'along / across the grain';
  }
  function headMass(head, dmm) {
    const hd = OKL.audio && OKL.audio.heads && OKL.audio.heads[Math.round(head)];
    if (!hd) return '';
    const r = dmm / 2000;
    return (hd.rho * 4 / 3 * Math.PI * r * r * r * 1000 + 3).toFixed(1) + ' g';
  }
  function headSub(v) {
    const hd = OKL.audio && OKL.audio.heads && OKL.audio.heads[Math.round(v)];
    if (hd && hd.K > 0) return 'wool · F = K x^' + hd.p.toFixed(2) + ' · K ' + (hd.K / 1e6).toFixed(0) + ' MN/m^' + hd.p.toFixed(2);   // v2.0: yarn
    return hd ? 'E ' + (hd.E >= 1e9 ? (hd.E / 1e9).toFixed(1) + ' GPa' : (hd.E / 1e6).toFixed(0) + ' MPa') + ' · e ' + hd.e.toFixed(2) : '';
  }

  /* P5 (plugin): the reverb is a computed church nave; its mid-frequency reverberation time */
  function roomSub() {
    const r = OKL.audio && OKL.audio.room;
    return r && r.t60 ? 'nave T60 ' + (0.5 * (r.t60[3] + r.t60[4])).toFixed(1) + ' s' : 'T60 3.2 s';
  }

  /* P5 (plugin): the side hole's place on the body, as derive() puts it (above the upper lip .. one radius below the top) */
  function holeSub(v, c) {
    const g = c.geom;
    if (!g || v < 0.02) return 'tone hole';
    const x0 = g.W + 1.2 * g.H, x1 = Math.max(x0, g.Lphys - 0.6 * g.d);
    return ((x0 + (x1 - x0) * v) * 1e3).toFixed(0) + ' mm up';
  }
  const FM_TGT = ['wind', 'length', 'upper lip'];
  const WIND_REF_PA = 500;          // Wind pressure 1.00 (src/dsp/Params.h kWindRefPa)
  const KNEE = 2 / 3;               // its knob: 1.00 at 2/3 of the travel (kWindKnee)

  function gasLabel(v) {
    if (v > 0.005) return 'He ' + pct(v) + '%';
    if (v < -0.005) return 'CO₂ ' + pct(-v) + '%';
    return 'AIR';
  }

  /*
   * key: DEFAULT_G key, g: panel group, cc: default MIDI CC (ctl PB = pitch bend),
   * slot: position in the MIDI CONTROLLER panel (the eight controller knobs),
   * band: real-instrument range (drawn as a green band), vis: screen only
   */
  const DEFS = [
    // ---- WIND
    // knob 5 is the former Bellows: x the windchest's reference pressure (1.00 = 500 Pa, G.wind); 0..1 over 2/3 of
    // the travel at the old 0..1.5 Bellows speed, 1..5 geometric over the last third (curve 'knee', Params.cpp)
    { key: 'bellows', g: 'midi', slot: 5, label: 'Wind pressure', en: 'p chest', min: 0, max: 5, curve: 'knee', cc: 93,
      fmt: (v) => v.toFixed(2), unit: '', sub: (v) => (v * WIND_REF_PA).toFixed(0) + ' Pa',
      band: () => [69 * 9.80665 / WIND_REF_PA, 97 * 9.80665 / WIND_REF_PA], bandNote: 'Baroque organ examples, 69–97 mmWS (680–950 Pa)' },
    { key: 'trem', g: 'midi', slot: 8, label: 'Tremulant', en: 'DEPTH', min: 0, max: 1, cc: 16,
      fmt: (v) => pct(v), unit: '%', sub: (v) => '±' + (15 * v).toFixed(1) + '% wind' },
    { key: 'tremRate', g: 'wind', label: 'Trem rate', en: 'RATE', min: 2, max: 12, fmt: (v) => v.toFixed(1), unit: 'Hz' },
    { key: 'velSens', g: 'wind', label: 'Pallet speed', en: 'VELOCITY', min: 0, max: 1, fmt: (v) => pct(v), unit: '%', sub: () => 'vel → opening' },

    // ---- VOICING
    { key: 'cutup', g: 'midi', slot: 1, label: 'Cut-up', en: 'W', min: 0.4, max: 2.5, curve: 'log', cc: 74,
      fmt: (v, c) => (c.geom ? (c.geom.W * 1e3).toFixed(2) : '—'), unit: 'mm',
      sub: (v, c) => 'I ' + isingOf(c, v).toFixed(2) + '  ×' + v.toFixed(2),
      band: (c) => (c.geom ? [cutupForIsing(c, 3), cutupForIsing(c, 2)] : null), bandNote: 'intonation number I = 2–3' },
    { key: 'y0b', g: 'midi', slot: 2, label: 'Labium offset', en: 'y₀', min: -2.5, max: 2.5, cc: 71, bipolar: true,
      // (v1.1: b is the jet's half-width where it meets the labium, after spreading across the mouth; nicks do not move the lip)
      fmt: (v) => sgn(v, 2), unit: 'b', sub: (v, c) => (c.geom ? sgn(v * (c.geom.y0b1 || 0.4 * c.geom.h) * 1e3, 3) + ' mm' : '') },
    { key: 'mouthFrac', g: 'voice', label: 'Mouth width', en: 'H', min: 0.1, max: 0.31,
      fmt: (v, c) => (c.geom ? (c.geom.H * 1e3).toFixed(1) : '—'), unit: 'mm',
      sub: (v, c) => (gedackt(c) ? '= inside width · 1/' + (1 / v).toFixed(2) : '1/' + (1 / v).toFixed(2) + ' circ.'),
      band: () => [1 / 7, 2 / 7], bandNote: '1/7–2/7 of the circumference' },
    { key: 'noise', g: 'voice', label: 'Turbulence', en: 'NOISE', min: 0, max: 0.06,
      fmt: (v) => (v * 100).toFixed(1), unit: '%h' },
    { key: 'toe', g: 'voice', label: 'Toe hole', en: 'TOE Ø', min: 0.4, max: 1.8, curve: 'log',
      fmt: (v, c) => (c.geom ? (c.geom.toe * 1e3).toFixed(1) : '—'), unit: 'mm', sub: (v) => '×' + v.toFixed(2) },
    { key: 'nicking', g: 'voice', label: 'Nicking', en: 'NICKS', min: 0, max: 1, def: 0, plug: true,
      fmt: (v) => pct(v), unit: '%', sub: (v) => (v < 0.02 ? 'unnicked' : 'thicker, calmer jet') },
    // V2: the Gedackt's mouth parts (they work on the Prinzipal too)
    { key: 'ears', g: 'voice', label: 'Ears', en: 'EARS', min: 0, max: 1, def: 0, plug: true,
      fmt: (v) => pct(v), unit: '%', sub: (v, c) => (v < 0.01 ? 'none' : (c.geom && c.geom.dMears ? '+' + (c.geom.dMears * 1e3).toFixed(1) + ' mm end corr.' : 'side ears')) },
    { key: 'arch', g: 'voice', label: 'Arched cut-up', en: 'ARCH', min: 0, max: 1, def: 0, plug: true,
      fmt: (v) => pct(v), unit: '%', sub: (v, c) => (v < 0.01 ? 'straight lip' : (c.geom && c.geom.archRise > 0 ? 'rise ' + (c.geom.archRise * 1e3).toFixed(1) + ' mm' : 'no room')) },

    // ---- GEOMETRY
    { key: 'scaleHT', g: 'midi', slot: 3, label: 'Scale', en: 'HT', min: -20, max: 16, cc: 76,
      fmt: (v) => sgn(v, 1), unit: 'HT',
      sub: (v, c) => (!c.geom ? '' : (gedackt(c) ? (c.geom.H * 1e3).toFixed(1) + '×' + (c.geom.depth * 1e3).toFixed(1) + ' mm' : 'Ø ' + (c.geom.d * 1e3).toFixed(1) + ' mm')) },
    { key: 'morph', g: 'midi', slot: 4, label: 'Open↔Stopped', en: 'TOP', min: 0, max: 1, cc: 77,
      fmt: (v) => pct(v), unit: '%',
      sub: (v, c) => (gedackt(c) ? (v < 0.02 ? 'stopper out' : (v > 0.98 ? 'stopped' : 'stopper drawn')) : (v < 0.02 ? 'open' : (v > 0.98 ? 'stopped' : 'impossible'))),
      band: (c) => (gedackt(c) ? [1, 1] : [0, 0]), bandNote: 'real pipes are 0% or 100%' },
    { key: 'glide', g: 'shape', label: 'Length glide', en: 'GLIDE', min: -12, max: 12, ctl: 'PB', bipolar: true,
      fmt: (v) => sgn(v, 2), unit: 'st', sub: (v, c) => (c.geom ? 'L ' + (c.geom.L * 1e3).toFixed(0) + ' mm' : '') },
    { key: 'sideHole', g: 'shape', label: 'Side hole', en: 'POS', min: 0, max: 1, vis: true,
      fmt: (v) => (v < 0.02 ? 'OFF' : pct(v)), unit: (v) => (v < 0.02 ? '' : '%'), sub: (v, c) => holeSub(v, c) },
    { key: 'pitchLock', g: 'shape', label: 'Pitch lock', en: 'LOCK', min: 0, max: 1, cc: 85,
      fmt: (v) => pct(v), unit: '%', sub: (v) => (v > 0.99 ? 'on the key' : (v < 0.01 ? 'pure physics' : 'partial')) },
    // v2.0 (plugin): the tuning (src/dsp/Tuning.h), in the TUNING panel (the top bar's TUNING button), which also loads Scala files
    { key: 'temperament', g: 'tune', label: 'Temperament', en: 'TEMPER', min: 0, max: 7, step: 1, def: 0, plug: true,
      fmt: (v) => TEMPER[Math.round(v)] || '?', unit: '', sub: (v) => temperSub(v) },
    { key: 'pitchA', g: 'tune', label: 'Pitch a¹', en: 'a¹', min: 380, max: 480, def: 440, plug: true,
      fmt: (v) => v.toFixed(1), unit: 'Hz', sub: (v) => { const c = 1200 * Math.log2(v / 440); return Math.abs(c) < 0.05 ? 'a¹ = 440' : sgn(c, 1) + ' cents re 440'; } },

    // ---- ATMOSPHERE
    { key: 'gas', g: 'midi', slot: 6, label: 'Gas', en: 'MIX', min: -1, max: 1, cc: 18, bipolar: true,
      fmt: (v) => gasLabel(v), unit: '', sub: (v, c) => (c.geom ? 'c ' + c.geom.c.toFixed(0) + ' m/s' : ''), band: () => [0, 0], bandNote: 'air' },
    { key: 'tempC', g: 'midi', slot: 7, label: 'Temperature', en: 'TEMP', min: -30, max: 80, cc: 19,
      fmt: (v) => v.toFixed(0), unit: '°C' },
    { key: 'cMult', g: 'atmos', label: 'Sound speed', en: 'c ONLY', min: 0.5, max: 2, curve: 'log', bipolar: true,
      fmt: (v) => '×' + v.toFixed(2), unit: '', sub: (v, c) => (c.geom ? c.geom.c.toFixed(0) + ' m/s' : ''), band: () => [1, 1], bandNote: 'physical' },
    { key: 'rhoMult', g: 'atmos', label: 'Density', en: 'ρ ONLY', min: 0.2, max: 5, curve: 'log', bipolar: true,
      fmt: (v) => '×' + v.toFixed(2), unit: '', sub: (v, c) => (c.geom ? c.geom.rho.toFixed(3) + ' kg/m³' : ''), band: () => [1, 1], bandNote: 'physical' },

    // ---- LAB
    { key: 'fmDepth', g: 'lab', label: 'FM depth', en: 'DEPTH', min: 0, max: 0.8, cc: 1,
      fmt: (v) => pct(v), unit: '%', sub: (v, c) => FM_TGT[Math.round(c.G.fmTarget || 0)] + ' · audio rate' },
    { key: 'fmRatio', g: 'lab', label: 'FM ratio', en: 'FM RATIO', min: 0.1, max: 4, curve: 'log',
      fmt: (v) => '×' + v.toFixed(2), unit: 'f', sub: (v, c) => (c.geom ? (v * c.geom.ftarget).toFixed(1) + ' Hz' : '') },
    { key: 'fmTarget', g: 'lab', label: 'FM target', en: 'TARGET', min: 0, max: 2, step: 1, def: 0, plug: true,
      fmt: (v) => FM_TGT[Math.round(v)].toUpperCase(), unit: '', sub: (v) => ['p chest', 'bore delay', 'labium offset y₀'][Math.round(v)] },
    { key: 'jetGain', g: 'lab', label: 'Jet feedback', en: 'COUPLING', min: 0, max: 3,
      fmt: (v) => '×' + v.toFixed(2), unit: '', sub: () => 'pipe → jet', band: () => [1, 1], bandNote: 'physical' },
    { key: 'crossDrive', g: 'lab', label: 'Cross drive', en: 'CROSS', min: 0, max: 1, def: 0, plug: true,
      fmt: (v) => pct(v), unit: '%', sub: () => 'other pipes → jet' },
    { key: 'vFollow', g: 'lab', label: 'Cut-up follow', en: 'FOLLOW', min: 0, max: 1,
      fmt: (v) => pct(v), unit: '%', sub: () => 'W tracks f' },
    { key: 'kappa', g: 'lab', label: 'Wave speed', en: 'κ', min: 0.6, max: 2.6, curve: 'log',
      fmt: (v) => v.toFixed(2), unit: 'κ', sub: () => 'convection' },
    { key: 'edge', g: 'lab', label: 'Edge tone', en: 'EDGE', min: 0, max: 0.4,
      fmt: (v) => v.toFixed(3), unit: '', sub: () => 'labium→flue' },
    { key: 'kick', g: 'lab', label: 'Start pulse', en: 'KICK', min: -1.5, max: 0.5, bipolar: true,
      fmt: (v) => sgn(v, 2), unit: '×pf', sub: () => 'start vortex' },
    { key: 'loss', g: 'lab', label: 'Wall loss', en: 'LOSS', min: 0.3, max: 10, curve: 'log',
      // (V2: against the stop's own default: 2.5 x the smooth-wall theory for the metal, 1.27 x for the wood)
      fmt: (v) => '×' + (v / (BY_KEY.loss.def || 2.5)).toFixed(2), unit: '', sub: (v, c) => (gedackt(c) ? 'wood, ' + v.toFixed(2) + '× theory' : 'viscous') },
    { key: 'reverb', g: 'lab', label: 'Reverb', en: 'REVERB', min: 0, max: 1, cc: 17,
      fmt: (v) => pct(v), unit: '%', sub: () => roomSub() },

    // ---- MALLET (plugin): the pipe body struck with a hard mallet
    { key: 'excite', g: null, label: 'Excitation', en: 'EXCITE', min: 0, max: 1, step: 1, def: 0, noRow: true,
      fmt: (v) => (v >= 0.5 ? 'MALLET' : 'WIND'), unit: '' },
    // V2: the stop (the top bar's menu, where the experiment recipes were)
    { key: 'stop', g: null, label: 'Stop', en: 'STOP', min: 0, max: 1, step: 1, def: 0, noRow: true,
      fmt: (v) => (Math.round(v) === 1 ? "Gedackt 8'" : "Prinzipal 8'"), unit: '' },
    { key: 'metal', g: 'mallet', label: 'Pipe metal', en: 'METAL', min: 0, max: 4, step: 1, def: 1, stop: 0,
      fmt: (v) => METAL[Math.round(v)] || '?', unit: '', sub: (v) => metalSub(v) },
    { key: 'wood', g: 'mallet', label: 'Pipe wood', en: 'WOOD', min: 0, max: 3, step: 1, def: 0, stop: 1, plug: true,
      fmt: (v) => WOOD[Math.round(v)] || '?', unit: '', sub: (v) => woodSub(v) },
    { key: 'wallMult', g: 'mallet', label: 'Wall', en: 'THICK', min: 0.5, max: 2, curve: 'log', def: 1,
      fmt: (v, c) => (c.geom ? ((gedackt(c) ? woodWallOf(c.geom.H) : wallOf(c.geom.d)) * v * 1e3).toFixed(2) : '—'), unit: 'mm',
      sub: (v, c) => '×' + v.toFixed(2) + (gedackt(c) ? ' boards' : ' builder') },
    { key: 'head', g: 'mallet', label: 'Mallet head', en: 'HEAD', min: 0, max: 4, step: 1, def: 1,
      fmt: (v) => HEAD[Math.round(v)] || '?', unit: '', sub: (v) => headSub(v) },
    { key: 'headD', g: 'mallet', label: 'Head size', en: 'HEAD Ø', min: 10, max: 50, def: 25,
      fmt: (v) => v.toFixed(0), unit: 'mm', sub: (v, c) => headMass(c.G.head, v) },
    { key: 'strikePos', g: 'mallet', label: 'Strike point', en: 'STRIKE', min: 0.05, max: 0.95, def: 0.3,
      fmt: (v) => pct(v), unit: '%', sub: (v, c) => (c.geom ? (v * c.geom.Lphys * 1e3).toFixed(0) + ' mm up' : '') },
    { key: 'damper', g: 'mallet', label: 'Damper', en: 'DAMPER', min: 0, max: 1, def: 0.6,
      fmt: (v) => pct(v), unit: '%', sub: () => 'felt, key up' },

    // ---- P5 (plugin): the modulation matrix (MATRIX panel); sources and targets are menus there
    ...[1, 2, 3, 4].flatMap((k) => [
      { key: 'modSrc' + k, g: null, noRow: true, label: 'Mod ' + k + ' source', en: 'SRC ' + k, min: 0, max: 11, step: 1, def: 0, fmt: (v) => String(v), unit: '' },
      { key: 'modDst' + k, g: null, noRow: true, label: 'Mod ' + k + ' target', en: 'DST ' + k, min: 0, max: 27, step: 1, def: 0, fmt: (v) => String(v), unit: '' },
      { key: 'modAmt' + k, g: 'mod' + k, label: 'Amount', en: 'AMT ' + k, min: -1, max: 1, def: 0, bipolar: true, plug: true,
        fmt: (v) => sgn(v * 100, 0), unit: '%', sub: () => 'of the travel' },
    ]),
    { key: 'lfo1Rate', g: 'modx', label: 'LFO 1 rate', en: 'LFO 1', min: 0.02, max: 20, curve: 'log', def: 0.5, plug: true,
      fmt: (v) => v.toFixed(v < 1 ? 2 : 1), unit: 'Hz', sub: (v) => (1 / v).toFixed(v > 1 ? 2 : 1) + ' s period' },
    { key: 'lfo2Rate', g: 'modx', label: 'LFO 2 rate', en: 'LFO 2', min: 0.02, max: 20, curve: 'log', def: 3, plug: true,
      fmt: (v) => v.toFixed(v < 1 ? 2 : 1), unit: 'Hz', sub: (v) => (1 / v).toFixed(v > 1 ? 2 : 1) + ' s period' },
    { key: 'envAttack', g: 'modx', label: 'Env attack', en: 'ATTACK', min: 0.001, max: 5, curve: 'log', def: 0.05, plug: true,
      fmt: (v) => (v < 1 ? (v * 1000).toFixed(0) : v.toFixed(2)), unit: (v) => (v < 1 ? 'ms' : 's'), sub: () => 'from the key' },
    { key: 'envDecay', g: 'modx', label: 'Env decay', en: 'DECAY', min: 0.01, max: 10, curve: 'log', def: 1, plug: true,
      fmt: (v) => (v < 1 ? (v * 1000).toFixed(0) : v.toFixed(2)), unit: (v) => (v < 1 ? 'ms' : 's'), sub: () => 'time constant' },
  ];
  const BY_KEY = {};
  for (const d of DEFS) { if (d.def === undefined) d.def = E.DEFAULT_G[d.key]; BY_KEY[d.key] = d; }
  /* every parameter's default (the Phase 2 engine's DEFAULT_G plus the plugin-only ones) */
  const DEFAULTS = Object.assign({}, E.DEFAULT_G);
  for (const d of DEFS) if (DEFAULTS[d.key] === undefined) DEFAULTS[d.key] = d.def;
  DEFAULTS.wind = WIND_REF_PA / E.MMWS;          // the windchest's reference pressure (no knob; the plugin's Globals::wind)

  function toNorm(d, v) {
    if (d.curve === 'log') return Math.log(v / d.min) / Math.log(d.max / d.min);
    if (d.curve === 'knee') return v <= 1 ? v * KNEE : KNEE + (1 - KNEE) * Math.log(v) / Math.log(d.max);
    return (v - d.min) / (d.max - d.min);
  }
  function fromNorm(d, n) {
    n = Math.min(1, Math.max(0, n));
    if (d.curve === 'log') return d.min * Math.pow(d.max / d.min, n);
    if (d.curve === 'knee') return n <= KNEE ? n / KNEE : Math.pow(d.max, (n - KNEE) / (1 - KNEE));
    return d.min + n * (d.max - d.min);
  }
  const clampV = (d, v) => Math.min(d.max, Math.max(d.min, v));

  /* v1.1.1: the RESET button at the right of every row (an anticlockwise arrow) */
  const RESET_ICON = '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M3.4 8.4A4.6 4.6 0 1 0 5 4.6" />' +
    '<path d="M4.1 1.9 4.7 5.3 8.1 4.7" /></svg>';
  const atDefault = (d, v) => Math.abs(toNorm(d, v) - toNorm(d, d.def)) < 1e-6;

  const UI = {
    defs: DEFS, byKey: BY_KEY, defaults: DEFAULTS, toNorm, fromNorm, rows: {},
    readOnly: false,
    onChange: null,            // (key, value, source)
    onLearn: null,             // plugin: (key) MIDI learn
    onReset: null,             // (key) a row's RESET button

    build(ctx) {
      this.ctx = ctx;
      // MIDI CONTROLLER rows in knob order, every other group in definition order
      const ordered = DEFS.slice().sort((a, b) => (a.slot || 0) - (b.slot || 0));
      for (const d of ordered) {
        if (d.noRow || !d.g) continue;
        const host = document.querySelector('#grp-' + d.g + ' .prms');
        const row = document.createElement('div');
        row.className = 'prm' + (d.vis ? ' vis' : '') + (d.step ? ' choice' : '') + (d.plug ? ' plug' : '');
        row.dataset.key = d.key;
        const ctl = d.cc !== undefined ? `<b>CC${d.cc}</b> · ` : (d.ctl ? `<b>${d.ctl}</b> · ` : '');
        row.innerHTML =
          `<div class="prm-l"><span class="nm">${d.label}</span><span class="cc">${ctl}${d.en}</span></div>` +
          `<div class="prm-t" title="${d.bandNote ? 'Green band: ' + d.bandNote : ''}"><div class="rail"></div><div class="ticks"></div><div class="band" hidden></div>` +
          `<div class="fill"></div><div class="def"></div><div class="mod"></div><div class="thumb"></div></div>` +
          `<div class="prm-v"></div>` +
          `<button type="button" class="prm-r" title="RESET: ${d.label} back to its default" aria-label="Reset ${d.label}">${RESET_ICON}</button>`;
        host.appendChild(row);
        const r = {
          row, d, track: row.querySelector('.prm-t'), fill: row.querySelector('.fill'), thumb: row.querySelector('.thumb'),
          band: row.querySelector('.band'), defm: row.querySelector('.def'), mod: row.querySelector('.mod'), val: row.querySelector('.prm-v'),
          rst: row.querySelector('.prm-r'),
        };
        r.defm.style.left = (toNorm(d, d.def) * 100) + '%';
        this.rows[d.key] = r;
        this._bind(r);
      }
      this.refresh();
    },

    _bind(r) {
      const d = r.d, tr = r.track;
      let drag = false, startX = 0, startN = 0;
      const setFromEvent = (e, fine) => {
        const rect = tr.getBoundingClientRect();
        let n;
        if (fine) n = startN + (e.clientX - startX) / rect.width * 0.15;
        else n = (e.clientX - rect.left) / rect.width;
        this.set(d.key, fromNorm(d, n), 'ui');
      };
      tr.addEventListener('pointerdown', (e) => {
        if (this.readOnly) return;
        drag = true; tr.setPointerCapture(e.pointerId);
        startX = e.clientX; startN = toNorm(d, this.ctx.G[d.key]);
        if (!e.shiftKey) setFromEvent(e, false);
      });
      tr.addEventListener('pointermove', (e) => { if (drag) setFromEvent(e, e.shiftKey); });
      const end = () => { drag = false; };
      tr.addEventListener('pointerup', end);
      tr.addEventListener('pointercancel', end);
      tr.addEventListener('dblclick', () => { if (!this.readOnly) this.set(d.key, d.def, 'ui'); });
      r.rst.addEventListener('click', () => { if (!this.readOnly) this.reset(d.key); });
      // plugin: right-click = MIDI learn (the plugin assigns the next controller that moves)
      r.row.addEventListener('contextmenu', (e) => { if (this.onLearn) { e.preventDefault(); this.onLearn(d.key); } });
      r.row.addEventListener('wheel', (e) => {
        if (this.readOnly) return;
        e.preventDefault();
        const step = (e.shiftKey ? 0.002 : 0.01) * (e.deltaY > 0 ? -1 : 1);
        this.set(d.key, fromNorm(d, toNorm(d, this.ctx.G[d.key]) + step), 'ui');
      }, { passive: false });
    },

    set(key, v, source) {
      const d = BY_KEY[key];
      if (!d) return;
      v = clampV(d, v);
      if (d.step) v = Math.round(v / d.step) * d.step;          // choices (metal, head, excitation)
      this.ctx.G[key] = v;
      if (source && source !== 'ui') this.flash(key);
      if (this.onChange) this.onChange(key, v, source || 'ui');
    },

    setNorm(key, n, source) { const d = BY_KEY[key]; if (d) this.set(key, fromNorm(d, n), source); },

    /* v1.1.1: one parameter back to its default (the row's RESET button) */
    reset(key) {
      const d = BY_KEY[key];
      if (!d) return;
      if (this.onReset) this.onReset(key);
      else this.set(key, d.def, 'reset');
      this.refresh();
    },

    /* the plugin's own defaults (its 'init' message): the RESET buttons and the default marks use them */
    setDefaults(map) {
      for (const key in map) {
        const d = BY_KEY[key], v = +map[key];
        if (!d || !isFinite(v)) continue;
        d.def = v; DEFAULTS[key] = v;
        const r = this.rows[key];
        if (r) r.defm.style.left = (toNorm(d, v) * 100) + '%';
      }
    },

    /* controller tag of a row ('CC74', 'CC93/82', 'PB'; empty = none), from the plugin's learnable map */
    setTag(key, ctl) {
      const r = this.rows[key];
      if (!r) return;
      const html = (ctl ? '<b>' + ctl + '</b> · ' : '') + r.d.en;
      const el = r.row.querySelector('.cc');
      if (el && r.tagHtml !== html) { el.innerHTML = html; r.tagHtml = html; }
    },

    flash(key) {
      const r = this.rows[key];
      if (!r) return;
      r.row.classList.add('hot');
      clearTimeout(r.hotT);
      r.hotT = setTimeout(() => r.row.classList.remove('hot'), 700);
    },

    /* modulation preview (pads): effective value marker */
    setMod(key, vEff) {
      const r = this.rows[key];
      if (!r) return;
      if (vEff === null || vEff === undefined) { r.mod.style.display = 'none'; return; }
      r.mod.style.display = 'block';
      r.mod.style.left = 'calc(' + (Math.min(1, Math.max(0, toNorm(r.d, vEff))) * 100) + '% - 1px)';
    },

    refresh() {
      const ctx = this.ctx;
      for (const key in this.rows) {
        const r = this.rows[key], d = r.d, v = ctx.G[key];
        const n = Math.min(1, Math.max(0, toNorm(d, v)));
        r.thumb.style.left = (n * 100) + '%';
        if (d.bipolar) {
          const n0 = Math.min(1, Math.max(0, toNorm(d, d.def)));
          r.fill.style.left = (Math.min(n, n0) * 100) + '%';
          r.fill.style.width = (Math.abs(n - n0) * 100) + '%';
        } else {
          r.fill.style.left = '0%';
          r.fill.style.width = (n * 100) + '%';
        }
        if (d.band) {
          const b = d.band(ctx);
          if (b && isFinite(b[0]) && isFinite(b[1])) {
            const a = Math.min(1, Math.max(0, toNorm(d, Math.max(d.min, b[0])))), z = Math.min(1, Math.max(0, toNorm(d, Math.min(d.max, b[1]))));
            r.band.hidden = false;
            r.band.classList.toggle('pt', z - a < 0.004);
            r.band.style.left = (a * 100) + '%';
            r.band.style.width = (Math.max(0, z - a) * 100) + '%';
          } else r.band.hidden = true;
        }
        const unit = typeof d.unit === 'function' ? d.unit(v, ctx) : (d.unit || '');
        const sub = d.sub ? d.sub(v, ctx) : '';
        const html = d.fmt(v, ctx) + (unit ? `<span class="u">${unit}</span>` : '') + (sub ? `<span class="sub">${sub}</span>` : '');
        if (r.html !== html) { r.val.innerHTML = html; r.html = html; }
        const atDef = atDefault(d, v);                          // the RESET button lights up when the value is off its default
        if (r.atDef !== atDef) { r.row.classList.toggle('atdef', atDef); r.atDef = atDef; }
      }
    },

    /* MALLET: rows the struck pipe does not feel are dimmed (fn(key) = does it act?) */
    setActive(fn) {
      for (const key in this.rows) this.rows[key].row.classList.toggle('off', !fn(key));
    },

    /* V2: rows that belong to one stop only (the Prinzipal's metal, the Gedackt's wood) */
    setStop(stop) {
      for (const key in this.rows) {
        const r = this.rows[key];
        if (r.d.stop !== undefined) r.row.classList.toggle('stophid', r.d.stop !== stop);
      }
    },

    setReadOnly(on) {
      this.readOnly = on;
      document.querySelectorAll('.prm-t').forEach((t) => { t.style.cursor = on ? 'default' : ''; });
      document.querySelectorAll('.prm-r').forEach((b) => { b.disabled = on; });
    },
  };
  OKL.params = UI;
})();
