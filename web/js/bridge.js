/*
 * OkumuLab 1 — plugin bridge (Phase 4)
 *
 * Inside the plugin (JUCE WebView2: window.__JUCE__ exists) the screen no longer
 * runs its own engine. The C++ engine plays, and this file stands in for
 * js/audio.js with the same interface:
 *   to the plugin   parameter moves, notes and pads (as MIDI), RESET / PANIC,
 *                   MIDI learn, frame-rate reports
 *   from the plugin about 60 times a second: the engine telemetry in the Phase 2
 *                   message format ({t:'tel', tel, sig, caps, cpu, ...}), the
 *                   output signal (spectrogram), parameter changes made by the
 *                   host or a controller, MIDI activity, pad pressures
 * Outside the plugin it does nothing: the browser prototype keeps js/audio.js.
 */
window.OKL = window.OKL || {};
(function () {
  'use strict';
  const J = window.__JUCE__;
  if (!J || !J.backend) { OKL.host = 'browser'; return; }
  OKL.host = 'plugin';
  document.documentElement.classList.add('plugin');
  const be = J.backend;
  const emit = (m) => be.emitEvent('okl', m);

  window.addEventListener('error', (e) => emit({ t: 'log', msg: 'JS error: ' + e.message + ' @ ' + (e.filename || '').split('/').pop() + ':' + e.lineno }));

  /* frame cost: every requestAnimationFrame callback is timed (the screen's own frame loop included) */
  const perf = { work: 0, workMax: 0, tel: 0, telN: 0 };
  const raf0 = window.requestAnimationFrame.bind(window);
  window.requestAnimationFrame = (cb) => raf0((ts) => {
    const t0 = performance.now();
    cb(ts);
    const dt = performance.now() - t0;
    perf.work += dt;
    if (dt > perf.workMax) perf.workMax = dt;
  });
  function glInfo() {
    try {
      const c = document.createElement('canvas'), gl = c.getContext('webgl2');
      if (!gl) return 'no WebGL2';
      const ext = gl.getExtension('WEBGL_debug_renderer_info');
      return ext ? gl.getParameter(ext.UNMASKED_RENDERER_WEBGL) + ' / ' + gl.getParameter(ext.UNMASKED_VENDOR_WEBGL) : gl.getParameter(gl.RENDERER);
    } catch (err) { return 'WebGL error ' + err.message; }
  }
  window.addEventListener('unhandledrejection', (e) => emit({ t: 'log', msg: 'JS rejection: ' + (e.reason && e.reason.message || e.reason) }));

  function b64f32(s) {
    if (!s) return new Float32Array(0);
    const bin = atob(s), n = bin.length, u8 = new Uint8Array(n);
    for (let i = 0; i < n; i++) u8[i] = bin.charCodeAt(i);
    return new Float32Array(u8.buffer, 0, n >> 2);
  }
  const idOf = (id) => (id >= 1000 ? 'pB' + (id - 1000) : 'k' + id);   // the Phase 2 voice ids

  /* ------------------------------------------------ output signal -> AnalyserNode stand-in */
  /* fftSize 8192, Blackman window, |X| / N in dB, no smoothing: what the Phase 2 AnalyserNode gave */
  /* real FFT of N points as a complex FFT of N/2 points (even samples real, odd imaginary) */
  const N = 8192, M = N / 2, LOG2M = 12, RING = 32768;
  const ring = new Float32Array(RING);
  let ringW = 0;
  const win = new Float64Array(N), rev = new Uint32Array(M), cosM = new Float64Array(M / 2), sinM = new Float64Array(M / 2);
  const cosN = new Float64Array(M), sinN = new Float64Array(M);
  for (let i = 0; i < N; i++) win[i] = 0.42 - 0.5 * Math.cos(2 * Math.PI * i / N) + 0.08 * Math.cos(4 * Math.PI * i / N);
  for (let i = 0; i < M; i++) {
    let r = 0;
    for (let b = 0; b < LOG2M; b++) r |= ((i >> b) & 1) << (LOG2M - 1 - b);
    rev[i] = r;
    cosN[i] = Math.cos(2 * Math.PI * i / N); sinN[i] = -Math.sin(2 * Math.PI * i / N);
  }
  for (let k = 0; k < M / 2; k++) { cosM[k] = Math.cos(2 * Math.PI * k / M); sinM[k] = -Math.sin(2 * Math.PI * k / M); }
  const zr = new Float64Array(M), zi = new Float64Array(M);
  function fftM() {
    for (let len = 2, step = M / 2; len <= M; len <<= 1, step >>= 1) {
      const hl = len >> 1;
      for (let i = 0; i < M; i += len) {
        for (let j = 0, t = 0; j < hl; j++, t += step) {
          const a = i + j, b = a + hl, wr = cosM[t], wi = sinM[t];
          const xr = zr[b] * wr - zi[b] * wi, xi = zr[b] * wi + zi[b] * wr;
          zr[b] = zr[a] - xr; zi[b] = zi[a] - xi; zr[a] += xr; zi[a] += xi;
        }
      }
    }
  }
  const analyser = {
    fftSize: N, frequencyBinCount: M,
    getFloatFrequencyData(out) {
      const base = ringW - N + RING;
      for (let n = 0; n < M; n++) {
        const i0 = 2 * n, i1 = i0 + 1;
        zr[rev[n]] = ring[(base + i0) % RING] * win[i0];
        zi[rev[n]] = ring[(base + i1) % RING] * win[i1];
      }
      fftM();
      const n = Math.min(out.length, M);
      for (let k = 0; k < n; k++) {
        const kr = k === 0 ? 0 : M - k;
        const Zr = zr[k], Zi = zi[k], Cr = zr[kr], Ci = -zi[kr];          // Z[k], conj Z[M-k]
        const Er = 0.5 * (Zr + Cr), Ei = 0.5 * (Zi + Ci);                 // even samples
        const Or = 0.5 * (Zi - Ci), Oi = -0.5 * (Zr - Cr);                // odd samples
        const wr = cosN[k], wi = sinN[k];
        const Xr = Er + wr * Or - wi * Oi, Xi = Ei + wr * Oi + wi * Or;
        out[k] = 10 * Math.log10((Xr * Xr + Xi * Xi) / (N * N) + 1e-40);
      }
    },
  };

  /* ------------------------------------------------ to the plugin */
  let padCh = 9;
  const padState = { A: new Float32Array(8), B: new Float32Array(8) };
  const localT = {};                         // last local move per parameter (host echoes are ignored briefly)
  const midi = (st, d1, d2) => emit({ t: 'midi', b: [st & 0xff, d1 & 0x7f, d2 & 0x7f] });
  const clamp = (v, a, b) => Math.min(b, Math.max(a, v));

  const A = {
    ctx: { sampleRate: 48000, currentTime: 0 }, mode: 'plugin', listeners: [], analyser,
    os: 2, maxVoices: 32, hostName: 'plugin', buffers: {},

    init() {
      OKL.midi.init = async (h) => {        // MIDI comes from the host into the C++ router
        A.midiHandlers = h;
        if (h.onDevices) h.onDevices([A.hostName]);
        return 'ok';
      };
      OKL.params.onLearn = (key) => emit({ t: 'learn', key });
      return new Promise((resolve, reject) => {
        A._resolve = resolve;
        emit({ t: 'ready' });
        A._retry = setInterval(() => emit({ t: 'ready' }), 1000);
        setTimeout(() => { if (A._resolve) { clearInterval(A._retry); A._resolve = null; reject(new Error('no answer from the plugin')); } }, 10000);
      });
    },
    on(fn) { A.listeners.push(fn); },

    /* the Phase 2 engine messages, translated */
    send(m) {
      const id = m.id !== undefined ? String(m.id) : '';
      switch (m.t) {
        case 'on':
          if (id.startsWith('pB')) A.pad('B', +id.slice(2), (m.vo && m.vo.wind || 0) / 300);
          else midi(0x90, m.midi, clamp(Math.round(m.vel || 100), 1, 127));
          break;
        case 'off':
          if (id.startsWith('pB')) A.pad('B', +id.slice(2), 0);
          else if (id.startsWith('k')) midi(0x80, +id.slice(1), 0);
          break;
        case 'vo': if (id.startsWith('pB')) A.pad('B', +id.slice(2), (m.vo && m.vo.wind || 0) / 300); break;
        case 'sustain': midi(0xB0, 64, m.on ? 127 : 0); break;
        case 'alloff': emit({ t: 'panic' }); padState.A.fill(0); padState.B.fill(0); break;
        default: break;                      // 'G' (parameters go one by one), 'period', 'mute', 'cap', 'focus'
      }
    },
    pad(bank, i, p) {
      const st = padState[bank], was = st[i], note = (bank === 'A' ? 36 : 44) + i, ch = padCh >= 0 ? padCh : 9;
      st[i] = p;
      const v = clamp(Math.round(p * 127), 1, 127);
      if (p > 0 && !(was > 0)) midi(0x90 | ch, note, v);
      if (p > 0) midi(0xA0 | ch, note, v);
      else if (was > 0) midi(0x80 | ch, note, 0);
    },
    param(key, v) {
      if (key === 'master') return;                            // screen-only
      localT[key] = performance.now();
      emit({ t: 'param', key, v });
    },
    cmd(name, extra) { emit(Object.assign({ t: name }, extra || {})); },
    preset(i) { emit({ t: 'preset', i }); },                 // P5: an experiment recipe (host program)
    setReverb() {},                          // the reverb is the plugin's 'reverb' parameter
    setMaster(v) {                           // OUT slider (0..1, 0.8 = 0 dB) -> output level parameter
      const db = clamp(20 * Math.log10(Math.max(v, 1e-4) / 0.8), -30, 6);
      localT.outDb = performance.now();
      emit({ t: 'param', key: 'outDb', v: db });
    },
    // the Phase 1 recordings (REPLAY) play in the browser prototype only
    loadRecording() { return Promise.reject(new Error('Phase 1 recordings play in the browser prototype only')); },
    playBuffer() {}, stopBuffer() {}, bufferTime() { return 0; },
  };
  OKL.audio = A;

  /* ------------------------------------------------ from the plugin */
  function applyParams(p, force) {
    if (!p) return;
    const now = performance.now(), G = OKL.params.ctx ? OKL.params.ctx.G : null;
    for (const key in p) {
      const v = +p[key];
      if (key === 'outDb') {
        if (force || now - (localT.outDb || 0) > 400) {
          const el = document.getElementById('masterVol');
          if (el) el.value = clamp(0.8 * Math.pow(10, v / 20), 0, 1);
        }
        continue;
      }
      if (!OKL.params.byKey[key] || !G) continue;
      if (!force && now - (localT[key] || 0) < 400) continue;     // our own move coming back
      if (Math.abs(G[key] - v) > 1e-9 * Math.max(1, Math.abs(v))) OKL.params.set(key, v, force ? 'init' : 'host');
    }
  }
  function applyCc(cc) {
    if (!cc) return;
    for (const key in cc) if (OKL.params.setTag) OKL.params.setTag(key, cc[key]);
  }
  const NOTE = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
  function describe(st, d1, d2) {
    const ty = st & 0xf0, ch = (st & 0x0f) + 1;
    const nn = (n) => NOTE[n % 12] + (Math.floor(n / 12) - 1) + '(' + n + ')';
    switch (ty) {
      case 0x90: return d2 ? `ch${ch} NOTE ON  ${nn(d1)} vel ${d2}` : `ch${ch} NOTE OFF ${nn(d1)}`;
      case 0x80: return `ch${ch} NOTE OFF ${nn(d1)}`;
      case 0xA0: return `ch${ch} POLY AT  ${nn(d1)} ${d2}`;
      case 0xB0: return `ch${ch} CC ${d1} = ${d2}`;
      case 0xD0: return `ch${ch} CH AT ${d1}`;
      case 0xE0: return `ch${ch} PITCH ${((d2 << 7) | d1) - 8192}`;
      case 0xC0: return `ch${ch} PROG ${d1}`;
      default: return 'SYS ' + st.toString(16);
    }
  }
  function midiLog(list) {
    if (!list || !list.length) return;
    const log = OKL.midi.log;
    for (const b of list) {
      const hex = b.map((x) => x.toString(16).padStart(2, '0')).join(' ');
      log.unshift(hex.padEnd(10) + '  ' + describe(b[0], b[1], b[2]));
    }
    if (log.length > 22) log.length = 22;
    if (OKL.app) OKL.app.midiHit = performance.now();
    const mon = document.getElementById('monLog');
    if (mon && !document.getElementById('midimon').hidden) mon.textContent = log.join('\n');
  }
  let learnKey = null;
  function showLearn(key) {
    if (key === learnKey) return;
    learnKey = key;
    document.querySelectorAll('.prm.learn').forEach((r) => r.classList.remove('learn'));
    if (key) { const r = document.querySelector('.prm[data-key="' + key + '"]'); if (r) r.classList.add('learn'); }
  }

  function onInit(m) {
    clearInterval(A._retry);
    A.ctx.sampleRate = m.fs; A.os = m.os; A.maxVoices = m.maxVoices; A.hostName = m.host || 'plugin'; padCh = m.padChannel;
    A.metals = m.metals || null; A.heads = m.heads || null;          // MALLET: material tables
    A.room = m.room || null;                                         // P5: the church the reverb is computed from
    A.presets = m.presets || []; A.program = m.program;              // P5: experiment recipes, matrix menus
    A.modSources = m.modSources || null; A.modTargets = m.modTargets || null;
    A.simd = m.simd || '';                                           // P5: SSE2 or AVX2+FMA
    if (OKL.lab) OKL.lab.onInit(A);
    applyParams(m.params, true);
    applyCc(m.cc);
    const v = document.querySelector('.logo-sub');
    if (v) v.textContent = 'v' + m.version;
    if (A._resolve) { const r = A._resolve; A._resolve = null; r('plugin'); }
    emit({ t: 'log', msg: 'WebGL: ' + glInfo() + ' · ' + navigator.userAgent });
  }

  function onTel(m) {
    A.ctx.currentTime = m.time || 0;
    // output signal -> analyser ring
    const out = b64f32(m.out);
    for (let i = 0; i < out.length; i++) { ring[ringW] = out[i]; ringW = (ringW + 1) % RING; }
    const t = m.tel;
    for (const v of t.vox) v.id = idOf(v.id);
    if (t.focus) t.focus.id = idOf(t.focus.id);
    const caps = [];
    if (m.cap) {
      const c = m.cap;
      caps.push({ id: idOf(c.id), midi: c.midi, K: c.K, NX: c.NX, p: b64f32(c.p), u: b64f32(c.u), eta: b64f32(c.eta), inflow: b64f32(c.inflow),
        vm: b64f32(c.vm), T: c.T, fs: c.fs, dline: c.dline, Leff: c.Leff });
    }
    applyParams(m.params, false);
    applyCc(m.cc);
    midiLog(m.midi);
    if (m.padChannel !== undefined) padCh = m.padChannel;
    if (m.program !== undefined) { A.program = m.program; if (OKL.lab) OKL.lab.setProgram(m.program, true); }
    showLearn(m.learn || null);
    const msg = { t: 'tel', tel: t, sig: b64f32(m.sig), caps, cpu: m.cpu, nActive: t.vox.length, maxVoices: A.maxVoices,
      pads: m.pads, lock: m.lock, audio: m.audio };
    for (const fn of A.listeners) fn(msg);
  }

  be.addEventListener('okl', (m) => {
    if (!m || !m.t) return;
    if (m.t === 'init') onInit(m);
    else if (m.t === 'tel') { const t0 = performance.now(); onTel(m); perf.tel += performance.now() - t0; perf.telN++; }
  });

  /* ------------------------------------------------ frame rate (reported to the plugin, shown in the top bar) */
  let fFrames = 0, fT0 = performance.now(), fPrev = fT0, fMax = 0, fLong = 0;
  const ivals = [];
  function fpsLoop(now) {
    const dt = now - fPrev;
    fPrev = now;
    fFrames++;
    ivals.push(dt);
    if (dt > fMax) fMax = dt;
    if (dt > 25) fLong++;
    if (now - fT0 >= 1000) {
      const fps = fFrames * 1000 / (now - fT0);
      ivals.sort((a, b) => a - b);
      const p99 = ivals[Math.min(ivals.length - 1, Math.floor(ivals.length * 0.99))] || 0;
      emit({ t: 'fps', fps, maxMs: fMax, p99Ms: p99, longFrames: fLong, frames: fFrames, w: window.innerWidth, h: window.innerHeight,
        dpr: window.devicePixelRatio || 1, visible: document.visibilityState,
        workMs: perf.work, workMaxMs: perf.workMax, telMs: perf.tel, telN: perf.telN });
      perf.work = perf.workMax = perf.tel = 0; perf.telN = 0;
      const el = document.getElementById('fpsVal');
      if (el) el.textContent = fps.toFixed(0);
      fFrames = 0; fT0 = now; fMax = 0; fLong = 0; ivals.length = 0;
    }
    requestAnimationFrame(fpsLoop);
  }
  document.addEventListener('DOMContentLoaded', () => {
    const right = document.querySelector('.tb-right');
    const fsStat = document.getElementById('srVal');
    if (right && fsStat) {
      const d = document.createElement('div');
      d.className = 'tb-stat'; d.title = 'Screen frame rate';
      d.innerHTML = '<span class="k">FPS</span><span class="v" id="fpsVal">—</span>';
      right.insertBefore(d, fsStat.parentElement.nextSibling);
    }
    requestAnimationFrame(fpsLoop);
  });
})();
