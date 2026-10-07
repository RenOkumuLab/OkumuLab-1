/* OkumuLab 1 — audio graph: engine (AudioWorklet, ScriptProcessor fallback), reverb, analyser, replay player */
window.OKL = window.OKL || {};
(function () {
  'use strict';

  function workletSource() {
    return OKL_ENGINE_FACTORY.toString() + `
const E = OKL_ENGINE_FACTORY();
const nowf = (typeof performance !== 'undefined' && performance.now) ? () => performance.now() : () => Date.now();
class OKLProcessor extends AudioWorkletProcessor {
  constructor(o) {
    super();
    this.host = new E.Host(sampleRate, o.processorOptions || {}, (m, tr) => this.port.postMessage(m, tr), nowf);
    this.port.onmessage = (e) => this.host.onMessage(e.data);
  }
  process(inputs, outputs) {
    const out = outputs[0];
    const L = out[0], R = out[1] || out[0];
    this.host.process(L, R, L.length);
    return true;
  }
}
registerProcessor('okl-engine', OKLProcessor);
`;
  }

  /* exponentially decaying noise, low-passed at 5 kHz, 18 ms pre-delay (same recipe as render_baseline.py) */
  function makeIR(ctx, t60) {
    const fs = ctx.sampleRate, n = Math.floor(t60 * 1.2 * fs);
    const ir = ctx.createBuffer(2, n, fs);
    const a = Math.exp(-2 * Math.PI * 5000 / fs);
    for (let ch = 0; ch < 2; ch++) {
      const d = ir.getChannelData(ch);
      let lp = 0, e = 0, seed = 1234567 + ch * 7654321;
      const pre = Math.floor(0.018 * fs);
      for (let i = 0; i < n; i++) {
        seed = (seed * 1103515245 + 12345) & 0x7fffffff;
        const u1 = (seed / 0x7fffffff) || 1e-9;
        seed = (seed * 1103515245 + 12345) & 0x7fffffff;
        const u2 = seed / 0x7fffffff;
        const g = Math.sqrt(-2 * Math.log(u1)) * Math.cos(2 * Math.PI * u2);
        lp = (1 - a) * g * Math.exp(-6.9 * (i / fs) / t60) + a * lp;
        d[i] = i < pre ? 0 : lp;
        e += d[i] * d[i];
      }
      const k = 1 / Math.sqrt(e);
      for (let i = 0; i < n; i++) d[i] *= k;
    }
    return ir;
  }

  const A = {
    ctx: null, node: null, mode: 'none', listeners: [],

    async init(log) {
      const AC = window.AudioContext || window.webkitAudioContext;
      if (!AC) throw new Error('Web Audio is not available');
      const ctx = this.ctx = new AC({ latencyHint: 'interactive' });
      if (ctx.state !== 'running') await ctx.resume();

      this.master = ctx.createGain();
      this.master.gain.value = 0.8;
      this.analyser = ctx.createAnalyser();
      this.analyser.fftSize = 8192;
      this.analyser.smoothingTimeConstant = 0;
      this.analyser.minDecibels = -120;
      this.analyser.maxDecibels = -10;
      this.master.connect(this.analyser);
      this.analyser.connect(ctx.destination);

      this.dry = ctx.createGain();
      this.wet = ctx.createGain();
      this.conv = ctx.createConvolver();
      this.conv.normalize = false;
      this.conv.buffer = makeIR(ctx, 3.2);
      this.dry.connect(this.master);
      this.conv.connect(this.wet);
      this.wet.connect(this.master);
      this.replayGain = ctx.createGain();
      this.replayGain.connect(this.master);

      const kcal = window.OKL_STEP1.kcal;
      let ok = false;
      if (ctx.audioWorklet && window.AudioWorkletNode) {
        try {
          const url = URL.createObjectURL(new Blob([workletSource()], { type: 'application/javascript' }));
          await ctx.audioWorklet.addModule(url);
          this.node = new AudioWorkletNode(ctx, 'okl-engine', {
            numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
            processorOptions: { kcal, maxVoices: 8 },
          });
          this.node.port.onmessage = (e) => this._recv(e.data);
          this.send = (m) => this.node.port.postMessage(m);
          this.node.connect(this.dry);
          this.node.connect(this.conv);
          this.mode = 'worklet';
          ok = true;
        } catch (err) {
          if (log) log('AudioWorklet failed: ' + err.message);
        }
      }
      if (!ok) {
        // fallback: same host on the main thread
        const E = OKL.E;
        const host = new E.Host(ctx.sampleRate, { kcal, maxVoices: 5 }, (m) => this._recv(m), () => performance.now());
        const sp = ctx.createScriptProcessor(1024, 0, 2);
        sp.onaudioprocess = (ev) => {
          const L = ev.outputBuffer.getChannelData(0), R = ev.outputBuffer.getChannelData(1);
          for (let i = 0; i < L.length; i += 128) host.process(L.subarray(i, i + 128), R.subarray(i, i + 128), 128);
        };
        sp.connect(this.dry);
        sp.connect(this.conv);
        this.node = sp;
        this.send = (m) => host.onMessage(m);
        this.mode = 'script';
      }
      this.setReverb(0.25);
      return this.mode;
    },

    on(fn) { this.listeners.push(fn); },
    _recv(m) { for (const fn of this.listeners) fn(m); },
    send() {},

    setReverb(r) {
      if (!this.ctx) return;
      const t = this.ctx.currentTime;
      this.dry.gain.setTargetAtTime(1 - 0.55 * r, t, 0.05);
      this.wet.gain.setTargetAtTime(1.2 * r, t, 0.05);
    },
    setMaster(v) { if (this.master) this.master.gain.setTargetAtTime(v, this.ctx.currentTime, 0.03); },

    /* ------------------------------------------------ replay of Phase 1 recordings ---- */
    buffers: {},
    loadRecording(key) {
      if (this.buffers[key]) return Promise.resolve(this.buffers[key]);
      return new Promise((resolve, reject) => {
        const have = window.OKL_AUDIO && window.OKL_AUDIO[key];
        const decode = () => {
          const uri = window.OKL_AUDIO[key];
          const b64 = uri.slice(uri.indexOf(',') + 1);
          const bin = atob(b64);
          const u8 = new Uint8Array(bin.length);
          for (let i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i);
          this.ctx.decodeAudioData(u8.buffer).then((buf) => { this.buffers[key] = buf; resolve(buf); }, reject);
        };
        if (have) { decode(); return; }
        const s = document.createElement('script');
        s.src = 'audio/' + key + '.js';
        s.onload = decode;
        s.onerror = () => reject(new Error('Cannot load audio/' + key + '.js'));
        document.head.appendChild(s);
      });
    },
    playBuffer(buf, offset) {
      this.stopBuffer();
      const src = this.ctx.createBufferSource();
      src.buffer = buf;
      src.connect(this.replayGain);
      const t0 = this.ctx.currentTime + 0.03;
      src.start(t0, Math.max(0, offset));
      this.src = src;
      this.srcT0 = t0 - offset;
      return src;
    },
    stopBuffer() {
      if (this.src) { try { this.src.onended = null; this.src.stop(); } catch (e) { /* already stopped */ } this.src.disconnect(); this.src = null; }
    },
    bufferTime() { return this.src ? this.ctx.currentTime - this.srcT0 : 0; },
  };
  OKL.audio = A;
})();
