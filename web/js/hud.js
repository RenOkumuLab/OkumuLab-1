/* OkumuLab 1 — 2D head-up display over the pipe view (rulers, callouts, bore pressure, telemetry) */
window.OKL = window.OKL || {};

/* organ (Helmholtz) note names with superscripts, English B for German H: c1 -> c¹ */
OKL.noteLabel = function (midi) {
  const s = OKL.E.organName(midi).replace(/^H/, 'B').replace(/^h/, 'b');
  const sup = { 0: '⁰', 1: '¹', 2: '²', 3: '³', 4: '⁴', 5: '⁵', 6: '⁶', 7: '⁷', 8: '⁸', 9: '⁹' };
  return s.replace(/(\d+)$/, (m) => m.split('').map((c) => sup[c]).join(''));
};

OKL.hud = (function () {
  'use strict';
  const C = {
    cyan: '#46e3ff', cyan2: 'rgba(70,227,255,0.55)', cyanD: 'rgba(70,227,255,0.22)', amber: '#ffb547', red: '#ff4d5e',
    green: '#6dffa0', fg: '#d6f4ff', fg2: '#93b8c7', dim: '#58737f', blue: '#5c8dff',
  };
  const MONO = '"JetBrains Mono", Consolas, monospace', DISP = '"Chakra Petch", "Noto Sans JP", sans-serif', SANS = '"Chakra Petch", "Segoe UI", sans-serif';
  let cv, cx, w = 868, h = 594, scale = 1;
  const pa = {}, pb = {};

  function init(canvas) { cv = canvas; cx = canvas.getContext('2d'); }
  function resize(cw, ch, pr) {
    w = cw; h = ch; scale = pr;
    cv.width = Math.round(cw * pr); cv.height = Math.round(ch * pr);
  }

  function niceStep(x) {
    const p = Math.pow(10, Math.floor(Math.log10(x))), m = x / p;
    return (m < 1.5 ? 1 : m < 3.5 ? 2 : m < 7.5 ? 5 : 10) * p;
  }
  function fmtLen(m) {
    const mm = m * 1e3;
    if (Math.abs(mm) >= 1000) return (m).toFixed(m >= 10 ? 1 : 2) + ' m';
    if (Math.abs(mm) >= 100) return mm.toFixed(0) + ' mm';
    if (Math.abs(mm) >= 10) return mm.toFixed(1) + ' mm';
    return mm.toFixed(2) + ' mm';
  }
  OKL.fmtLen = fmtLen;

  function text(s, x, y, font, color, align, base) {
    cx.font = font; cx.fillStyle = color; cx.textAlign = align || 'left'; cx.textBaseline = base || 'alphabetic';
    cx.fillText(s, x, y);
  }
  function tag(s, x, y, color, align) {
    cx.font = '500 10.5px ' + MONO;
    const tw = cx.measureText(s).width + 10;
    const x0 = align === 'right' ? x - tw : (align === 'center' ? x - tw / 2 : x);
    cx.fillStyle = 'rgba(3,9,14,0.82)';
    cx.fillRect(x0, y - 8, tw, 16);
    cx.strokeStyle = color; cx.lineWidth = 1;
    cx.strokeRect(x0 + 0.5, y - 7.5, tw - 1, 15);
    text(s, x0 + 5, y + 4, '500 10.5px ' + MONO, color);
    return tw;
  }
  function arrowSeg(p0, p1, col) {
    cx.strokeStyle = col; cx.fillStyle = col; cx.lineWidth = 1;
    cx.beginPath(); cx.moveTo(p0.x, p0.y); cx.lineTo(p1.x, p1.y); cx.stroke();
    const L = Math.hypot(p1.x - p0.x, p1.y - p0.y) || 1, ux = (p1.x - p0.x) / L, uy = (p1.y - p0.y) / L;
    const head = (p, sg) => {
      const bx = p.x + sg * 6 * ux, by = p.y + sg * 6 * uy;
      cx.beginPath(); cx.moveTo(p.x, p.y); cx.lineTo(bx - 3 * uy, by + 3 * ux); cx.lineTo(bx + 3 * uy, by - 3 * ux); cx.fill();
    };
    head(p0, 1); head(p1, -1);
  }
  function brackets(x, y, ww, hh, col, len) {
    cx.strokeStyle = col; cx.lineWidth = 1.2;
    const L = len || 12;
    cx.beginPath();
    cx.moveTo(x, y + L); cx.lineTo(x, y); cx.lineTo(x + L, y);
    cx.moveTo(x + ww - L, y); cx.lineTo(x + ww, y); cx.lineTo(x + ww, y + L);
    cx.moveTo(x + ww, y + hh - L); cx.lineTo(x + ww, y + hh); cx.lineTo(x + ww - L, y + hh);
    cx.moveTo(x + L, y + hh); cx.lineTo(x, y + hh); cx.lineTo(x, y + hh - L);
    cx.stroke();
  }

  /*
   * s: { vm, g (display geometry), an (analysis), src (label), mode, ghosts, rec (recorded values), G }
   */
  function draw(s) {
    cx.setTransform(scale, 0, 0, scale, 0, 0);
    cx.clearRect(0, 0, w, h);
    const P = OKL.pipe3d, g = s.g, I = P.info;
    if (!g || !I) return;
    const view = P.state.view;
    if (view === 'full') drawFull(s, g, I);
    else drawMouthView(s, g, I);
    drawHeader(s, g);
    drawTelemetry(s, g);
    drawStrobe(s);
    drawPip(s, g);
    drawGhostLabels(s);
    if (s.hint) {
      text(s.hint, w * 0.39, h - 18, '400 12px ' + SANS, 'rgba(147,184,199,0.75)', 'center');
    }
  }

  /*
   * The pipe leans, so everything along it is drawn in the pipe's own screen frame:
   * u runs along the axis toward the top, nL points to the left of the pipe.
   */
  function drawFull(s, g, I) {
    const P = OKL.pipe3d, vm = s.vm;
    const r = I.r;
    const at = (v) => P.projectTilt(0, v, 0, {});                 // point on the axis (v from the languid)
    const bot = at(-g.footLen), mid = at(0), top = at(g.L);
    const ax = top.x - bot.x, ay = top.y - bot.y, alen = Math.hypot(ax, ay) || 1;
    const u = { x: ax / alen, y: ay / alen }, nL = { x: u.y, y: -u.x };
    const e1 = P.projectTilt(-r, g.L * 0.5, 0, {}), e2 = P.projectTilt(r, g.L * 0.5, 0, {});
    const rpx = Math.max(4, Math.hypot(e2.x - e1.x, e2.y - e1.y) / 2);
    const off = (p, d, along) => ({ x: p.x + nL.x * d + u.x * (along || 0), y: p.y + nL.y * d + u.y * (along || 0) });
    const seg = (a, b) => { cx.beginPath(); cx.moveTo(a.x, a.y); cx.lineTo(b.x, b.y); cx.stroke(); };

    // ---------------- ruler (left, parallel to the pipe)
    const dR = rpx + 30;
    const span = g.L + g.footLen, pxPerM = alen / span;
    const major = niceStep(48 / pxPerM), minor = major / 5;
    cx.strokeStyle = C.cyan2; cx.lineWidth = 1;
    seg(off(bot, dR), off(top, dR));
    for (let v = Math.ceil(-g.footLen / minor) * minor; v <= g.L + 1e-9; v += minor) {
      const isMaj = Math.abs(v / major - Math.round(v / major)) < 1e-6;
      const b0 = at(v);
      cx.strokeStyle = isMaj ? C.cyan2 : C.cyanD;
      seg(off(b0, dR), off(b0, dR + (isMaj ? 9 : 4)));
      if (isMaj) { const q = off(b0, dR + 12); text(fmtLen(v).replace(' mm', ''), q.x, q.y + 3.5, '400 9.5px ' + MONO, C.fg2, 'right'); }
    }
    const uq = off(top, dR + 12, 10);
    text(major * 1e3 >= 1000 ? 'm' : 'mm', uq.x, uq.y, '600 9px ' + DISP, C.dim, 'right');
    // zero mark at the languid
    cx.strokeStyle = C.amber; seg(off(mid, dR - 4), off(mid, dR + 12));

    // ---------------- dimension lines (right)
    const dD = -(rpx + 18);
    arrowSeg(off(mid, dD), off(top, dD), C.fg2);
    const lm = off(at(g.L / 2), dD);
    tag('L ' + fmtLen(g.L), lm.x + 6, lm.y, C.fg2);
    arrowSeg(off(bot, dD), off(mid, dD), C.dim);
    const fm = off(at(-g.footLen / 2), dD);
    tag('FOOT ' + fmtLen(g.footLen), fm.x + 6, fm.y, C.dim);
    // diameter across the top
    cx.strokeStyle = C.fg2; cx.fillStyle = C.fg2;
    seg(off(top, rpx, 14), off(top, -rpx, 14));
    seg(off(top, rpx, 10), off(top, rpx, 18)); seg(off(top, -rpx, 10), off(top, -rpx, 18));
    const dq = off(top, 0, 27);
    tag('Ø ' + fmtLen(g.d), dq.x, dq.y, C.fg2, 'center');

    // ---------------- callouts (left of the ruler)
    const lxAt = (p) => off(p, dR).x - 58;
    const mouth = P.projectLocal(0, g.W * 0.5, I.zc, pb);
    const facing = P.mouthFacing();
    const lxm = lxAt(at(g.W * 0.5));
    leader(mouth.x, mouth.y, lxm, mouth.y - 26, facing > -0.1 ? C.amber : C.dim);
    label2(lxm, mouth.y - 26, 'MOUTH', 'W ' + fmtLen(g.W) + ' · H ' + fmtLen(g.H), facing > -0.1 ? C.amber : C.dim);
    const lxt = lxAt(bot);
    leader(bot.x - 3, bot.y, lxt, bot.y - 8, C.dim);
    label2(lxt, bot.y - 8, 'TOE HOLE', 'Ø ' + fmtLen(g.toe), C.fg2);
    const topP = P.projectTilt(-r * 0.7, g.L, 0, pa);
    const mtxt = g.morph < 0.02 ? 'OPEN' : (g.morph > 0.98 ? 'STOPPED' : 'CLOSING ' + (g.morph * 100).toFixed(0) + '%');
    const lxp = lxAt(top);
    leader(topP.x, topP.y, lxp, topP.y + 18, g.morph > 0.02 ? C.amber : C.dim);
    label2(lxp, topP.y + 18, 'TOP', mtxt, g.morph > 0.02 ? C.amber : C.fg2);
    if (s.G.sideHole > 0.02) {
      const y0 = g.W + I.A * 1.2, y1 = g.L - 1.2 * r;
      const yh = y0 + (Math.max(y0, y1) - y0) * s.G.sideHole;
      const hp = P.projectLocal(r, yh, 0, pa);
      const lxh = lxAt(at(yh));
      leader(hp.x, hp.y, lxh, hp.y, C.amber);
      label2(lxh, hp.y, 'SIDE HOLE', fmtLen(yh) + (OKL.host === 'plugin' ? ' · tone hole' : ' (visual only)'), C.amber);
    }

    // ---------------- bore pressure profile (parallel to the pipe, on the right)
    if (vm.pInst) drawProfile(s, g, { at, off, mid, top, rpx });
  }

  function leader(x0, y0, x1, y1, col) {
    cx.strokeStyle = col; cx.lineWidth = 1;
    cx.beginPath(); cx.moveTo(x0, y0); cx.lineTo(x1 + 8, y1); cx.lineTo(x1, y1); cx.stroke();
    cx.fillStyle = col; cx.beginPath(); cx.arc(x0, y0, 2.2, 0, Math.PI * 2); cx.fill();
  }
  function label2(x, y, a, b, col) {
    text(a, x - 4, y - 3, '600 9.5px ' + DISP, col, 'right');
    text(b, x - 4, y + 10, '500 10.5px ' + MONO, C.fg, 'right');
  }

  /* F: the pipe's screen frame from drawFull */
  function drawProfile(s, g, F) {
    const vm = s.vm, n = vm.pInst.length, pm = Math.max(1e-6, vm.pmax);
    const halfW = 52, d0 = F.rpx + 92;                       // the plot starts this far right of the axis
    // frac: 0 mouth .. 1 top; across: px to the right of the plot's left edge
    const Pt = (frac, across) => F.off(F.at(frac * g.L), -(d0 + across));
    const path = (pts) => { cx.beginPath(); pts.forEach((p, i) => (i ? cx.lineTo(p.x, p.y) : cx.moveTo(p.x, p.y))); };
    // frame and centre line
    cx.strokeStyle = 'rgba(70,227,255,0.18)'; cx.lineWidth = 1;
    path([Pt(0, 0), Pt(1, 0), Pt(1, 2 * halfW), Pt(0, 2 * halfW)]); cx.closePath(); cx.stroke();
    cx.strokeStyle = 'rgba(147,184,199,0.35)';
    path([Pt(0, halfW), Pt(1, halfW)]); cx.stroke();
    const t1 = F.off(F.top, -d0, 8), t2 = F.off(F.top, -(d0 + 2 * halfW), 8), t3 = F.off(F.mid, -d0, -13);
    text('BORE p(x)', t1.x, t1.y, '600 9.5px ' + DISP, C.fg2);
    text('±' + vm.pmax.toFixed(0) + ' Pa', t2.x, t2.y, '400 9.5px ' + MONO, C.dim, 'right');
    const fr = (i) => i / (n - 1);
    // envelope band
    const env = [];
    for (let i = 0; i < n; i++) env.push(Pt(fr(i), halfW + vm.env[i] / pm * halfW));
    for (let i = n - 1; i >= 0; i--) env.push(Pt(fr(i), halfW - vm.env[i] / pm * halfW));
    path(env); cx.closePath();
    cx.fillStyle = 'rgba(70,227,255,' + (0.10 + 0.12 * vm.on) + ')'; cx.fill();
    cx.strokeStyle = 'rgba(70,227,255,0.45)'; cx.stroke();
    // instantaneous pressure
    const inst = [];
    for (let i = 0; i < n; i++) inst.push(Pt(fr(i), halfW + vm.pInst[i] / pm * halfW));
    path(inst);
    cx.strokeStyle = C.amber; cx.lineWidth = 1.6; cx.shadowColor = C.amber; cx.shadowBlur = 6; cx.stroke();
    cx.shadowBlur = 0; cx.lineWidth = 1;
    // nodes / antinodes (interior extrema of the envelope)
    const e = vm.env, marks = [];
    for (let i = 1; i < n - 1; i++) {
      if (e[i] >= e[i - 1] && e[i] > e[i + 1] && e[i] > 0.35 * pm) marks.push([i, 'A', C.cyan]);
      if (e[i] <= e[i - 1] && e[i] < e[i + 1] && e[i] < 0.5 * pm) marks.push([i, 'N', C.fg2]);
    }
    if (e[0] < 0.25 * pm) marks.push([0, 'N', C.fg2]);
    if (e[n - 1] < 0.25 * pm) marks.push([n - 1, 'N', C.fg2]);
    else if (e[n - 1] > 0.6 * pm) marks.push([n - 1, 'A', C.cyan]);
    for (const [i, lb, col] of marks) {
      const a = F.off(F.at(fr(i) * g.L), -(F.rpx + 2)), b = Pt(fr(i), 0), q = Pt(fr(i), 2 * halfW + 6);
      cx.setLineDash([2, 3]); cx.strokeStyle = col === C.cyan ? 'rgba(70,227,255,0.45)' : 'rgba(147,184,199,0.3)';
      cx.beginPath(); cx.moveTo(a.x, a.y); cx.lineTo(b.x, b.y); cx.stroke(); cx.setLineDash([]);
      text(lb, q.x, q.y + 4, '500 11px ' + SANS, col);
    }
    const nA = marks.filter((m) => m[1] === 'A').length;
    text('A antinode ' + nA + ' · N node ' + (marks.length - nA), t3.x, t3.y + 4, '500 9.5px ' + DISP, C.dim);
  }

  function drawMouthView(s, g, I) {
    const P = OKL.pipe3d;
    const lab = P.projectLocal(0, g.W, I.zc, pa);
    const flue = P.projectLocal(0, 0, I.zc, pb);
    const a = P.projectLocal(-I.half, g.W, I.zc, {}), b = P.projectLocal(I.half, g.W, I.zc, {});
    leader(lab.x, lab.y, 190, lab.y - 40, C.cyan);
    label2(190, lab.y - 40, 'LABIUM', 'at W = ' + fmtLen(g.W), C.cyan);
    leader(flue.x, flue.y, 190, flue.y + 30, C.amber);
    label2(190, flue.y + 30, 'FLUE', 'h ' + fmtLen(g.h), C.amber);
    cx.strokeStyle = C.fg2; cx.beginPath(); cx.moveTo(a.x, a.y - 16); cx.lineTo(b.x, b.y - 16); cx.stroke();
    tag('H ' + fmtLen(g.H), (a.x + b.x) / 2, (a.y + b.y) / 2 - 28, C.fg2, 'center');
  }

  function drawHeader(s, g) {
    const x = 16, y = 16;
    const an = s.an;
    text(OKL.noteLabel(g.midi), x, y + 34, '600 40px ' + DISP, '#eafaff');
    cx.font = '600 40px ' + DISP;
    const nw = cx.measureText(OKL.noteLabel(g.midi)).width;
    text("PRINZIPAL 8'", x + nw + 12, y + 14, '600 10px ' + DISP, C.dim);
    text('MIDI ' + g.midi + ' · key ' + g.ftarget.toFixed(2) + ' Hz', x + nw + 12, y + 30, '500 11px ' + MONO, C.fg2);
    let line = '';
    if (s.mallet) {
      // MALLET: the pipe is struck: what rings is the wall; the air column only answers at its own resonance
      const ml = s.mallet.ml, top = ml && ml.modes && ml.modes.length ? ml.modes[0] : null;
      line = top ? 'STRUCK · strongest ' + top[0].toFixed(1) + ' Hz (n ' + top[3] + ', m ' + top[4] + ')   air column ' + (s.mallet.fres || 0).toFixed(1) + ' Hz'
                 : 'STRUCK · strike a key   air column ' + (s.mallet.fres || 0).toFixed(1) + ' Hz';
      text(line, x, y + 56, '500 12px ' + MONO, top ? C.amber : C.dim);
      text('L ' + fmtLen(g.Lphys || g.L) + ' · Ø ' + fmtLen(g.d) + ' · wall ' + (ml ? fmtLen(ml.h) : '—') + ' · ' + (s.mallet.metal || ''), x, y + 73, '400 11px ' + MONO, C.fg2);
      return;
    }
    if (an && an.f > 0) {
      const cents = 1200 * Math.log2(an.f / g.ftarget);
      line = 'f ' + an.f.toFixed(2) + ' Hz   vs key ' + (cents >= 0 ? '+' : '−') + Math.abs(cents).toFixed(1) + ' ¢   MODE ' + an.mode;
    } else line = 'f —   (not sounding)';
    text(line, x, y + 56, '500 12px ' + MONO, an && an.f > 0 ? C.cyan : C.dim);
    text('L ' + fmtLen(g.L) + ' · Ø ' + fmtLen(g.d) + ' · mouth ' + fmtLen(g.H) + ' × ' + fmtLen(g.W), x, y + 73, '400 11px ' + MONO, C.fg2);
    if (s.src) {
      const tw = tag(s.src, x, y + 95, s.srcColor || C.amber);
      if (s.rec) text(s.rec, x + tw + 8, y + 99, '400 10.5px ' + MONO, C.amber);
    }
  }

  function drawTelemetry(s, g) {
    const vm = s.vm, t = s.tel;
    const x = 16, y0 = h - 132;
    if (s.mallet) return drawTelemetryMallet(s, g, x, y0);
    const rows = [
      ['U_j', vm.Uj > 0.01 ? vm.Uj.toFixed(1) + ' m/s' : '—', 'jet speed'],
      ['p_f', vm.pf > 0.5 ? (vm.pf / OKL.E.MMWS).toFixed(1) + ' mmWS' : '—', 'foot press.'],
      ['I', s.ising > 0 ? s.ising.toFixed(2) : '—', 'Ising no.'],
      ['τ', vm.tauP > 0 ? vm.tauP.toFixed(2) + ' T' : '—', 'jet delay'],
      ['η/b', vm.on > 0.05 ? (vm.etaRel >= 0 ? '+' : '') + vm.etaRel.toFixed(2) : '—', 'at labium'],
      ['inflow', vm.on > 0.05 ? (vm.inflow * 100).toFixed(0) + ' %' : '—', 'into pipe'],
      ['L_eff', t && t.Leff ? fmtLen(t.Leff) : fmtLen(g.Leff), 'acoustic L'],
    ];
    // plugin: the pitch-lock servo's length correction (cents) and whether it holds the key
    if (t && t.servo !== undefined) rows.push(['servo', (t.servo >= 0 ? '+' : '−') + Math.abs(t.servo).toFixed(1) + ' ¢' + (t.locked ? '  ✓' : ''), 'pitch lock']);
    cx.fillStyle = 'rgba(3,9,14,0.55)';
    cx.fillRect(x - 6, y0 - 14, 196, rows.length * 16 + 10);
    rows.forEach((r, i) => {
      const y = y0 + i * 16;
      text(r[0], x, y, '500 10.5px ' + MONO, C.dim);
      text(r[1], x + 44, y, '500 11px ' + MONO, C.fg);
      text(r[2], x + 184, y, '400 9.5px ' + SANS, C.dim, 'right');
    });
  }

  /* MALLET: the stroke and the wall instead of jet and wind */
  function drawTelemetryMallet(s, g, x, y0) {
    const ml = s.mallet.ml;
    const has = ml && ml.strikes > 0;
    const rows = [
      ['v', has ? ml.v0.toFixed(2) + ' m/s' : '—', 'mallet'],
      ['F', has ? ml.fPeak.toFixed(ml.fPeak < 10 ? 2 : 0) + ' N' : '—', 'peak force'],
      ['t_c', has ? (ml.tContact * 1e3).toFixed(3) + ' ms' : '—', 'contact'],
      ['m', ml ? (ml.mass * 1e3).toFixed(1) + ' g' : '—', s.mallet.head || 'head'],
      ['K', ml ? (ml.K / 1e9).toFixed(ml.K < 1e9 ? 3 : 1) + ' GN/m^1.5' : '—', 'Hertz'],
      ['h', ml ? fmtLen(ml.h) : '—', 'wall'],
      ['modes', ml ? ml.nActive + ' / ' + ml.nModes : '—', 'ringing'],
      ['f_air', (s.mallet.fres || 0).toFixed(1) + ' Hz', 'air column'],
    ];
    cx.fillStyle = 'rgba(3,9,14,0.55)';
    cx.fillRect(x - 6, y0 - 14, 196, rows.length * 16 + 10);
    rows.forEach((r, i) => {
      const y = y0 + i * 16;
      text(r[0], x, y, '500 10.5px ' + MONO, C.dim);
      text(r[1], x + 44, y, '500 11px ' + MONO, C.fg);
      text(r[2], x + 184, y, '400 9.5px ' + SANS, C.dim, 'right');
    });
  }

  function drawStrobe(s) {
    const vm = s.vm;
    const x = w * 0.39, y = h - 40;
    if (vm.source === 'attack') {
      text('ATTACK ' + vm.attackT.toFixed(0) + ' ms / 400 ms  ·  Phase 1 data (every 4 ms)', x, y + 4, '500 10.5px ' + MONO, C.amber, 'center');
      return;
    }
    if (!(vm.f > 0)) return;
    cx.strokeStyle = 'rgba(70,227,255,0.3)'; cx.lineWidth = 2;
    cx.beginPath(); cx.arc(x - 150, y, 7, 0, Math.PI * 2); cx.stroke();
    cx.strokeStyle = C.cyan;
    cx.beginPath(); cx.arc(x - 150, y, 7, -Math.PI / 2, -Math.PI / 2 + vm.phase * Math.PI * 2); cx.stroke();
    cx.lineWidth = 1;
    const slow = vm.f * vm.strobe;
    text('STROBE  1 period = ' + vm.strobe.toFixed(1) + ' s  (1/' + (slow >= 100 ? slow.toFixed(0) : slow.toFixed(1)) + ' real time)', x - 136, y + 4, '500 10.5px ' + MONO, C.fg2);
  }

  function drawPip(s, g) {
    const p = OKL.pipe3d.state.pip;
    brackets(p.x - 1, p.y - 1, p.w + 2, p.h + 2, C.cyan, 14);
    cx.strokeStyle = 'rgba(70,227,255,0.25)'; cx.strokeRect(p.x - 0.5, p.y - 0.5, p.w + 1, p.h + 1);
    const view = OKL.pipe3d.state.view;
    text(view === 'full' ? 'MOUTH CAM' : 'OVERVIEW', p.x + 8, p.y + 15, '600 10px ' + DISP, C.cyan);
    if (view === 'full') {
      const vm = s.vm;
      const s1 = vm.on > 0.05 ? 'η/b ' + (vm.etaRel >= 0 ? '+' : '') + vm.etaRel.toFixed(1) + '   inflow ' + (vm.inflow * 100).toFixed(0) + '%' : 'no jet';
      cx.fillStyle = 'rgba(2,6,10,0.7)'; cx.fillRect(p.x, p.y + p.h - 20, p.w, 20);
      text(s1, p.x + 8, p.y + p.h - 6, '500 10.5px ' + MONO, vm.on > 0.05 ? C.fg : C.dim);
      // crosshair on the labium
      const mx = p.x + p.w / 2, my = p.y + p.h / 2;
      cx.strokeStyle = 'rgba(70,227,255,0.35)';
      cx.beginPath(); cx.moveTo(mx - 10, my); cx.lineTo(mx - 4, my); cx.moveTo(mx + 4, my); cx.lineTo(mx + 10, my);
      cx.moveTo(mx, my - 10); cx.lineTo(mx, my - 4); cx.moveTo(mx, my + 4); cx.lineTo(mx, my + 10); cx.stroke();
    }
  }

  function drawGhostLabels(s) {
    const P = OKL.pipe3d;
    if (P.state.view !== 'full') return;
    for (const gr of P.ghosts) {
      if (!gr.visible || !gr.userData.top) continue;
      const t = gr.userData.top;
      const q = P.projectWorld(t.x, t.y, t.z, pa);
      if (q.z > 1 || q.x < 0 || q.x > w - 260 || q.y < 0 || q.y > h) continue;
      text(gr.userData.label, q.x, Math.max(14, q.y - 6), '500 11px ' + DISP, 'rgba(147,184,199,0.8)', 'center');
    }
  }

  return { init, resize, draw };
})();
