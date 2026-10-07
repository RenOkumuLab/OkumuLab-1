/* OkumuLab 1 — instruments: jet section scope, gauges, spectrogram, harmonics, annunciators */
window.OKL = window.OKL || {};
OKL.inst = (function () {
  'use strict';
  const C = {
    cyan: '#46e3ff', cyanA: 'rgba(70,227,255,', amber: '#ffb547', red: '#ff4d5e', green: '#6dffa0',
    fg: '#d6f4ff', fg2: '#93b8c7', dim: '#58737f', dim2: '#2c434d', line: '#15303c', metal: '#7d8a92', metal2: '#a9b5bc',
  };
  const MONO = '"JetBrains Mono", Consolas, monospace', DISP = '"Chakra Petch", "Noto Sans JP", sans-serif', SANS = '"Chakra Petch", "Segoe UI", sans-serif';
  let pr = 1;
  const cvs = {};

  function setup(id) {
    const cv = document.getElementById(id);
    const r = { cv, cx: cv.getContext('2d'), w: cv.clientWidth || parseFloat(getComputedStyle(cv).width), h: cv.clientHeight || parseFloat(getComputedStyle(cv).height) };
    cvs[id] = r;
    return r;
  }
  function sizeAll(pixelRatio) {
    pr = pixelRatio;
    for (const id in cvs) {
      const r = cvs[id];
      r.cv.width = Math.round(r.w * pr); r.cv.height = Math.round(r.h * pr);
      if (id === 'spec') r.fresh = true;
    }
  }
  function text(cx, s, x, y, font, color, align) {
    cx.font = font; cx.fillStyle = color; cx.textAlign = align || 'left'; cx.textBaseline = 'alphabetic';
    cx.fillText(s, x, y);
  }

  function init() {
    ['jetScope', 'inst', 'spec', 'specOver', 'harm'].forEach(setup);
    buildLut();
  }

  /* =================================================================== jet section */
  function drawJet(s) {
    const { cx, w, h } = cvs.jetScope;
    cx.setTransform(pr, 0, 0, pr, 0, 0);
    cx.clearRect(0, 0, w, h);
    const g = s.g, vm = s.vm;
    if (!g) return;
    const W = g.W, hh = g.h, y0 = g.y0, b = Math.max(1e-6, vm.b || 0.4 * hh);
    const secH = 172;
    // px per metre: the designed cut-up is 118 px, so changing the cut-up visibly moves the upper lip
    const sc = Math.min(118 / (g.Wdesign || W), 128 / W);
    const ox = 150, oy = 142;                 // flue exit
    const X = (xm) => ox + xm * sc, Y = (ym) => oy - ym * sc;
    // grid
    cx.strokeStyle = 'rgba(70,227,255,0.05)'; cx.lineWidth = 1;
    const mm = 1e-3 * sc;
    const step = mm >= 6 ? mm : (mm * 5 >= 6 ? mm * 5 : mm * 10);
    for (let x = ox % step; x < w; x += step) { cx.beginPath(); cx.moveTo(x, 0); cx.lineTo(x, secH); cx.stroke(); }
    for (let y = oy % step; y < secH; y += step) { cx.beginPath(); cx.moveTo(0, y); cx.lineTo(w, y); cx.stroke(); }
    // labels
    text(cx, '← OUTSIDE', 10, 16, '500 10px ' + SANS, C.dim);
    text(cx, 'INSIDE PIPE →', w - 10, 16, '500 10px ' + SANS, C.dim, 'right');
    const tw = Math.max(3, 0.0007 * sc);
    // lower lip (outer wall of the flueway)
    const metal = cx.createLinearGradient(0, 0, 0, secH);
    metal.addColorStop(0, '#b9c4ca'); metal.addColorStop(1, '#5d6a72');
    cx.fillStyle = metal;
    cx.fillRect(X(-hh / 2) - tw, Y(0), tw, secH - Y(0));
    // languid (inner wall) with a bevel
    cx.beginPath();
    cx.moveTo(X(hh / 2), Y(0)); cx.lineTo(w + 2, Y(0)); cx.lineTo(w + 2, secH); cx.lineTo(X(hh / 2) + 0.18 * W * sc, secH);
    cx.lineTo(X(hh / 2), Y(-0.14 * W)); cx.closePath();
    cx.fillStyle = '#6f7d85'; cx.fill();
    cx.strokeStyle = '#a9b5bc'; cx.stroke();
    text(cx, 'languid', X(hh / 2) + 0.35 * W * sc, secH - 8, '400 9.5px ' + SANS, '#e6eef2');
    text(cx, 'lower lip', X(-hh / 2) - tw - 4, secH - 8, '400 9.5px ' + SANS, C.fg2, 'right');
    // upper lip with the labium edge at (y0, W)
    cx.beginPath();
    cx.moveTo(X(y0) - tw, 0); cx.lineTo(X(y0) - tw, Y(W) - tw * 1.6); cx.lineTo(X(y0), Y(W)); cx.lineTo(X(y0) + 1, Y(W) - tw * 3); cx.lineTo(X(y0) + 1, 0); cx.closePath();
    cx.fillStyle = metal; cx.fill();
    cx.fillStyle = C.cyan; cx.beginPath(); cx.arc(X(y0), Y(W), 2.2, 0, Math.PI * 2); cx.fill();
    text(cx, 'upper lip', X(y0) - tw - 5, 30, '400 9.5px ' + SANS, C.fg2, 'right');

    // jet: centreline eta(y) with half-width b
    const on = vm.jetOn;
    const col = OKL.gasColor(s.G.gas, s.G.tempC);
    const rgb = (a) => 'rgba(' + Math.round(col[0] * 255) + ',' + Math.round(col[1] * 255) + ',' + Math.round(col[2] * 255) + ',' + a + ')';
    if (on > 0.01) {
      const N = 40, pts = [];
      for (let i = 0; i <= N; i++) { const yn = i / N; pts.push([vm.etaAt(yn), yn * W]); }
      cx.beginPath();
      pts.forEach(([e, y], i) => { const x = X(e - b), yy = Y(y); if (i) cx.lineTo(x, yy); else cx.moveTo(x, yy); });
      for (let i = pts.length - 1; i >= 0; i--) cx.lineTo(X(pts[i][0] + b), Y(pts[i][1]));
      cx.closePath();
      cx.fillStyle = rgb(0.35 * on); cx.fill();
      cx.beginPath();
      pts.forEach(([e, y], i) => { if (i) cx.lineTo(X(e), Y(y)); else cx.moveTo(X(e), Y(y)); });
      cx.strokeStyle = rgb(0.95 * on); cx.lineWidth = 1.6; cx.shadowColor = rgb(1); cx.shadowBlur = 8; cx.stroke();
      cx.shadowBlur = 0; cx.lineWidth = 1;
      // flow dots along the jet (slow motion)
      const t = performance.now() / 1000;
      for (let k = 0; k < 9; k++) {
        const yn = ((t * 0.9 * on + k / 9) % 1);
        cx.fillStyle = rgb(0.9 * on);
        cx.beginPath(); cx.arc(X(vm.etaAt(yn)), Y(yn * W), 1.6, 0, Math.PI * 2); cx.fill();
      }
      // split at the labium
      const fin = Math.min(1, Math.max(0, vm.inflow));
      const ex = X(vm.etaAt(1)), ey = Y(W);
      cx.lineCap = 'round';
      cx.strokeStyle = rgb(0.85 * on); cx.lineWidth = 1 + 7 * fin;
      cx.beginPath(); cx.moveTo(ex, ey); cx.quadraticCurveTo(ex + 0.1 * W * sc, ey - 0.25 * W * sc, ex + 0.45 * W * sc, ey - 0.42 * W * sc); cx.stroke();
      cx.strokeStyle = rgb(0.55 * on); cx.lineWidth = 1 + 7 * (1 - fin);
      cx.beginPath(); cx.moveTo(ex, ey); cx.quadraticCurveTo(ex - 0.1 * W * sc, ey - 0.25 * W * sc, ex - 0.45 * W * sc, ey - 0.42 * W * sc); cx.stroke();
      cx.lineCap = 'butt'; cx.lineWidth = 1;
      text(cx, 'in ' + (fin * 100).toFixed(0) + '%', ex + 0.5 * W * sc, ey - 0.45 * W * sc, '600 11px ' + MONO, C.fg);
      text(cx, 'out ' + ((1 - fin) * 100).toFixed(0) + '%', ex - 0.5 * W * sc, ey - 0.45 * W * sc, '600 11px ' + MONO, C.fg2, 'right');
    } else {
      text(cx, 'no wind', X(0), Y(W * 0.5), '500 11px ' + SANS, C.dim, 'center');
    }
    // dimensions
    cx.strokeStyle = C.dim; cx.fillStyle = C.dim;
    const dxp = X(-hh / 2) - tw - 30;
    cx.beginPath(); cx.moveTo(dxp, Y(0)); cx.lineTo(dxp, Y(W)); cx.stroke();
    cx.beginPath(); cx.moveTo(dxp - 4, Y(0)); cx.lineTo(dxp + 4, Y(0)); cx.moveTo(dxp - 4, Y(W)); cx.lineTo(dxp + 4, Y(W)); cx.stroke();
    text(cx, 'W ' + OKL.fmtLen(W), dxp - 6, Y(W / 2) + 4, '500 10.5px ' + MONO, C.fg2, 'right');
    text(cx, 'h ' + OKL.fmtLen(hh), X(0), Y(0) + 15, '500 10px ' + MONO, C.amber, 'center');
    // scale bar 1 mm
    const bar = 1e-3 * sc;
    cx.strokeStyle = C.fg2; cx.beginPath(); cx.moveTo(w - 14 - bar, secH - 10); cx.lineTo(w - 14, secH - 10); cx.stroke();
    text(cx, '1 mm', w - 14 - bar / 2, secH - 14, '400 9px ' + MONO, C.fg2, 'center');
    // readouts
    const ro = [
      ['U_j', vm.Uj > 0.01 ? vm.Uj.toFixed(1) + ' m/s' : '—'],
      ['u_c', vm.uc > 0 ? vm.uc.toFixed(1) + ' m/s' : '—'],
      ['τ', vm.tauP > 0 ? vm.tauP.toFixed(2) + ' T' : '—'],
      ['b', OKL.fmtLen(b)],
    ];
    ro.forEach((r, i) => {
      text(cx, r[0], w - 92, 34 + i * 14, '500 10px ' + MONO, C.dim);
      text(cx, r[1], w - 10, 34 + i * 14, '500 10.5px ' + MONO, C.fg, 'right');
    });

    // ---- one period: eta/b at the labium and the inflow fraction
    const y1 = secH + 6, hh2 = h - y1 - 6;
    cx.fillStyle = 'rgba(0,0,0,0.25)'; cx.fillRect(0, y1 - 4, w, h - y1 + 4);
    cx.strokeStyle = C.line; cx.beginPath(); cx.moveTo(0, y1 - 4.5); cx.lineTo(w, y1 - 4.5); cx.stroke();
    const x0 = 34, x1 = w - 10;
    if (vm.etaK && vm.etaK.length) {
      const K = vm.etaK.length;
      let em = 1; for (let k = 0; k < K; k++) em = Math.max(em, Math.abs(vm.etaK[k]));
      em = Math.ceil(em);
      const ym = y1 + hh2 / 2;
      cx.beginPath();
      for (let k = 0; k <= K; k++) { const x = x0 + (x1 - x0) * k / K, y = y1 + hh2 - vm.inK[k % K] * hh2; if (k) cx.lineTo(x, y); else cx.moveTo(x, y); }
      cx.lineTo(x1, y1 + hh2); cx.lineTo(x0, y1 + hh2); cx.closePath();
      cx.fillStyle = rgb(0.14); cx.fill();
      cx.strokeStyle = 'rgba(147,184,199,0.25)'; cx.beginPath(); cx.moveTo(x0, ym); cx.lineTo(x1, ym); cx.stroke();
      cx.beginPath();
      for (let k = 0; k <= K; k++) { const x = x0 + (x1 - x0) * k / K, y = ym - vm.etaK[k % K] / em * hh2 / 2; if (k) cx.lineTo(x, y); else cx.moveTo(x, y); }
      cx.strokeStyle = C.cyan; cx.lineWidth = 1.4; cx.stroke(); cx.lineWidth = 1;
      const px = x0 + (x1 - x0) * vm.phase;
      cx.strokeStyle = C.amber; cx.beginPath(); cx.moveTo(px, y1); cx.lineTo(px, y1 + hh2); cx.stroke();
      text(cx, '±' + em, 4, y1 + 9, '400 9px ' + MONO, C.dim);
      text(cx, 'η/b', 4, ym + 4, '500 9.5px ' + MONO, C.cyan);
      text(cx, 'inflow', 4, y1 + hh2, '400 9px ' + SANS, C.dim);
      text(cx, '1 period', x1, y1 + 9, '400 9px ' + SANS, C.dim, 'right');
    } else {
      text(cx, 'Jet deflection over one period (shown while sounding)', w / 2, y1 + hh2 / 2 + 4, '400 10px ' + SANS, C.dim, 'center');
    }
  }

  /* =================================================================== gauges */
  function gauge(cx, x, y, r, frac, frac2, band, ticks, label, value, unit) {
    const a0 = Math.PI * 0.75, a1 = Math.PI * 2.25;
    const ang = (f) => a0 + (a1 - a0) * Math.min(1, Math.max(0, f));
    cx.lineWidth = 6; cx.strokeStyle = '#0c1c24';
    cx.beginPath(); cx.arc(x, y, r - 4, a0, a1); cx.stroke();
    if (band) {
      cx.strokeStyle = 'rgba(109,255,160,0.55)';
      cx.beginPath(); cx.arc(x, y, r - 4, ang(band[0]), ang(band[1])); cx.stroke();
    }
    cx.lineWidth = 1;
    for (const t of ticks) {
      const a = ang(t[0]);
      cx.strokeStyle = t[1] ? C.fg2 : C.dim2;
      cx.beginPath(); cx.moveTo(x + Math.cos(a) * (r - 9), y + Math.sin(a) * (r - 9)); cx.lineTo(x + Math.cos(a) * (r - (t[1] ? 16 : 13)), y + Math.sin(a) * (r - (t[1] ? 16 : 13))); cx.stroke();
      if (t[1]) text(cx, t[1], x + Math.cos(a) * (r - 24), y + Math.sin(a) * (r - 24) + 3, '400 8.5px ' + MONO, C.dim, 'center');
    }
    const needle = (f, col, len) => {
      const a = ang(f);
      cx.strokeStyle = col; cx.lineWidth = 2; cx.shadowColor = col; cx.shadowBlur = 6;
      cx.beginPath(); cx.moveTo(x, y); cx.lineTo(x + Math.cos(a) * len, y + Math.sin(a) * len); cx.stroke();
      cx.shadowBlur = 0; cx.lineWidth = 1;
    };
    if (frac2 !== null && frac2 !== undefined) needle(frac2, C.cyan, r - 12);
    needle(frac, C.amber, r - 8);
    cx.fillStyle = '#0a141b'; cx.beginPath(); cx.arc(x, y, 4, 0, Math.PI * 2); cx.fill();
    cx.strokeStyle = C.fg2; cx.stroke();
    text(cx, label, x, y + r * 0.45, '600 9px ' + DISP, C.dim, 'center');
    text(cx, value, x, y + r * 0.45 + 15, '600 13px ' + MONO, C.fg, 'center');
    text(cx, unit, x, y + r * 0.45 + 26, '400 8.5px ' + MONO, C.dim, 'center');
  }

  function bar(cx, x, y, ww, label, frac, band, valueText, col, center) {
    text(cx, label, x, y - 4, '600 9px ' + DISP, C.dim);
    text(cx, valueText, x + ww, y - 4, '600 11px ' + MONO, col || C.fg, 'right');
    cx.fillStyle = '#0c1c24'; cx.fillRect(x, y, ww, 8);
    if (band) { cx.fillStyle = 'rgba(109,255,160,0.25)'; cx.fillRect(x + band[0] * ww, y, (band[1] - band[0]) * ww, 8); }
    const f = Math.min(1, Math.max(0, frac));
    cx.fillStyle = col || C.cyan;
    if (center) {
      const c0 = x + ww / 2, c1 = x + f * ww;
      cx.fillRect(Math.min(c0, c1), y + 2, Math.abs(c1 - c0), 4);
      cx.fillStyle = C.fg2; cx.fillRect(c0 - 0.5, y - 2, 1, 12);
    } else cx.fillRect(x, y + 2, f * ww, 4);
    cx.fillStyle = '#eafaff'; cx.fillRect(x + f * ww - 1, y - 2, 2, 12);
  }

  function drawGauges(s) {
    const { cx, w, h } = cvs.inst;
    cx.setTransform(pr, 0, 0, pr, 0, 0);
    cx.clearRect(0, 0, w, h);
    const vm = s.vm, g = s.g, MM = OKL.E.MMWS;
    const lg = (v) => Math.log(Math.max(10, v) / 10) / Math.log(100);
    const chest = (g ? g.pchest : 735) / MM;
    gauge(cx, 59, 66, 52, lg(chest), vm.pf > 0.5 ? lg(vm.pf / MM) : null, [lg(69), lg(97)],
      [[0, '10'], [lg(20), ''], [lg(50), ''], [lg(100), '100'], [lg(200), ''], [lg(500), ''], [1, '1k']],
      'WIND', chest.toFixed(chest < 100 ? 1 : 0), 'mmWS chest/foot');
    const uj = vm.Uj || 0;
    gauge(cx, 175, 66, 52, uj / 120, null, null, [[0, '0'], [0.25, ''], [0.5, '60'], [0.75, ''], [1, '120']],
      'JET U_j', uj > 0.01 ? uj.toFixed(1) : '—', 'm/s');
    const I = s.ising || 0;
    bar(cx, 12, 150, w - 24, 'ISING  intonation no.', I / 10, [0.2, 0.3], I > 0 ? I.toFixed(2) : '—', I > 0 && (I < 2 || I > 3) ? C.amber : C.cyan);
    const an = s.an;
    let cents = 0, has = an && an.f > 0 && g;
    if (has) cents = 1200 * Math.log2(an.f / (g.ftarget * (an.modeKey || 1)));
    bar(cx, 12, 184, w - 24, 'PITCH  vs key harmonic', 0.5 + Math.max(-50, Math.min(50, cents)) / 100, null,
      has ? (cents >= 0 ? '+' : '−') + Math.abs(cents).toFixed(1) + ' ¢' : '—', Math.abs(cents) > 5 ? C.amber : C.cyan, true);
    // mode lamps
    text(cx, 'MODE', 12, 214, '600 9px ' + DISP, C.dim);
    for (let m = 1; m <= 4; m++) {
      const x = 52 + (m - 1) * 44, on = has && an.mode === m;
      cx.fillStyle = on ? (m === 1 ? C.green : C.amber) : '#0c1c24';
      cx.fillRect(x, 205, 38, 12);
      text(cx, String(m), x + 19, 215, '600 10px ' + MONO, on ? '#04130a' : C.dim, 'center');
    }
    const lv = an && an.level > -50 ? an.level : null;
    bar(cx, 12, 240, w - 24, 'LEVEL  dB SPL @1m', lv ? (lv - 40) / 60 : 0, null, lv ? lv.toFixed(1) : '—', C.cyan);
    // voices
    text(cx, 'VOICES', 12, 272, '600 9px ' + DISP, C.dim);
    let x = 12;
    const vox = (s.vox || []).slice(0, 8);
    vox.forEach((v) => {
      const t = OKL.noteLabel(v.midi);
      cx.font = '500 10px ' + MONO;
      const tw = cx.measureText(t).width + 8;
      cx.fillStyle = v.focus ? 'rgba(70,227,255,0.85)' : (v.gate ? 'rgba(70,227,255,0.22)' : 'rgba(70,227,255,0.08)');
      cx.fillRect(x, 278, tw, 14);
      text(cx, t, x + 4, 289, '500 10px ' + MONO, v.focus ? '#04222b' : C.fg2);
      x += tw + 3;
    });
    if (!vox.length) text(cx, '—', 12, 289, '500 10px ' + MONO, C.dim);
  }

  /* =================================================================== spectrogram */
  const LUT = new Uint8ClampedArray(256 * 3);
  function buildLut() {
    const stops = [[0, [3, 7, 12]], [0.22, [8, 30, 58]], [0.42, [10, 92, 128]], [0.6, [30, 185, 215]], [0.76, [140, 240, 255]], [0.88, [255, 255, 255]], [1, [255, 196, 90]]];
    for (let i = 0; i < 256; i++) {
      const t = i / 255;
      let k = 0; while (k < stops.length - 2 && t > stops[k + 1][0]) k++;
      const [ta, ca] = stops[k], [tb, cb] = stops[k + 1];
      const f = (t - ta) / (tb - ta);
      for (let c = 0; c < 3; c++) LUT[i * 3 + c] = ca[c] + (cb[c] - ca[c]) * f;
    }
  }
  const SPEC = { fmin: 30, fmax: 16000, dbLo: -112, dbHi: -22, pxPerSec: 58, acc: 0, data: null, rowBins: null };
  function specRows(H, fs, nfft) {
    const rows = [];
    for (let y = 0; y < H; y++) {
      const fa = SPEC.fmin * Math.pow(SPEC.fmax / SPEC.fmin, 1 - (y + 1) / H);
      const fb = SPEC.fmin * Math.pow(SPEC.fmax / SPEC.fmin, 1 - y / H);
      rows.push([fa * nfft / fs, fb * nfft / fs]);
    }
    return rows;
  }
  function drawSpec(dt, analyser, fs, focusF, focusLabel, marks) {
    const r = cvs.spec;
    const cx = r.cx, W = r.cv.width, H = r.cv.height;
    if (!analyser) return;
    if (!SPEC.data || SPEC.data.length !== analyser.frequencyBinCount) SPEC.data = new Float32Array(analyser.frequencyBinCount);
    if (!SPEC.rowBins || SPEC.rowH !== H) { SPEC.rowBins = specRows(H, fs, analyser.fftSize); SPEC.rowH = H; SPEC.col = cx.createImageData(1, H); }
    if (r.fresh) { cx.fillStyle = '#03070c'; cx.fillRect(0, 0, W, H); r.fresh = false; }
    SPEC.acc += dt * SPEC.pxPerSec * pr;
    let dx = Math.floor(SPEC.acc);
    if (dx < 1) return;
    SPEC.acc -= dx;
    dx = Math.min(dx, 12);
    analyser.getFloatFrequencyData(SPEC.data);
    const d = SPEC.data, n = d.length;
    cx.drawImage(r.cv, -dx, 0);
    const col = SPEC.col, px = col.data;
    for (let y = 0; y < H; y++) {
      const [ba, bb] = SPEC.rowBins[y];
      let v;
      if (bb - ba < 1) {
        const fb = (ba + bb) / 2, i0 = Math.floor(fb), f = fb - i0;
        v = (i0 + 1 < n) ? d[i0] * (1 - f) + d[i0 + 1] * f : -200;
      } else {
        v = -200;
        for (let i = Math.floor(ba); i <= Math.min(n - 1, Math.ceil(bb)); i++) if (d[i] > v) v = d[i];
      }
      const t = Math.min(255, Math.max(0, Math.round((v - SPEC.dbLo) / (SPEC.dbHi - SPEC.dbLo) * 255)));
      const o = y * 4;
      px[o] = LUT[t * 3]; px[o + 1] = LUT[t * 3 + 1]; px[o + 2] = LUT[t * 3 + 2]; px[o + 3] = 255;
    }
    for (let k = 0; k < dx; k++) cx.putImageData(col, W - dx + k, 0);
    drawSpecOverlay(focusF, focusLabel, marks);
  }
  function fToY(f, H) { return (1 - Math.log(f / SPEC.fmin) / Math.log(SPEC.fmax / SPEC.fmin)) * H; }
  function drawSpecOverlay(focusF, focusLabel, marks) {
    const { cx, w, h } = cvs.specOver;
    cx.setTransform(pr, 0, 0, pr, 0, 0);
    cx.clearRect(0, 0, w, h);
    const grid = [50, 100, 200, 500, 1000, 2000, 5000, 10000];
    cx.strokeStyle = 'rgba(147,184,199,0.13)'; cx.lineWidth = 1;
    for (const f of grid) {
      const y = Math.round(fToY(f, h)) + 0.5;
      cx.beginPath(); cx.moveTo(0, y); cx.lineTo(w - 46, y); cx.stroke();
      text(cx, f >= 1000 ? (f / 1000) + 'k' : String(f), 4, y - 3, '400 9px ' + MONO, 'rgba(147,184,199,0.6)');
    }
    // time axis
    for (let s = 1; s <= 60; s++) {
      const x = w - s * SPEC.pxPerSec;
      if (x < 20) break;
      cx.fillStyle = 'rgba(147,184,199,0.35)'; cx.fillRect(x, h - 4, 1, 4);
      if (s % 2 === 0) text(cx, '−' + s + 's', x, h - 6, '400 8.5px ' + MONO, 'rgba(147,184,199,0.45)', 'center');
    }
    // harmonic ladder of the focus pipe
    cx.fillStyle = 'rgba(2,6,10,0.75)'; cx.fillRect(w - 46, 0, 46, h);
    cx.strokeStyle = C.line; cx.beginPath(); cx.moveTo(w - 46.5, 0); cx.lineTo(w - 46.5, h); cx.stroke();
    if (marks && marks.length) {
      // MALLET: the wall's strongest modes instead of a harmonic series (n: circumferential order)
      for (const mk of marks) {
        if (!(mk.f > SPEC.fmin && mk.f < SPEC.fmax)) continue;
        const y = fToY(mk.f, h);
        cx.strokeStyle = MODE_COL(mk.n, 0.85);
        cx.beginPath(); cx.moveTo(w - 46, y); cx.lineTo(w - 34, y); cx.stroke();
        text(cx, mk.n + ',' + mk.m, w - 31, y + 3, '500 8px ' + MONO, MODE_COL(mk.n, 0.9));
      }
      text(cx, focusLabel || '', w - 23, 12, '600 10px ' + DISP, C.amber, 'center');
    } else if (focusF > 0) {
      for (let n = 1; n <= 24; n++) {
        const f = n * focusF;
        if (f > SPEC.fmax) break;
        const y = fToY(f, h);
        const big = n === 1 || n === 2 || n === 4 || n === 8;
        cx.strokeStyle = n === 1 ? C.amber : 'rgba(70,227,255,' + (big ? 0.8 : 0.4) + ')';
        cx.beginPath(); cx.moveTo(w - 46, y); cx.lineTo(w - (big ? 30 : 38), y); cx.stroke();
        if (n <= 6 || big) text(cx, String(n), w - 26, y + 3, '500 8.5px ' + MONO, n === 1 ? C.amber : C.fg2);
      }
      text(cx, focusLabel || '', w - 23, 12, '600 10px ' + DISP, C.amber, 'center');
    }
  }

  /* =================================================================== harmonics */
  function drawHarm(s) {
    const { cx, w, h } = cvs.harm;
    cx.setTransform(pr, 0, 0, pr, 0, 0);
    cx.clearRect(0, 0, w, h);
    const an = s.an, N = 16;
    const x0 = 112, x1 = w - 12, top = 10, bot = h - 18;
    text(cx, 'HARMONICS', 10, 22, '600 11px ' + DISP, C.fg);
    text(cx, 'focus pipe', 10, 35, '500 9px ' + DISP, C.dim);
    text(cx, 'dB re fund.', 10, 50, '400 9.5px ' + SANS, C.dim);
    cx.fillStyle = C.amber; cx.fillRect(10, 60, 8, 2);
    text(cx, 'Phase 1 ref.', 22, 64, '400 9px ' + SANS, C.fg2);
    const dbMin = -60;
    const yOf = (db) => top + (bot - top) * Math.min(1, Math.max(0, db / dbMin));
    for (const db of [0, -20, -40, -60]) {
      const y = Math.round(yOf(db)) + 0.5;
      cx.strokeStyle = 'rgba(147,184,199,0.12)'; cx.beginPath(); cx.moveTo(x0 - 4, y); cx.lineTo(x1, y); cx.stroke();
      text(cx, String(db), x0 - 8, y + 3, '400 8.5px ' + MONO, C.dim, 'right');
    }
    const bw = (x1 - x0) / N;
    for (let n = 1; n <= N; n++) {
      const x = x0 + (n - 1) * bw;
      text(cx, String(n), x + bw / 2, h - 5, '400 8.5px ' + MONO, n % 2 ? C.dim : C.fg2, 'center');
      if (an && an.harm && isFinite(an.harm[n - 1])) {
        const v = an.harm[n - 1];
        const y = yOf(v);
        const gr = cx.createLinearGradient(0, y, 0, bot);
        const even = n % 2 === 0;
        gr.addColorStop(0, even ? 'rgba(70,227,255,0.95)' : 'rgba(160,240,255,0.95)');
        gr.addColorStop(1, even ? 'rgba(70,227,255,0.15)' : 'rgba(160,240,255,0.12)');
        cx.fillStyle = gr;
        cx.fillRect(x + bw * 0.18, y, bw * 0.64, bot - y);
      }
      if (s.harmRef && isFinite(s.harmRef[n - 1])) {
        const y = yOf(s.harmRef[n - 1]);
        cx.fillStyle = C.amber; cx.fillRect(x + bw * 0.1, y - 1, bw * 0.8, 2);
      }
    }
  }

  /* =================================================================== MALLET (plugin): the struck pipe */
  /* colour of a circumferential order: 0 breathing, 1 bending, 2.. ovalling */
  function MODE_COL(n, a) {
    if (n === 0) return 'rgba(255,120,220,' + a + ')';
    if (n === 1) return 'rgba(70,227,255,' + a + ')';
    return 'rgba(255,181,71,' + a + ')';
  }
  const wallState = { amp: new Float32Array(9), strikes: -1, hitAt: -10 };

  /* panel 07: the wall's cross-section at the strike height (orders n = 0..8, exaggerated) and the stroke's force */
  function drawWall(s) {
    const { cx, w, h } = cvs.jetScope;
    cx.setTransform(pr, 0, 0, pr, 0, 0);
    cx.clearRect(0, 0, w, h);
    const ml = s.ml, g = s.g, now = performance.now() / 1000;
    const secH = 172;
    const cxp = 148, cyp = 92, R = 52;
    // amplitude per order: peak-hold of the radial displacement at the strike height, slow decay
    if (ml) {
      if (ml.strikes !== wallState.strikes) { wallState.strikes = ml.strikes; wallState.hitAt = now; }
      for (let n = 0; n < 9; n++) {
        const v = Math.abs(ml.ring[n] || 0);
        wallState.amp[n] = Math.max(v, wallState.amp[n] * Math.exp(-s.dt / 0.25));
      }
    } else wallState.amp.fill(0);
    let amax = 1e-12;
    for (let n = 0; n < 9; n++) amax = Math.max(amax, wallState.amp[n]);
    const ex = 16 / amax;                 // px per metre of wall motion (exaggerated)
    // grid
    cx.strokeStyle = 'rgba(70,227,255,0.05)'; cx.lineWidth = 1;
    for (let x = 10; x < w; x += 16) { cx.beginPath(); cx.moveTo(x, 0); cx.lineTo(x, secH); cx.stroke(); }
    text(cx, 'CROSS-SECTION AT THE STRIKE', 10, 16, '500 10px ' + SANS, C.dim);
    // rest circle and the deformed wall (slow motion: each order at its own visible rate)
    cx.strokeStyle = 'rgba(147,184,199,0.25)'; cx.setLineDash([3, 3]);
    cx.beginPath(); cx.arc(cxp, cyp, R, 0, Math.PI * 2); cx.stroke(); cx.setLineDash([]);
    cx.beginPath();
    for (let k = 0; k <= 120; k++) {
      const th = k / 120 * Math.PI * 2;
      let dr = 0;
      for (let n = 0; n < 9; n++) if (wallState.amp[n] > 0) dr += wallState.amp[n] * Math.cos(n * th) * Math.sin(2 * Math.PI * (0.35 + 0.22 * n) * now + n);
      const rr = R - dr * ex;            // inward positive (the mallet pushes in)
      const x = cxp - rr * Math.cos(th), y = cyp - rr * Math.sin(th);    // theta = 0 faces the mallet (left)
      if (k === 0) cx.moveTo(x, y); else cx.lineTo(x, y);
    }
    cx.closePath();
    cx.fillStyle = 'rgba(217,213,204,0.06)'; cx.fill();
    cx.strokeStyle = '#d9d5cc'; cx.lineWidth = 2; cx.stroke(); cx.lineWidth = 1;
    // the mallet head, true scale to the pipe: comes in, touches, flies off
    if (g && ml && s.headD) {
      const sc = R / (g.d / 2);
      const rh = Math.min(34, s.headD / 2 * sc);
      const tH = now - wallState.hitAt;
      const gap = tH < 0 ? 40 : (tH < 0.06 ? 0 : Math.min(60, (tH - 0.06) * 220));
      const hx = cxp - R - rh - gap;
      if (hx + rh > -10) {
        cx.fillStyle = s.headColor || '#b07a3a';
        cx.beginPath(); cx.arc(hx, cyp, rh, 0, Math.PI * 2); cx.fill();
        cx.fillStyle = 'rgba(255,255,255,0.18)'; cx.beginPath(); cx.arc(hx - rh * 0.3, cyp - rh * 0.3, rh * 0.35, 0, Math.PI * 2); cx.fill();
        cx.strokeStyle = '#6d5236'; cx.lineWidth = 3;
        cx.beginPath(); cx.moveTo(hx - rh, cyp); cx.lineTo(Math.max(0, hx - rh - 70), cyp - 26); cx.stroke(); cx.lineWidth = 1;
      }
      if (tH >= 0 && tH < 0.25) {
        cx.strokeStyle = 'rgba(255,181,71,' + (1 - tH / 0.25) + ')';
        cx.beginPath(); cx.arc(cxp - R, cyp, 6 + tH * 120, -1.2, 1.2); cx.stroke();
      }
    }
    // amplitudes per order (right side), 60 dB scale
    const bx = w - 118;
    text(cx, 'n   radial at the strike', bx - 12, 36, '500 8.5px ' + MONO, C.dim);
    for (let n = 0; n < 9; n++) {
      const y = 50 + n * 12, a = wallState.amp[n];
      const f = a > 0 ? Math.max(0, 1 + Math.log10(a / amax) / 3) : 0;
      cx.fillStyle = '#0c1c24'; cx.fillRect(bx, y - 7, 56, 7);
      cx.fillStyle = MODE_COL(n, 0.9); cx.fillRect(bx, y - 7, 56 * f, 7);
      text(cx, String(n), bx - 12, y, '500 9px ' + MONO, MODE_COL(n, 0.95));
      text(cx, a > 0 ? fmtUm(a) : '', bx + 60, y, '400 8.5px ' + MONO, C.fg2);
    }
    text(cx, '0 breathing · 1 bending · 2+ ovalling', 10, 164, '400 9px ' + SANS, C.dim);
    text(cx, '×' + fmtEx(ex / (R / (g ? g.d / 2 : 0.025))) + ' motion', w - 10, 164, '400 9px ' + MONO, C.dim, 'right');
    text(cx, '← MALLET', 10, 32, '500 9.5px ' + SANS, C.amber);
    // the stroke: contact force against time
    const y0 = secH + 6, y1 = h - 10, x0 = 40, x1 = w - 10;
    cx.strokeStyle = C.line; cx.strokeRect(x0, y0, x1 - x0, y1 - y0);
    if (ml && ml.strikes > 0 && ml.fPeak > 0) {
      const fp = ml.fPeak, n = ml.force.length;
      cx.beginPath();
      for (let i = 0; i < n; i++) {
        const x = x0 + (x1 - x0) * i / (n - 1), y = y1 - (y1 - y0 - 4) * Math.min(1, ml.force[i] / fp);
        if (i === 0) cx.moveTo(x, y); else cx.lineTo(x, y);
      }
      cx.strokeStyle = C.amber; cx.lineWidth = 1.5; cx.stroke(); cx.lineWidth = 1;
      text(cx, fp.toFixed(fp < 10 ? 2 : 0) + ' N', 4, y0 + 9, '500 9.5px ' + MONO, C.amber);
      text(cx, 'F', 4, y1 - 2, '500 9px ' + MONO, C.dim);
      text(cx, (ml.forceSpan * 1e3).toFixed(2) + ' ms', x1, y1 + 9, '400 8.5px ' + MONO, C.dim, 'right');
      text(cx, 'contact ' + (ml.tContact * 1e3).toFixed(3) + ' ms' + (ml.contacts > 1 ? ' · ' + ml.contacts + ' bounces' : ''),
        x0 + 6, y0 + 12, '500 10px ' + MONO, C.fg2);
    } else {
      text(cx, 'Contact force of the stroke (shown after a key is struck)', (x0 + x1) / 2, (y0 + y1) / 2 + 4, '400 10px ' + SANS, C.dim, 'center');
    }
  }
  function fmtUm(m) { const u = m * 1e6; return u >= 10 ? u.toFixed(0) + ' µm' : (u >= 0.1 ? u.toFixed(1) + ' µm' : (u * 1000).toFixed(0) + ' nm'); }
  function fmtEx(x) { return x >= 1e4 ? (x / 1e3).toFixed(0) + 'k' : x.toFixed(0); }

  /* bottom-right: the strongest modes of the wall instead of harmonics */
  function drawModes(s) {
    const { cx, w, h } = cvs.harm;
    cx.setTransform(pr, 0, 0, pr, 0, 0);
    cx.clearRect(0, 0, w, h);
    const ml = s.ml;
    const x0 = 112, x1 = w - 12, top = 10, bot = h - 18;
    text(cx, 'WALL MODES', 10, 22, '600 11px ' + DISP, C.fg);
    text(cx, 'strongest now', 10, 35, '500 9px ' + DISP, C.dim);
    text(cx, 'dB SPL @1m', 10, 50, '400 9.5px ' + SANS, C.dim);
    [[0, 'breathing'], [1, 'bending'], [2, 'ovalling']].forEach(([n, l], i) => {
      cx.fillStyle = MODE_COL(n, 0.9); cx.fillRect(10, 60 + i * 12, 8, 3);
      text(cx, l, 22, 64 + i * 12, '400 9px ' + SANS, C.fg2);
    });
    const modes = ml && ml.modes ? ml.modes.slice().sort((a, b) => a[0] - b[0]) : [];
    const N = 16, bw = (x1 - x0) / N;
    const dbTop = 100, dbLo = 20;
    const yOf = (db) => top + (bot - top) * Math.min(1, Math.max(0, (dbTop - db) / (dbTop - dbLo)));
    for (const db of [100, 80, 60, 40]) {
      const y = Math.round(yOf(db)) + 0.5;
      cx.strokeStyle = 'rgba(147,184,199,0.12)'; cx.beginPath(); cx.moveTo(x0 - 4, y); cx.lineTo(x1, y); cx.stroke();
      text(cx, String(db), x0 - 8, y + 3, '400 8.5px ' + MONO, C.dim, 'right');
    }
    modes.forEach((md, i) => {
      const [f, lv, t60, n, m] = md, x = x0 + i * bw, y = yOf(lv);
      const gr = cx.createLinearGradient(0, y, 0, bot);
      gr.addColorStop(0, MODE_COL(n, 0.95)); gr.addColorStop(1, MODE_COL(n, 0.12));
      cx.fillStyle = gr; cx.fillRect(x + bw * 0.18, y, bw * 0.64, bot - y);
      text(cx, f >= 1000 ? (f / 1000).toFixed(f >= 10000 ? 0 : 1) + 'k' : f.toFixed(0), x + bw / 2, h - 5, '400 8px ' + MONO, C.fg2, 'center');
      text(cx, n + ',' + m, x + bw / 2, Math.max(top + 8, y - 3), '400 7.5px ' + MONO, C.dim, 'center');
    });
    if (!modes.length) text(cx, 'Strike a key: the wall rings in its own modes', (x0 + x1) / 2, (top + bot) / 2, '400 10px ' + SANS, C.dim, 'center');
  }

  /* instruments panel for the struck pipe */
  function drawMalletGauges(s) {
    const { cx, w, h } = cvs.inst;
    cx.setTransform(pr, 0, 0, pr, 0, 0);
    cx.clearRect(0, 0, w, h);
    const ml = s.ml, g = s.g;
    const v0 = ml ? ml.v0 : 0, fp = ml ? ml.fPeak : 0;
    gauge(cx, 59, 66, 52, v0 / 3, null, null, [[0, '0'], [1 / 3, '1'], [2 / 3, '2'], [1, '3']], 'MALLET', ml && ml.strikes ? v0.toFixed(2) : '—', 'm/s at the wall');
    const lgF = (f) => Math.log(Math.max(1, f)) / Math.log(1000);
    gauge(cx, 175, 66, 52, lgF(fp), null, null, [[0, '1'], [lgF(10), '10'], [lgF(100), '100'], [1, '1k']], 'FORCE', fp > 0 ? fp.toFixed(fp < 10 ? 1 : 0) : '—', 'N peak');
    const tc = ml ? ml.tContact * 1e3 : 0;
    bar(cx, 12, 150, w - 24, 'CONTACT  time on the wall', tc / 2, null, tc > 0 ? tc.toFixed(3) + ' ms' : '—', C.cyan);
    const fr = s.fres || 0;
    bar(cx, 12, 184, w - 24, 'AIR COLUMN  passive resonance', fr > 0 ? Math.log(fr / 20) / Math.log(200) : 0, null, fr > 0 ? fr.toFixed(1) + ' Hz' : '—', C.cyan);
    text(cx, 'MODES', 12, 214, '600 9px ' + DISP, C.dim);
    text(cx, ml ? ml.nActive + ' ringing of ' + ml.nModes : '—', 52, 215, '500 10px ' + MONO, C.fg2);
    const an = s.an, lv = an && an.level > -50 ? an.level : null;
    bar(cx, 12, 240, w - 24, 'LEVEL  dB SPL @1m', lv ? (lv - 40) / 60 : 0, null, lv ? lv.toFixed(1) : '—', C.cyan);
    text(cx, 'VOICES', 12, 272, '600 9px ' + DISP, C.dim);
    let x = 12;
    const vox = (s.vox || []).slice(0, 8);
    vox.forEach((v) => {
      const t = OKL.noteLabel(v.midi);
      cx.font = '500 10px ' + MONO;
      const tw = cx.measureText(t).width + 8;
      cx.fillStyle = v.focus ? 'rgba(255,181,71,0.85)' : (v.gate ? 'rgba(255,181,71,0.25)' : 'rgba(255,181,71,0.08)');
      cx.fillRect(x, 278, tw, 14);
      text(cx, t, x + 4, 289, '500 10px ' + MONO, v.focus ? '#2a1700' : C.fg2);
      x += tw + 3;
    });
    if (!vox.length) text(cx, '—', 12, 289, '500 10px ' + MONO, C.dim);
  }

  /* =================================================================== annunciators */
  const annState = {};
  function ann(id, cls) {
    if (annState[id] === cls) return;
    annState[id] = cls;
    const el = document.getElementById('an-' + id);
    if (el) el.className = 'ann' + (cls ? ' ' + cls : '');
  }

  return { init, sizeAll, drawJet, drawGauges, drawSpec, drawHarm, ann, SPEC, drawWall, drawModes, drawMalletGauges, MODE_COL };
})();
