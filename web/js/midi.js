/*
 * OkumuLab 1 — Web MIDI input and the MIDI monitor.
 * The default map follows the MiniLab 3 user program (the reference controller);
 * any controller that sends these messages works.
 */
window.OKL = window.OKL || {};
OKL.midi = (function () {
  'use strict';
  /* pads on channel 10: bank A = notes 36..43, bank B = 44..51 (MiniLab 3, to be confirmed with MON) */
  const PAD_CH = 9;                       // 0-based channel 10
  const PAD_NOTES = { A: [36, 37, 38, 39, 40, 41, 42, 43], B: [44, 45, 46, 47, 48, 49, 50, 51] };
  /* CC -> parameter. The eight knobs drive the MIDI CONTROLLER panel; the faders keep
     wind pressure / tremulant (also on knobs 5 and 8), pitch lock and reverb */
  const CC_MAP = {
    74: 'cutup', 71: 'y0b', 76: 'scaleHT', 77: 'morph', 93: 'bellows', 18: 'gas', 19: 'tempC', 16: 'trem',
    82: 'bellows', 83: 'trem', 85: 'pitchLock', 17: 'reverb',
  };
  /* pad bank A: what each pad's pressure pushes */
  const PAD_A = [
    { key: 'bellows', mod: (v, p) => v * (1 + 4 * p) },          // wind x5 (the plugin multiplies the reference; same product)
    { key: 'cutup', mod: (v, p) => v * (1 - 0.55 * p) },
    { key: 'y0b', mod: (v, p) => v + 2.5 * p },
    { key: 'morph', mod: (v, p) => Math.min(1, v + p) },
    { key: 'gas', mod: (v, p) => Math.min(1, v + p) },
    { key: 'tempC', mod: (v, p) => v + 70 * p },
    { key: 'fmDepth', mod: (v, p) => Math.min(0.8, v + 0.5 * p) },
    { key: 'glide', mod: (v, p) => v - 12 * p },
  ];
  /* pad bank B: wind pipes (one pipe per pad, pressure = wind) */
  const PAD_B_NOTES = [36, 43, 48, 55, 60, 64, 67, 72];

  let access = null, h = null;
  const log = [];
  const NOTE = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

  function padOf(note) {
    for (const b of ['A', 'B']) { const i = PAD_NOTES[b].indexOf(note); if (i >= 0) return [b, i]; }
    return null;
  }

  function describe(st, d1, d2) {
    const ty = st & 0xf0, ch = (st & 0x0f) + 1;
    const nn = (n) => NOTE[n % 12] + (Math.floor(n / 12) - 1) + '(' + n + ')';
    switch (ty) {
      case 0x90: return `ch${ch} NOTE ON  ${nn(d1)} vel ${d2}`;
      case 0x80: return `ch${ch} NOTE OFF ${nn(d1)}`;
      case 0xA0: return `ch${ch} POLY AT  ${nn(d1)} ${d2}`;
      case 0xB0: return `ch${ch} CC ${d1} = ${d2}`;
      case 0xD0: return `ch${ch} CH AT ${d1}`;
      case 0xE0: return `ch${ch} PITCH ${((d2 << 7) | d1) - 8192}`;
      case 0xC0: return `ch${ch} PROG ${d1}`;
      default: return 'SYS ' + st.toString(16);
    }
  }

  function onMsg(e) {
    const d = e.data;
    if (!d || !d.length) return;
    const st = d[0];
    if (st >= 0xF8) return;            // clock / active sensing
    const d1 = d[1] || 0, d2 = d[2] || 0;
    const ty = st & 0xf0, ch = st & 0x0f;
    // monitor
    const hex = Array.from(d).map((b) => b.toString(16).padStart(2, '0')).join(' ');
    log.unshift(hex.padEnd(10) + '  ' + describe(st, d1, d2));
    if (log.length > 22) log.length = 22;
    if (h.onActivity) h.onActivity();
    const mon = document.getElementById('monLog');
    if (mon && !document.getElementById('midimon').hidden) mon.textContent = log.join('\n');

    if (ch === PAD_CH && (ty === 0x90 || ty === 0x80 || ty === 0xA0)) {
      const p = padOf(d1);
      if (p) {
        if (ty === 0x90 && d2 > 0) h.pad(p[0], p[1], Math.max(0.15, d2 / 127), 'midi');
        else if (ty === 0xA0) h.pad(p[0], p[1], Math.max(0.05, d2 / 127), 'midi');
        else h.pad(p[0], p[1], 0, 'midi');
        return;
      }
    }
    switch (ty) {
      case 0x90: if (d2 > 0) h.noteOn(d1, d2, 'midi'); else h.noteOff(d1, 'midi'); break;
      case 0x80: h.noteOff(d1, 'midi'); break;
      case 0xB0: {
        if (CC_MAP[d1]) h.cc(CC_MAP[d1], d2 / 127);
        else if (d1 === 1) h.mod(d2 / 127);
        else if (d1 === 11) h.expr(d2 / 127);
        else if (d1 === 64) h.sustain(d2 >= 64);
        else if (d1 === 123 || d1 === 120) h.allOff();
        break;
      }
      case 0xE0: h.pb((((d2 << 7) | d1) - 8192) / 8192); break;
      case 0xD0: if (ch === PAD_CH && h.chanPressure) h.chanPressure(d1 / 127); break;
    }
  }

  function bind() {
    if (!access) return;
    const names = [];
    access.inputs.forEach((inp) => { inp.onmidimessage = onMsg; names.push(inp.name); });
    if (h.onDevices) h.onDevices(names);
    const devs = document.getElementById('monDevs');
    if (devs) devs.textContent = names.length ? 'Inputs: ' + names.join(' / ') : 'No MIDI input devices';
  }

  async function init(handlers) {
    h = handlers;
    if (!navigator.requestMIDIAccess) { if (h.onDevices) h.onDevices(null); return 'unsupported'; }
    try {
      access = await navigator.requestMIDIAccess({ sysex: false });
      access.onstatechange = bind;
      bind();
      return 'ok';
    } catch (err) {
      if (h.onDevices) h.onDevices(null, err);
      return 'denied';
    }
  }

  return { init, PAD_NOTES, CC_MAP, PAD_A, PAD_B_NOTES, get log() { return log; } };
})();
