/*
 * OkumuLab 1 — v2.0 (plugin): the tuning
 *
 * The plugin tunes every key (src/dsp/Tuning.h): equal temperament, a historical temperament (the Temperament
 * knob, in this panel) or a scale loaded from Scala files (.scl, and a .kbm keyboard mapping if wanted), with a1 on the
 * Pitch knob. A tuned key sounds the canonical pipe of the nearest equal-tempered key, its body cut to the tuned
 * length, so the drawing does the same: pipe(midi) gives that key and the rest to cut [semitones].
 *
 * The TUNING panel (over the pipe view, opened by the top bar's TUNING button) shows the loaded files and one octave of the tuning; LOAD opens the
 * plugin's file dialog, and a .scl or .kbm dropped anywhere on the screen is loaded too.
 */
window.OKL = window.OKL || {};
OKL.tuning = (function () {
  'use strict';
  const $ = (id) => document.getElementById(id);
  const NAMES = ['c', 'c#', 'd', 'd#', 'e', 'f', 'f#', 'g', 'g#', 'a', 'a#', 'h'];
  let st = null, built = false, open = false;
  const roundAway = (x) => (x < 0 ? -Math.round(-x) : Math.round(x));      // (std::lround: halves away from zero)
  const hzOf = (m) => 440 * Math.pow(2, (m - 69) / 12);

  /* the pipe that sounds a key (Tuning.cpp pipeKeyFor) */
  function pipe(midi) {
    const s = st && st.semis ? (+st.semis[midi & 127] || 0) : 0;
    const key = Math.min(127, Math.max(0, roundAway(midi + s)));
    return { key, rest: midi + s - key, mapped: !st || !st.mapped || !!st.mapped[midi & 127] };
  }
  /* a key's geometry as the plugin builds it: that pipe, its rest added to the Length glide */
  function geometry(E, midi, G, fs) {
    const p = pipe(midi);
    if (!p.rest && p.key === midi) return E.displayGeometry(midi, G, fs);
    const g = E.displayGeometry(p.key, Object.assign({}, G, { glide: (G.glide || 0) + p.rest }), fs);
    g.midi = midi; g.name = E.organName(midi);              // (named for the key played)
    return g;
  }
  /* changes whenever the tuning does (cache keys) */
  function stamp() { return st ? st.stamp : 0; }

  const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

  function render() {
    if (!built || !st) return;
    const G = OKL.params.ctx ? OKL.params.ctx.G : {};
    const tIdx = Math.round(G.temperament || 0), tName = (OKL.params.byKey.temperament && OKL.params.byKey.temperament.fmt(tIdx)) || '';
    $('tuSummary').innerHTML = `<b>${esc(tName)}</b> · a¹ ${(+G.pitchA || 440).toFixed(1)} Hz` +
      (tIdx === 7 && !st.loaded ? ' · <em>no scale loaded: equal temperament</em>' : '');
    $('tuScl').innerHTML = st.scl ? `<b>${esc(st.scl)}</b>${st.description ? ' — ' + esc(st.description) : ''} <em>${st.notes} notes</em>` : '<em>none</em>';
    $('tuKbm').innerHTML = st.kbm ? `<b>${esc(st.kbm)}</b> <em>its own reference pitch (the Pitch knob does not move it)</em>`
      : '<em>none: 1/1 on c¹, a¹ on the Pitch knob</em>';
    $('tuErr').textContent = st.error ? 'Not loaded: ' + st.error : '';
    $('tuErr').hidden = !st.error;
    $('tuClearScl').disabled = !st.scl && !st.kbm;
    $('tuClearKbm').disabled = !st.kbm;
    // one octave from c¹: frequency, and the departure from equal temperament at the same a¹ (or at 440 Hz under a .kbm)
    const ref = st.usesPitch || tIdx !== 7 ? 1200 * Math.log2((+G.pitchA || 440) / 440) : 0;
    let rows = '';
    for (let m = 60; m < 72; m++) {
      const on = !st.mapped || st.mapped[m];
      const s = +st.semis[m] || 0;
      const dev = s * 100 - ref;
      rows += `<tr${on ? '' : ' class="off"'}><td>${NAMES[m - 60]}¹</td><td>${on ? (hzOf(m) * Math.pow(2, s / 12)).toFixed(2) : '—'}</td>` +
        `<td>${on ? (dev >= 0 ? '+' : '') + dev.toFixed(1) : 'silent'}</td>` +
        `<td><div class="tu-bar"><div class="tu-bf" style="${on ? barStyle(dev) : ''}"></div></div></td></tr>`;
    }
    $('tuTable').innerHTML = rows;
    let silent = 0;
    for (let m = 24; m <= 96; m++) if (st.mapped && !st.mapped[m]) silent++;
    $('tuSilent').textContent = silent ? silent + ' key(s) of the compass left out by the mapping sound nothing' : '';
    const tuned = tIdx !== 0 || Math.abs((+G.pitchA || 440) - 440) > 1e-3;
    // the top bar's TUNING button: the temperament (a loaded scale by its file's name) and a1
    const chip = $('tuneChip');
    if (chip) {
      const scaleName = st.scl ? String(st.scl).replace(/\.scl$/i, '') : '';
      const what = tIdx === 7 ? (scaleName || 'Custom: no .scl') : tName;
      const a1 = tIdx === 7 && st.kbm && !st.usesPitch ? 'pitch: .kbm' : 'a¹ ' + (+G.pitchA || 440).toFixed(1) + ' Hz';
      $('tuneChipVal').textContent = what;
      $('tuneChipA').textContent = a1;
      chip.title = 'Tuning: ' + what + ', ' + a1 + (st.kbm ? ' (' + st.kbm + ')' : '') +
        '. Click for the temperament, Pitch a¹ and Scala files (.scl / .kbm); drop a .scl or .kbm anywhere to load it';
      chip.classList.toggle('live', !open && tuned);
    }
  }
  function barStyle(dev) {
    const w = Math.min(50, Math.abs(dev) / 30 * 50);          // ±30 cents across the half bar
    return dev >= 0 ? `left:50%;width:${w}%` : `left:${50 - w}%;width:${w}%`;
  }

  function toggle(on) {
    open = on === undefined ? !open : on;
    $('tuning').hidden = !open;
    if ($('tuneChip')) $('tuneChip').classList.toggle('on', open);
    render();
  }

  /* a file dropped on the screen: its text to the plugin (UTF-8, else Latin-1 as older .scl files are) */
  function readDropped(file) {
    const kind = /\.kbm$/i.test(file.name) ? 'kbm' : (/\.scl$/i.test(file.name) ? 'scl' : null);
    if (!kind || file.size > (1 << 20)) return false;
    file.arrayBuffer().then((buf) => {
      let text;
      try { text = new TextDecoder('utf-8', { fatal: true }).decode(buf); } catch (e) { text = new TextDecoder('latin1').decode(buf); }
      OKL.audio.cmd('tuningLoad', { kind, name: file.name, text });
    });
    return true;
  }

  function build() {
    if (built || OKL.host !== 'plugin') return;
    built = true;
    if ($('tuneChip')) $('tuneChip').addEventListener('click', () => toggle());
    $('tuningClose').addEventListener('click', () => toggle(false));
    $('tuLoadScl').addEventListener('click', () => OKL.audio.cmd('tuningBrowse', { kind: 'scl' }));
    $('tuLoadKbm').addEventListener('click', () => OKL.audio.cmd('tuningBrowse', { kind: 'kbm' }));
    $('tuClearScl').addEventListener('click', () => OKL.audio.cmd('tuningClear', { kind: 'all' }));
    $('tuClearKbm').addEventListener('click', () => OKL.audio.cmd('tuningClear', { kind: 'kbm' }));
    // a file dragged over the screen: say where it goes (the drop then loads it)
    const hasFile = (e) => e.dataTransfer && Array.from(e.dataTransfer.items || []).some((it) => it.kind === 'file');
    let over = 0;
    const hint = (on) => { if ($('dropHint')) $('dropHint').hidden = !on; };
    document.addEventListener('dragenter', (e) => { if (hasFile(e)) { over++; hint(true); } });
    document.addEventListener('dragleave', () => { over = Math.max(0, over - 1); if (!over) hint(false); });
    document.addEventListener('dragover', (e) => {
      if (hasFile(e)) e.preventDefault();
    });
    document.addEventListener('drop', (e) => {
      over = 0; hint(false);
      const files = e.dataTransfer ? Array.from(e.dataTransfer.files || []) : [];
      if (!files.length) return;
      e.preventDefault();
      let any = false;
      for (const f of files) any = readDropped(f) || any;
      if (any && !open) toggle(true);
    });
  }

  /* the plugin's message (t = 'tuning'): every key's tuning, the loaded files */
  function onState(m) {
    st = Object.assign({}, m, { stamp: (st ? st.stamp : 0) + 1 });
    build();
    render();
  }

  return { pipe, geometry, stamp, onState, render, build, get open() { return open; } };
})();
