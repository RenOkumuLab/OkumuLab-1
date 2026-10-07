/*
 * OkumuLab 1 — step 5 on the screen (plugin): the modulation matrix and the experiment recipes
 *
 * The matrix runs in the plugin (src/dsp/Engine.cpp): four slots, each moving one knob by
 * amount x source in that knob's own travel, separately for every sounding pipe (velocity,
 * key, envelope, level, overblowing and pitch deviation are the pipe's own). This file
 * draws the MATRIX panel over the pipe view (menus for the sources and targets, the amount
 * sliders, the sources of the focus pipe as meters), shows on the side panels where the
 * matrix has moved each knob (amber marker), and lets the 3D view and the HUD draw the pipe
 * as the matrix shapes it. The recipes are the plugin's programs (src/dsp/Presets.cpp).
 */
window.OKL = window.OKL || {};
OKL.lab = (function () {
  'use strict';
  const $ = (id) => document.getElementById(id);
  const SLOTS = 4;
  // fallbacks (the plugin sends its own lists at start)
  let srcNames = ['Off', 'Velocity', 'Key', 'LFO 1', 'LFO 2', 'Envelope', 'Mod wheel', 'Pressure', 'Pipe level', 'Overblow', 'Pitch deviation', 'Random'];
  let dstKeys = ['off', 'bellows', 'cutup', 'y0b', 'scaleHT', 'morph', 'gas', 'tempC', 'trem', 'mouthFrac', 'noise', 'toe', 'glide', 'sideHole', 'cMult',
    'rhoMult', 'fmDepth', 'fmRatio', 'jetGain', 'crossDrive', 'kappa', 'edge', 'kick', 'nicking', 'loss', 'strikePos', 'damper', 'wallMult'];
  // which sources swing both ways (drawn from the middle)
  const BIPOLAR = new Set(['Key', 'LFO 1', 'LFO 2', 'Pitch deviation', 'Random']);
  const SRC_NOTE = {
    'Velocity': 'key velocity', 'Key': 'c¹ = 0, ±4 octaves = ±1', 'LFO 1': 'sine', 'LFO 2': 'sine', 'Envelope': 'per note: attack, then decay',
    'Mod wheel': 'CC1', 'Pressure': 'channel / poly aftertouch', 'Pipe level': '40–100 dB SPL', 'Overblow': 'the pipe sounds an upper regime',
    'Pitch deviation': '±50 cents from the key', 'Random': 'per note',
  };
  let presets = [], program = -1, built = false, open = false;
  const meters = [], outs = [];
  const marked = new Set();

  const label = (key) => (key === 'off' ? 'Off' : (OKL.params.byKey[key] ? OKL.params.byKey[key].label : key));

  function fillSelects() {
    for (let k = 1; k <= SLOTS; k++) {
      const s = $('mxSrc' + k), d = $('mxDst' + k);
      s.innerHTML = srcNames.map((n, i) => `<option value="${i}">${n}</option>`).join('');
      d.innerHTML = dstKeys.map((key, i) => `<option value="${i}">${label(key)}</option>`).join('');
    }
    const box = $('mxSources');
    box.innerHTML = '';
    meters.length = 0;
    srcNames.forEach((n, i) => {
      if (i === 0) return;
      const row = document.createElement('div');
      row.className = 'mx-m';
      row.title = SRC_NOTE[n] || '';
      row.innerHTML = `<span class="mx-mn">${n}</span><div class="mx-mt${BIPOLAR.has(n) ? ' bi' : ''}"><div class="mx-mf"></div></div><span class="mx-mv">—</span>`;
      box.appendChild(row);
      meters[i] = { fill: row.querySelector('.mx-mf'), val: row.querySelector('.mx-mv'), bi: BIPOLAR.has(n), row };
    });
  }

  function build() {
    if (built) return;
    built = true;
    for (let k = 1; k <= SLOTS; k++) {
      outs[k] = $('mxOut' + k);
      $('mxSrc' + k).addEventListener('change', (e) => OKL.params.set('modSrc' + k, +e.target.value, 'ui'));
      $('mxDst' + k).addEventListener('change', (e) => OKL.params.set('modDst' + k, +e.target.value, 'ui'));
    }
    fillSelects();
    $('btnMatrix').addEventListener('click', () => toggle());
    $('matrixClose').addEventListener('click', () => toggle(false));
    $('presetSel').addEventListener('change', (e) => {
      const i = +e.target.value;
      if (OKL.audio && OKL.audio.preset) OKL.audio.preset(i);
      setProgram(i, true);
    });
    // the plugin's top bar has no room for the message line: whatever it says is shown over the pipe view for a while
    const msg = $('tbMsg'), vmsg = $('viewMsg');
    let hideT = 0, fadeT = 0;
    const hide = () => { vmsg.classList.add('fade'); fadeT = setTimeout(() => { vmsg.hidden = true; }, 400); };
    new MutationObserver(() => {
      if (OKL.host !== 'plugin' || !msg.innerHTML.trim()) return;
      vmsg.innerHTML = msg.innerHTML;
      vmsg.hidden = false; vmsg.classList.remove('fade');
      clearTimeout(hideT); clearTimeout(fadeT);
      hideT = setTimeout(hide, Math.max(6000, 60 * msg.textContent.length));
    }).observe(msg, { childList: true, characterData: true, subtree: true });
    vmsg.addEventListener('click', () => { clearTimeout(hideT); hide(); });
  }

  function toggle(on) {
    open = on === undefined ? !open : on;
    $('matrix').hidden = !open;
    $('btnMatrix').classList.toggle('on', open);
  }

  /* the plugin's lists (init message) */
  function onInit(A) {
    if (A.modSources && A.modSources.length) srcNames = A.modSources;
    if (A.modTargets && A.modTargets.length) dstKeys = A.modTargets;
    presets = A.presets || [];
    const sel = $('presetSel');
    sel.innerHTML = presets.map((p, i) => `<option value="${i}">${String(i + 1).padStart(2, '0')}  ${p.name}</option>`).join('');
    if (built) fillSelects();
    setProgram(A.program, false);
  }

  function setProgram(i, announce) {
    if (i === undefined || i === null) return;
    program = i;
    const sel = $('presetSel');
    if (sel && +sel.value !== i) sel.value = String(i);
    const p = presets[i];
    if (p && announce) {
      const msg = $('tbMsg');
      if (msg) { msg.innerHTML = '<b>RECIPE</b> ' + p.recipe; msg.title = p.recipe; }
    }
  }

  /* the knobs as the focus pipe feels them (for the drawing), from the slots' outputs */
  function modulate(Gx, tel) {
    const mo = tel && tel.mod;
    if (!mo || !mo.out) return Gx;
    let g = null;
    for (let k = 1; k <= SLOTS; k++) {
      const out = mo.out[k - 1], key = dstKeys[Math.round(Gx['modDst' + k] || 0)];
      if (!out || Math.round(Gx['modSrc' + k] || 0) <= 0 || !key || key === 'off') continue;
      const d = OKL.params.byKey[key];
      if (!d) continue;
      if (!g) g = Object.assign({}, Gx);
      g[key] = OKL.params.fromNorm(d, Math.min(1, Math.max(0, OKL.params.toNorm(d, g[key]) + out)));
    }
    return g || Gx;
  }

  /* each frame: menus follow the host, meters follow the focus pipe, amber markers on the moved knobs */
  function frame(G, Gm, tel) {
    if (!built) return;
    for (let k = 1; k <= SLOTS; k++) {
      const s = $('mxSrc' + k), d = $('mxDst' + k);
      if (document.activeElement !== s && +s.value !== Math.round(G['modSrc' + k] || 0)) s.value = String(Math.round(G['modSrc' + k] || 0));
      if (document.activeElement !== d && +d.value !== Math.round(G['modDst' + k] || 0)) d.value = String(Math.round(G['modDst' + k] || 0));
    }
    const mo = tel && tel.mod;
    if (open) {
      for (let i = 1; i < meters.length; i++) {
        const m = meters[i];
        if (!m) continue;
        const v = mo && mo.src ? mo.src[i] : 0;
        const c = Math.max(-1, Math.min(1, v || 0));
        if (m.bi) { m.fill.style.left = (50 + Math.min(0, c) * 50) + '%'; m.fill.style.width = (Math.abs(c) * 50) + '%'; }
        else { m.fill.style.left = '0%'; m.fill.style.width = (Math.max(0, c) * 100) + '%'; }
        m.val.textContent = mo ? (v >= 0 ? '+' : '−') + Math.abs(v).toFixed(2) : '—';
        let used = false;
        for (let k = 1; k <= SLOTS; k++) if (Math.round(G['modSrc' + k] || 0) === i && Math.round(G['modDst' + k] || 0) > 0 && G['modAmt' + k]) used = true;
        m.row.classList.toggle('used', used);
      }
      for (let k = 1; k <= SLOTS; k++) {
        const o = mo && mo.out ? mo.out[k - 1] : 0, c = Math.max(-1, Math.min(1, o || 0));
        outs[k].style.left = (50 + Math.min(0, c) * 50) + '%';
        outs[k].style.width = (Math.abs(c) * 50) + '%';
      }
    }
    // the knobs the matrix moves right now
    const now = new Set();
    if (Gm !== G) {
      for (let k = 1; k <= SLOTS; k++) {
        const key = dstKeys[Math.round(G['modDst' + k] || 0)];
        if (key && key !== 'off' && Gm[key] !== G[key]) now.add(key);
      }
    }
    for (const key of now) OKL.params.setMod(key, Gm[key]);
    for (const key of marked) if (!now.has(key)) OKL.params.setMod(key, null);
    marked.clear();
    for (const key of now) marked.add(key);
    $('btnMatrix').classList.toggle('live', !open && now.size > 0);
  }

  return { build, onInit, setProgram, modulate, frame, toggle, get open() { return open; } };
})();
