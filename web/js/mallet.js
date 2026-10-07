/*
 * OkumuLab 1 — Mallet mode on the screen (plugin): the pipe body struck instead of blown
 *
 * The sound is the plugin's (src/dsp/Mallet.cpp): the wall's modes from the metal and the
 * geometry, the mallet's Hertz contact, the radiation of the wall and the air column driven
 * by the wall's breathing. This file switches the screen over: the MALLET panel takes the
 * LAB panel's place (wall loss and reverb go with it), the knobs a struck pipe does not
 * feel are dimmed, panel 07 shows the wall's cross-section and the stroke's force, the
 * harmonics panel the strongest wall modes, the gauges the stroke.
 */
window.OKL = window.OKL || {};
OKL.mallet = (function () {
  'use strict';
  const $ = (id) => document.getElementById(id);

  /* the knobs that act on a struck pipe: geometry and atmosphere (the wall's modes, its gas loading and
     radiation, the air column), the mouth (the air column's end correction), wall loss (the air column) */
  const ACTIVE = new Set(['scaleHT', 'morph', 'glide', 'gas', 'tempC', 'cMult', 'rhoMult', 'cutup', 'mouthFrac', 'vFollow',
    'loss', 'reverb', 'metal', 'wallMult', 'head', 'headD', 'strikePos', 'damper',
    'sideHole',                                                    // P5: the hole opens the air column the wall drives
    'modAmt1', 'modAmt2', 'modAmt3', 'modAmt4', 'lfo1Rate', 'lfo2Rate', 'envAttack', 'envDecay']);   // the matrix moves the knobs above
  const MOVED = ['loss', 'reverb'];          // shown in the MALLET panel while it replaces LAB

  const ANN = {
    'an-sound': ['speaking<b>SOUND</b>', 'struck<b>RINGING</b>'],
    'an-over': ['upper mode<b>OVERBLOW</b>', 'mallet on wall<b>CONTACT</b>'],
    'an-under': ['not speaking<b>UNDER</b>', 'felt on<b>DAMPER</b>'],
    'an-lock': ['length corr.<b>LOCK</b>', 'struck: off<b>LOCK</b>'],
  };
  const HEAD_COL = [0x2a1d1b, 0xe6a63c, 0xc9a26b, 0xd8b04a];
  const HEAD_METAL = [0, 0, 0, 1];
  let on = false, lastStrikes = -1, hitAt = -1e9;

  function setMode(m) {
    on = m;
    document.documentElement.classList.toggle('mallet', m);
    const rows = OKL.params.rows;
    const dst = document.querySelector((m ? '#grp-mallet' : '#grp-lab') + ' .prms');
    for (const k of MOVED) if (rows[k] && dst) dst.appendChild(rows[k].row);
    OKL.params.setActive(m ? (k) => ACTIVE.has(k) : () => true);
    document.querySelectorAll('#exciteSeg button').forEach((b) => b.classList.toggle('on', (+b.dataset.ex >= 0.5) === m));
    const head = document.querySelector('#jetPanel .mfd-h');
    if (head) head.childNodes.forEach((nd) => { if (nd.nodeType === 3 && nd.textContent.trim()) nd.textContent = m ? 'WALL SECTION' : 'JET SECTION'; });
    for (const id in ANN) { const el = $(id); if (el) el.innerHTML = ANN[id][m ? 1 : 0]; }
    const msg = $('tbMsg');
    if (msg) msg.innerHTML = m ? '<b>MALLET</b> velocity = mallet speed · key up = damper'
      : 'Play with the computer keys (A–K) or a MIDI keyboard';
  }

  function annunciate(A, ml, tf, an, Gx, gr) {
    const now = performance.now();
    if (ml && ml.strikes !== lastStrikes) { lastStrikes = ml.strikes; hitAt = now; }
    A('sound', ml && ml.nActive > 0 && an && an.level > 35 ? 'g' : '');
    A('over', now - hitAt < 160 ? 'a' : '');
    A('under', tf && tf.gate < 0.5 && Gx.damper > 0.005 && ml && ml.nActive > 0 ? 'c' : '');
    A('stop', Gx.morph > 0.5 ? 'c' : '');
    A('gas', Math.abs(Gx.gas) > 0.005 ? 'c' : '');          // the Gas knob only, whenever it does not read AIR
    A('lock', '');
    A('lim', gr < 0.89 ? 'a' : '');
  }

  function bind() {
    document.querySelectorAll('#exciteSeg button').forEach((b) => b.addEventListener('click', () => OKL.params.set('excite', +b.dataset.ex, 'ui')));
  }

  /* what the 3D view needs this frame */
  function view3d(G, g, ml) {
    const h = Math.round(G.head);
    return { on: true, y: G.strikePos * (g.Lphys || g.L), headR: G.headD / 2000, strikes: ml ? ml.strikes : 0,
      color: HEAD_COL[h] || HEAD_COL[1], metal: HEAD_METAL[h] || 0 };
  }
  function headCss(G) { return '#' + (HEAD_COL[Math.round(G.head)] || HEAD_COL[1]).toString(16).padStart(6, '0'); }
  function metalName(G) {
    const m = OKL.audio && OKL.audio.metals && OKL.audio.metals[Math.round(G.metal)];
    return m ? m.name : '';
  }
  function headName(G) {
    const hd = OKL.audio && OKL.audio.heads && OKL.audio.heads[Math.round(G.head)];
    return hd ? hd.name : '';
  }

  return { setMode, annunciate, bind, view3d, headCss, metalName, headName, get on() { return on; }, ACTIVE };
})();
