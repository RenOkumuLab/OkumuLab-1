/*
 * OkumuLab 1 — the rotating pipe (Three.js)
 *
 * Everything is in metres, in pipe-local coordinates: y up, y = 0 at the
 * languid (bottom of the mouth), the mouth faces +z. The body is rebuilt on
 * the CPU whenever its dimensions change (fixed topology, so only positions
 * and normals are rewritten).
 */
window.OKL = window.OKL || {};

/* particle colour from the gas and its temperature */
OKL.gasColor = function (gas, tempC, out) {
  // hue: air = cyan, helium = pink, CO2 = lime
  let h = 190, s = 0.78, l = 0.62;
  if (gas > 0) h = 190 + (330 - 190) * gas;
  else if (gas < 0) h = 190 + (95 - 190) * (-gas);
  const t = Math.max(-1, Math.min(1, (tempC - 20) / 55));
  const ht = t > 0 ? 28 : 222;
  let dh = ((ht - h + 540) % 360) - 180;
  h = (h + dh * Math.abs(t) * 0.65 + 360) % 360;
  l += 0.06 * Math.abs(t);
  // hsl -> rgb
  const c = (1 - Math.abs(2 * l - 1)) * s, x = c * (1 - Math.abs((h / 60) % 2 - 1)), m = l - c / 2;
  let r = 0, g = 0, b = 0;
  if (h < 60) { r = c; g = x; } else if (h < 120) { r = x; g = c; } else if (h < 180) { g = c; b = x; }
  else if (h < 240) { g = x; b = c; } else if (h < 300) { r = x; b = c; } else { r = c; b = x; }
  out = out || [0, 0, 0];
  out[0] = r + m; out[1] = g + m; out[2] = b + m;
  return out;
};

OKL.pipe3d = (function () {
  'use strict';
  let T = null;
  let renderer, scene, camMain, camPip, composer, bloom;
  let cw = 868, ch = 594, pr = 1;
  let pipeG, body, bodyGeo, bodyMat, glow, glowMat, glowTex, glowData, languid, languidMat, iris, irisGeo, irisMat;
  let rim, toeRim, labEdge, flueGlow, jet, jetGeo, jetMat, pts, ptsGeo, ptsMat, holeG, floor, floorMat;
  let haze, hazeMat, mouthFrame, topRing;      // parameter effects: atmosphere haze, highlight outlines
  let ghostG, ghostGeoBody, ghostGeoFoot, ghostMat;
  let malletG, malletHead, malletHeadMat, malletStick, hitRing, MDIR, MUP;     // MALLET (plugin)
  /* the pipe leans (pivoting on its toe, which stays on the floor) and spins slowly about its own axis */
  let tiltG;
  const TILT_Z = -0.31, TILT_X = 0.08;        // ~18 deg to the right, top slightly toward the viewer
  const SPIN_PERIOD = 60;                      // seconds per turn
  const TIN = 0xd9d5cc, TIN_DARK = 0x8f8b84;   // polished organ tin, languid metal
  const ghosts = [];
  const st = {
    rotY: 0.35, autoRotate: true, pauseUntil: 0, zoom: 1, view: 'full', xray: 0.4, time: 0,
    camD: 1, tgt: null, camDM: 0.1, pip: { x: 0, y: 0, w: 250, h: 188 }, ready: false, mouthWorld: null,
    pulse: { mouth: 0, top: 0, atmos: 0 },     // highlight of the part a parameter acts on (decays)
    mallet: { on: false, y: 0.1, headR: 0.0125, strikes: 0, seen: -1, hitT: 10, color: 0xe6a63c, metal: 0 },
  };
  let g = null;          // geometry being displayed
  let gKey = '';
  let geoInfo = null;    // derived build info (zc, phiF ...)

  /* ---------------------------------------------------------------- topology */
  const SEG = [28, 8, 36, 8, 28];          // theta segments: back, lip, mouth, lip, back
  const NS = SEG.reduce((a, b) => a + b, 0);
  const ROW = [22, 8, 12, 40];             // y segments: foot, mouth, upper lip zone, body
  const NR = ROW.reduce((a, b) => a + b, 0);
  const MOUTH_C0 = SEG[0] + SEG[1], MOUTH_C1 = MOUTH_C0 + SEG[2];
  const MOUTH_R0 = ROW[0], MOUTH_R1 = ROW[0] + ROW[1];
  const smooth = (a, b, x) => { const t = Math.min(1, Math.max(0, (x - a) / (b - a))); return t * t * (3 - 2 * t); };

  function buildIndex() {
    const idx = [];
    const C = NS + 1;
    for (let i = 0; i < NR; i++) {
      for (let j = 0; j < NS; j++) {
        if (i >= MOUTH_R0 && i < MOUTH_R1 && j >= MOUTH_C0 && j < MOUTH_C1) continue;   // the mouth window
        const a = i * C + j, b = a + 1, c = a + C, d = c + 1;
        idx.push(a, c, b, b, c, d);
      }
    }
    return idx;
  }

  function thetaAt(j, phiF, thM) {
    const P = Math.PI;
    const k = [0, SEG[0], SEG[0] + SEG[1], SEG[0] + SEG[1] + SEG[2], SEG[0] + SEG[1] + SEG[2] + SEG[3], NS];
    const v = [-P, -phiF, -thM, thM, phiF, P];
    for (let s = 0; s < 5; s++) if (j <= k[s + 1]) return v[s] + (v[s + 1] - v[s]) * (j - k[s]) / (k[s + 1] - k[s]);
    return P;
  }

  function rowY(i, gg, lipZone) {
    if (i <= ROW[0]) return -gg.footLen * Math.pow(1 - i / ROW[0], 1.5);
    i -= ROW[0];
    if (i <= ROW[1]) return gg.W * i / ROW[1];
    i -= ROW[1];
    if (i <= ROW[2]) return gg.W + lipZone * i / ROW[2];
    i -= ROW[2];
    return gg.W + lipZone + (gg.L - gg.W - lipZone) * i / ROW[3];
  }

  /* derived drawing quantities (mouth on a flattened chord of the cylinder) */
  function info(gg) {
    const r = gg.d / 2;
    const half = Math.min(0.95 * r, gg.H / 2);            // mouth half width (can't exceed the diameter)
    const thM = Math.asin(half / r);
    const phiF = Math.asin(Math.min(0.985, 1.16 * half / r));
    const zc = r * Math.cos(phiF);                        // flat face (mouth plane)
    const A = Math.min(1.05 * 2 * half, 0.45 * Math.max(gg.L - gg.W, 1e-4));   // upper-lip arch height
    const Al = Math.min(0.75 * 2 * half, 0.45 * gg.footLen);                 // lower-lip arch
    const lipZone = Math.min(1.25 * A, 0.6 * Math.max(gg.L - gg.W, 1e-4));
    const rt = Math.max(gg.toe / 2, 0.08 * r);
    const hv = Math.max(gg.h, 0.01 * r);                  // flueway as drawn
    return { r, half, thM, phiF, zc, A, Al, lipZone, rt, hv };
  }

  function buildBody(gg) {
    const I = info(gg);
    const { r, thM, phiF, zc, A, Al, lipZone, rt } = I;
    const C = NS + 1;
    const pos = bodyGeo.attributes.position.array;
    for (let i = 0; i <= NR; i++) {
      const y = rowY(i, gg, lipZone);
      for (let j = 0; j <= NS; j++) {
        const th = thetaAt(j, phiF, thM);
        const at = Math.abs(th);
        let rr = r, s = 0, zcl = zc;
        if (y >= 0) {
          if (at < phiF) {
            const q = Math.sqrt(Math.max(0, 1 - (th / phiF) * (th / phiF)));
            const yTop = gg.W + A * q, bw = 0.55 * A * q + 0.04 * r;
            s = 1 - smooth(yTop - bw, yTop, y);
          }
        } else {
          const u = -y / gg.footLen;
          rr = r + (rt - r) * u;
          zcl = zc * rr / r;
          if (at < phiF) {
            const q = Math.sqrt(Math.max(0, 1 - (th / phiF) * (th / phiF)));
            const yBot = -Al * q, bw = 0.55 * Al * q + 0.04 * r;
            s = smooth(yBot, yBot + bw, y);
          }
        }
        const k = (i * C + j) * 3;
        const zcyl = rr * Math.cos(th);
        pos[k] = rr * Math.sin(th);
        pos[k + 1] = y;
        pos[k + 2] = (zcyl > zcl) ? zcyl * (1 - s) + zcl * s : zcyl;
      }
    }
    bodyGeo.attributes.position.needsUpdate = true;
    bodyGeo.computeVertexNormals();
    // weld the seam normals (theta = -pi and +pi are the same line)
    const nrm = bodyGeo.attributes.normal.array;
    for (let i = 0; i <= NR; i++) {
      const a = (i * C) * 3, b = (i * C + NS) * 3;
      for (let k = 0; k < 3; k++) { const m = 0.5 * (nrm[a + k] + nrm[b + k]); nrm[a + k] = m; nrm[b + k] = m; }
    }
    bodyGeo.attributes.normal.needsUpdate = true;
    bodyGeo.computeBoundingSphere();
    return I;
  }

  function buildLanguid(gg, I) {
    const rl = I.r * 0.985, zl = I.zc - I.hv;
    const thc = Math.acos(Math.max(-1, Math.min(1, zl / rl)));
    const sh = new T.Shape();
    const N = 64;
    for (let k = 0; k <= N; k++) {
      const th = thc + (2 * Math.PI - 2 * thc) * k / N;
      const x = rl * Math.sin(th), z = rl * Math.cos(th);
      if (k === 0) sh.moveTo(x, z); else sh.lineTo(x, z);
    }
    sh.closePath();
    const t = Math.max(0.04 * I.r, 0.0006);
    const geo = new T.ExtrudeGeometry(sh, { depth: t, bevelEnabled: false, curveSegments: 4 });
    geo.rotateX(Math.PI / 2);
    if (languid.geometry) languid.geometry.dispose();
    languid.geometry = geo;
  }

  function buildIris(gg, I) {
    const m = Math.min(1, Math.max(0, gg.morph));
    const N = 7, K = 10;
    const pos = [];
    if (m > 0.004) {
      const r = I.r * 0.99, ri = I.r * (1 - m) * 0.99;
      const span = 2 * Math.PI / N * 1.42, twist = 1.05 * m;
      for (let b = 0; b < N; b++) {
        const a0 = 2 * Math.PI * b / N;
        const yb = gg.L - 0.0025 * I.r * b;
        for (let k = 0; k < K; k++) {
          const t0 = k / K, t1 = (k + 1) / K;
          const o0 = [r * Math.sin(a0 + span * t0), yb, r * Math.cos(a0 + span * t0)];
          const o1 = [r * Math.sin(a0 + span * t1), yb, r * Math.cos(a0 + span * t1)];
          const i0 = [ri * Math.sin(a0 + twist + span * t0 * 0.55), yb, ri * Math.cos(a0 + twist + span * t0 * 0.55)];
          const i1 = [ri * Math.sin(a0 + twist + span * t1 * 0.55), yb, ri * Math.cos(a0 + twist + span * t1 * 0.55)];
          pos.push(...o0, ...i0, ...o1, ...o1, ...i0, ...i1);
        }
      }
    }
    irisGeo.setAttribute('position', new T.Float32BufferAttribute(pos, 3));
    irisGeo.computeVertexNormals();
    irisGeo.computeBoundingSphere();
    iris.visible = m > 0.004;
  }

  /* ---------------------------------------------------------------- shaders */
  const GLOW_VS = `
    varying float vY; varying vec3 vN; varying vec3 vV;
    void main() {
      vY = position.y;
      vec4 mv = modelViewMatrix * vec4(position, 1.0);
      vN = normalize(normalMatrix * normal);
      vV = normalize(-mv.xyz);
      gl_Position = projectionMatrix * mv;
    }`;
  const GLOW_FS = `
    uniform sampler2D uProf; uniform vec3 uEnvC; uniform vec3 uPosC; uniform vec3 uNegC; uniform float uOn; uniform float uIdle;
    varying float vY; varying vec3 vN; varying vec3 vV;
    void main() {
      vec4 pr = texture2D(uProf, vec2(clamp(vY, 0.0, 1.0), 0.5));
      float p = pr.r * 2.0 - 1.0;
      float env = pr.g;
      float facing = abs(dot(normalize(vN), normalize(vV)));
      vec3 col = uEnvC * (0.05 + 1.15 * env * env) + (p > 0.0 ? uPosC : uNegC) * abs(p) * 1.25;
      col = col * uOn + uEnvC * uIdle * 0.05;
      col *= (0.22 + 0.78 * facing);
      gl_FragColor = vec4(col, 1.0);
      #include <tonemapping_fragment>
      #include <colorspace_fragment>
    }`;
  const PTS_VS = `
    attribute float aSize; attribute float aAlpha; attribute vec3 aColor;
    uniform float uScale; varying vec3 vC; varying float vA;
    void main() {
      vec4 mv = modelViewMatrix * vec4(position, 1.0);
      gl_Position = projectionMatrix * mv;
      gl_PointSize = clamp(aSize * uScale / max(1e-6, -mv.z), 1.3, 26.0);
      vC = aColor; vA = aAlpha;
    }`;
  const PTS_FS = `
    varying vec3 vC; varying float vA;
    void main() {
      vec2 d = gl_PointCoord - 0.5;
      float a = smoothstep(0.5, 0.0, length(d));
      gl_FragColor = vec4(vC * a * a * vA, 1.0);
      #include <tonemapping_fragment>
      #include <colorspace_fragment>
    }`;
  const JET_VS = `
    varying vec2 vUv;
    void main() { vUv = uv; gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0); }`;
  const JET_FS = `
    uniform vec3 uC; uniform float uI;
    varying vec2 vUv;
    void main() {
      float ex = smoothstep(0.0, 0.12, vUv.x) * smoothstep(1.0, 0.88, vUv.x);
      float along = 0.55 + 0.45 * (1.0 - vUv.y);
      gl_FragColor = vec4(uC * uI * ex * along, 1.0);
      #include <tonemapping_fragment>
      #include <colorspace_fragment>
    }`;
  /* gas / temperature: slowly drifting tinted haze filling the bore */
  const HAZE_VS = `
    varying float vY; varying float vA; varying vec3 vN; varying vec3 vV;
    void main() {
      vY = position.y; vA = atan(position.x, position.z);
      vec4 mv = modelViewMatrix * vec4(position, 1.0);
      vN = normalize(normalMatrix * normal); vV = normalize(-mv.xyz);
      gl_Position = projectionMatrix * mv;
    }`;
  const HAZE_FS = `
    uniform vec3 uC; uniform float uA; uniform float uT;
    varying float vY; varying float vA; varying vec3 vN; varying vec3 vV;
    void main() {
      float facing = abs(dot(normalize(vN), normalize(vV)));
      float wisp = 0.6 + 0.4 * sin(vY * 22.0 - uT * 0.9 + 1.7 * sin(vA * 3.0 + uT * 0.35));
      float ends = smoothstep(0.0, 0.05, vY) * smoothstep(1.0, 0.92, vY);
      gl_FragColor = vec4(uC * uA * wisp * ends * (0.25 + 0.75 * facing), 1.0);
      #include <tonemapping_fragment>
      #include <colorspace_fragment>
    }`;
  const GHOST_VS = `
    varying vec3 vN; varying vec3 vV;
    void main() { vec4 mv = modelViewMatrix * vec4(position, 1.0); vN = normalize(normalMatrix * normal); vV = normalize(-mv.xyz); gl_Position = projectionMatrix * mv; }`;
  const GHOST_FS = `
    uniform vec3 uC; uniform float uA;
    varying vec3 vN; varying vec3 vV;
    void main() {
      float f = pow(1.0 - abs(dot(normalize(vN), normalize(vV))), 2.2);
      gl_FragColor = vec4(uC * (0.04 + f) * uA, 1.0);
      #include <tonemapping_fragment>
      #include <colorspace_fragment>
    }`;
  const FLOOR_VS = `
    varying vec3 vW;
    void main() { vec4 w = modelMatrix * vec4(position, 1.0); vW = w.xyz; gl_Position = projectionMatrix * viewMatrix * w; }`;
  const FLOOR_FS = `
    uniform float uCell; uniform vec3 uC; uniform vec3 uA; uniform float uFade; uniform float uR; uniform float uTime;
    varying vec3 vW;
    float gridLine(vec2 p) { vec2 gd = abs(fract(p - 0.5) - 0.5) / fwidth(p); return 1.0 - min(min(gd.x, gd.y), 1.0); }
    void main() {
      vec2 q = vW.xz;
      float l1 = gridLine(q / uCell), l5 = gridLine(q / (uCell * 5.0));
      float d = length(q);
      float fade = exp(-pow(d / uFade, 2.0));
      vec3 col = uC * (0.18 * l1 + 0.55 * l5) * fade;
      // holo pad: two rings with rotating dashes
      float ang = atan(q.y, q.x);
      float r1 = abs(d - uR * 1.7) / fwidth(d), r2 = abs(d - uR * 2.5) / fwidth(d);
      float dash = step(0.5, fract(ang * 6.0 / 6.2832 * 6.0 + uTime * 0.15));
      col += uC * (1.0 - min(r1 / 1.2, 1.0)) * 0.9;
      col += uA * (1.0 - min(r2 / 1.2, 1.0)) * 0.55 * dash;
      col += uC * exp(-pow(d / (uR * 2.2), 2.0)) * 0.06;
      gl_FragColor = vec4(col, 1.0);
      #include <tonemapping_fragment>
      #include <colorspace_fragment>
    }`;

  /* ---------------------------------------------------------------- particles */
  const NJ = 520, NF = 240, NB = 620, NL = 120;
  const NP = NJ + NF + NB + NL;
  const P = {
    x: new Float32Array(NP), y: new Float32Array(NP), z: new Float32Array(NP),
    age: new Float32Array(NP), life: new Float32Array(NP), br: new Int8Array(NP),
    vx: new Float32Array(NP), vy: new Float32Array(NP), vz: new Float32Array(NP),
    u: new Float32Array(NP), rx: new Float32Array(NP), rz: new Float32Array(NP), ry: new Float32Array(NP), seed: new Float32Array(NP),
  };
  let rnd = Math.random;

  function initParticles() {
    for (let i = 0; i < NP; i++) {
      P.age[i] = rnd(); P.life[i] = 1; P.seed[i] = rnd();
      P.u[i] = rnd(); P.br[i] = 0;
      // bore rest positions in normalised cylinder coordinates
      const a = rnd() * Math.PI * 2, rr = Math.sqrt(rnd()) * 0.82;
      P.rx[i] = rr * Math.sin(a); P.rz[i] = rr * Math.cos(a); P.ry[i] = rnd();
    }
  }

  /* ---------------------------------------------------------------- init */
  function init(canvas) {
    T = window.THREE;
    renderer = new T.WebGLRenderer({ canvas, antialias: true, powerPreference: 'high-performance' });
    renderer.setClearColor(0x050b12, 1);
    renderer.toneMapping = T.ACESFilmicToneMapping;
    renderer.toneMappingExposure = 1.05;
    renderer.outputColorSpace = T.SRGBColorSpace;

    scene = new T.Scene();
    scene.fog = null;
    camMain = new T.PerspectiveCamera(28, cw / ch, 0.001, 100);
    camPip = new T.PerspectiveCamera(30, 250 / 188, 0.0005, 100);

    // environment for the metal: a dim interior with a few soft lights, so polished tin shows
    // its light and dark vertical bands instead of reflecting a bright white room
    const pmrem = new T.PMREMGenerator(renderer);
    const room = new T.Scene();
    room.add(new T.Mesh(new T.SphereGeometry(20, 32, 16), new T.ShaderMaterial({
      side: T.BackSide,
      vertexShader: 'varying vec3 vP; void main() { vP = position; gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0); }',
      fragmentShader: 'varying vec3 vP; void main() { float h = normalize(vP).y;' +
        ' gl_FragColor = vec4(mix(vec3(0.022, 0.021, 0.019), vec3(0.11, 0.105, 0.10), smoothstep(-0.6, 0.8, h)), 1.0); }',
    })));
    const softbox = (c, x, y, z, sx, sy, sz) => {
      const m = new T.Mesh(new T.BoxGeometry(1, 1, 1), new T.MeshBasicMaterial({ color: c }));
      m.position.set(x, y, z); m.scale.set(sx, sy, sz); room.add(m);
    };
    softbox(new T.Color(4.2, 3.9, 3.4), -6, 3, 6, 0.4, 9, 2.6);      // warm key, front left
    softbox(new T.Color(2.6, 2.75, 3.0), 7, 2, -1, 0.4, 10, 1.2);    // cool strip, right
    softbox(new T.Color(1.6, 1.5, 1.35), -2, 2, -8, 4, 7, 0.4);      // dim back
    softbox(new T.Color(2.2, 2.2, 2.2), 0, 10, 0, 6, 0.4, 3);        // overhead
    scene.environment = pmrem.fromScene(room, 0.02).texture;

    scene.add(new T.AmbientLight(0xc8c6c0, 0.14));
    const key = new T.DirectionalLight(0xfff4e6, 1.3); key.position.set(-2, 3, 4); scene.add(key);
    const rimC = new T.DirectionalLight(0xd6ecff, 0.7); rimC.position.set(3, 1.5, -3); scene.add(rimC);
    const rimA = new T.DirectionalLight(0xffe2bd, 0.5); rimA.position.set(-3, 0.5, -2.5); scene.add(rimA);

    tiltG = new T.Group();
    tiltG.rotation.set(TILT_X, 0, TILT_Z);
    scene.add(tiltG);
    pipeG = new T.Group();
    tiltG.add(pipeG);

    // ---- body
    bodyGeo = new T.BufferGeometry();
    bodyGeo.setAttribute('position', new T.BufferAttribute(new Float32Array((NR + 1) * (NS + 1) * 3), 3));
    bodyGeo.setIndex(buildIndex());
    bodyMat = new T.MeshPhysicalMaterial({
      color: TIN, metalness: 1.0, roughness: 0.27, clearcoat: 0.12, clearcoatRoughness: 0.35,
      envMapIntensity: 1.0, transparent: true, side: T.DoubleSide, depthWrite: false,
    });
    bodyMat.userData.uXray = { value: st.xray };
    bodyMat.onBeforeCompile = (sh) => {
      sh.uniforms.uXray = bodyMat.userData.uXray;
      sh.fragmentShader = 'uniform float uXray;\n' + sh.fragmentShader.replace(
        '#include <normal_fragment_maps>',
        `#include <normal_fragment_maps>
         float okF = pow(1.0 - abs(dot(normalize(normal), normalize(vViewPosition))), 1.6);
         diffuseColor.a *= mix(1.0, mix(0.07, 0.95, okF), uXray);`);
    };
    body = new T.Mesh(bodyGeo, bodyMat);
    body.renderOrder = 10;
    pipeG.add(body);

    // ---- languid, rims, labium edge, flue glow
    languidMat = new T.MeshPhysicalMaterial({ color: TIN_DARK, metalness: 1, roughness: 0.45, envMapIntensity: 0.85 });
    languid = new T.Mesh(new T.BufferGeometry(), languidMat);
    pipeG.add(languid);
    const rimGeo = new T.TorusGeometry(1, 0.014, 8, 128); rimGeo.rotateX(Math.PI / 2);
    const rimMat = new T.MeshPhysicalMaterial({ color: TIN, metalness: 1, roughness: 0.22, envMapIntensity: 1.05 });
    rim = new T.Mesh(rimGeo, rimMat); pipeG.add(rim);
    toeRim = new T.Mesh(rimGeo, rimMat); pipeG.add(toeRim);
    labEdge = new T.Mesh(new T.BoxGeometry(1, 1, 1), new T.MeshBasicMaterial({ color: new T.Color(0.6, 2.6, 3.4), toneMapped: true }));
    pipeG.add(labEdge);
    flueGlow = new T.Mesh(new T.BoxGeometry(1, 1, 1), new T.MeshBasicMaterial({ color: new T.Color(1.2, 2.2, 2.6), transparent: true, blending: T.AdditiveBlending, depthWrite: false }));
    pipeG.add(flueGlow);
    // highlight outlines: mouth window (cut-up) and top rim (open <-> stopped)
    const hlMat = () => new T.LineBasicMaterial({ color: new T.Color(2.8, 1.7, 0.45), transparent: true, opacity: 0, blending: T.AdditiveBlending, depthWrite: false });
    mouthFrame = new T.LineLoop(new T.BufferGeometry(), hlMat()); mouthFrame.renderOrder = 4; pipeG.add(mouthFrame);
    const ring = [];
    for (let k = 0; k < 96; k++) { const a = 2 * Math.PI * k / 96; ring.push(Math.sin(a), 0, Math.cos(a)); }
    const ringGeo = new T.BufferGeometry(); ringGeo.setAttribute('position', new T.Float32BufferAttribute(ring, 3));
    topRing = new T.LineLoop(ringGeo, hlMat()); topRing.renderOrder = 4; pipeG.add(topRing);

    // ---- iris (open <-> stopped)
    irisGeo = new T.BufferGeometry();
    irisMat = new T.MeshPhysicalMaterial({ color: TIN, metalness: 1, roughness: 0.32, side: T.DoubleSide, envMapIntensity: 1.0, polygonOffset: true, polygonOffsetFactor: -1 });
    iris = new T.Mesh(irisGeo, irisMat); pipeG.add(iris);

    // ---- standing-wave glow column
    glowData = new Uint8Array(64 * 4);
    glowTex = new T.DataTexture(glowData, 64, 1, T.RGBAFormat);
    glowTex.magFilter = T.LinearFilter; glowTex.minFilter = T.LinearFilter; glowTex.needsUpdate = true;
    glowMat = new T.ShaderMaterial({
      vertexShader: GLOW_VS, fragmentShader: GLOW_FS, transparent: true, depthWrite: false,
      blending: T.AdditiveBlending, side: T.DoubleSide,
      uniforms: {
        uProf: { value: glowTex }, uEnvC: { value: new T.Color(0.12, 0.75, 1.0) },
        uPosC: { value: new T.Color(1.6, 0.75, 0.12) }, uNegC: { value: new T.Color(0.2, 0.45, 1.8) },
        uOn: { value: 0 }, uIdle: { value: 1 },
      },
    });
    const gGeo = new T.CylinderGeometry(1, 1, 1, 64, 1, true); gGeo.translate(0, 0.5, 0);
    glow = new T.Mesh(gGeo, glowMat); glow.renderOrder = 1;
    pipeG.add(glow);
    hazeMat = new T.ShaderMaterial({ vertexShader: HAZE_VS, fragmentShader: HAZE_FS, transparent: true, depthWrite: false,
      blending: T.AdditiveBlending, side: T.DoubleSide, uniforms: { uC: { value: new T.Color(0.3, 0.8, 1.0) }, uA: { value: 0 }, uT: { value: 0 } } });
    haze = new T.Mesh(gGeo, hazeMat); haze.renderOrder = 1; haze.visible = false;
    pipeG.add(haze);

    // ---- jet sheet
    jetGeo = new T.PlaneGeometry(1, 1, 6, 48);
    jetMat = new T.ShaderMaterial({ vertexShader: JET_VS, fragmentShader: JET_FS, transparent: true, depthWrite: false, blending: T.AdditiveBlending, side: T.DoubleSide,
      uniforms: { uC: { value: new T.Color(0.5, 1.8, 2.4) }, uI: { value: 0 } } });
    jet = new T.Mesh(jetGeo, jetMat); jet.renderOrder = 2; jet.frustumCulled = false;
    pipeG.add(jet);

    // ---- side hole
    holeG = new T.Group();
    const hd = new T.Mesh(new T.CircleGeometry(1, 40), new T.MeshBasicMaterial({ color: 0x010304 }));
    const hr = new T.Mesh(new T.TorusGeometry(1, 0.1, 8, 48), new T.MeshBasicMaterial({ color: new T.Color(2.8, 1.6, 0.35) }));
    hd.rotation.y = Math.PI / 2; hr.rotation.y = Math.PI / 2;
    holeG.add(hd, hr); holeG.visible = false;
    pipeG.add(holeG);

    // ---- MALLET (plugin): the mallet on the mouth side of the body (true size), and a flash where it hits
    malletG = new T.Group(); malletG.visible = false;
    malletHeadMat = new T.MeshPhysicalMaterial({ color: 0xe6a63c, metalness: 0, roughness: 0.35, clearcoat: 0.5, envMapIntensity: 0.8 });
    malletHead = new T.Mesh(new T.SphereGeometry(1, 28, 18), malletHeadMat);
    malletStick = new T.Mesh(new T.CylinderGeometry(1, 1, 1, 12), new T.MeshPhysicalMaterial({ color: 0x8a6a45, roughness: 0.6, metalness: 0 }));
    malletG.add(malletHead, malletStick);
    pipeG.add(malletG);
    hitRing = new T.Mesh(new T.TorusGeometry(1, 0.05, 8, 64), new T.MeshBasicMaterial({ color: new T.Color(2.8, 1.6, 0.4), transparent: true, opacity: 0, blending: T.AdditiveBlending, depthWrite: false }));
    hitRing.visible = false;
    pipeG.add(hitRing);
    MDIR = new T.Vector3(0, 0.45, 1).normalize(); MUP = new T.Vector3(0, 1, 0);

    // ---- particles
    initParticles();
    ptsGeo = new T.BufferGeometry();
    ptsGeo.setAttribute('position', new T.BufferAttribute(new Float32Array(NP * 3), 3));
    ptsGeo.setAttribute('aColor', new T.BufferAttribute(new Float32Array(NP * 3), 3));
    ptsGeo.setAttribute('aSize', new T.BufferAttribute(new Float32Array(NP), 1));
    ptsGeo.setAttribute('aAlpha', new T.BufferAttribute(new Float32Array(NP), 1));
    ptsMat = new T.ShaderMaterial({ vertexShader: PTS_VS, fragmentShader: PTS_FS, transparent: true, depthWrite: false, blending: T.AdditiveBlending,
      uniforms: { uScale: { value: 400 } } });
    pts = new T.Points(ptsGeo, ptsMat); pts.frustumCulled = false; pts.renderOrder = 3;
    pipeG.add(pts);

    // ---- floor
    floorMat = new T.ShaderMaterial({ vertexShader: FLOOR_VS, fragmentShader: FLOOR_FS, transparent: true, depthWrite: false, blending: T.AdditiveBlending,
      uniforms: { uCell: { value: 0.05 }, uC: { value: new T.Color(0.15, 0.7, 0.9) }, uA: { value: new T.Color(1.0, 0.6, 0.15) }, uFade: { value: 1 }, uR: { value: 0.03 }, uTime: { value: 0 } } });
    const fg = new T.PlaneGeometry(1, 1); fg.rotateX(-Math.PI / 2);
    floor = new T.Mesh(fg, floorMat); floor.renderOrder = 0;
    scene.add(floor);

    // ---- ghosts (other sounding pipes)
    ghostG = new T.Group(); scene.add(ghostG);
    ghostGeoBody = new T.CylinderGeometry(1, 1, 1, 40, 1, true); ghostGeoBody.translate(0, 0.5, 0);
    ghostGeoFoot = new T.CylinderGeometry(1, 0.14, 1, 40, 1, true); ghostGeoFoot.translate(0, -0.5, 0);

    // ---- post
    const rt = new T.WebGLRenderTarget(cw, ch, { type: T.HalfFloatType, samples: 4 });
    composer = new T.EffectComposer(renderer, rt);
    composer.addPass(new T.RenderPass(scene, camMain));
    bloom = new T.UnrealBloomPass(new T.Vector2(cw, ch), 0.55, 0.45, 0.82);
    composer.addPass(bloom);
    composer.addPass(new T.OutputPass());

    st.tgt = new T.Vector3();
    st.mouthWorld = new T.Vector3();
    for (const k in vTmp) vTmp[k] = new T.Vector3();
    st.ready = true;
    bindPointer(canvas);
  }

  function resize(w, h, pixelRatio) {
    cw = w; ch = h; pr = pixelRatio;
    renderer.setPixelRatio(pr);
    renderer.setSize(w, h, false);
    composer.setPixelRatio(pr);
    composer.setSize(w, h);
    camMain.aspect = w / h; camMain.updateProjectionMatrix();
    layoutPip();
    camPip.aspect = st.pip.w / st.pip.h; camPip.updateProjectionMatrix();
  }

  /* mouth camera inset: lower right, above the view controls (or above the replay strip) */
  function layoutPip() {
    const bottom = Math.min(st.pipBottom || ch - 42, ch - 42);
    st.pip = { x: cw - 262, y: bottom - 188, w: 250, h: 188 };
  }

  function bindPointer(canvas) {
    let drag = false, lx = 0;
    canvas.addEventListener('pointerdown', (e) => { drag = true; lx = e.clientX; canvas.setPointerCapture(e.pointerId); });
    canvas.addEventListener('pointermove', (e) => {
      if (!drag) return;
      const s = OKL.stageScale || 1;
      st.rotY += (e.clientX - lx) / s * 0.008; lx = e.clientX;
      st.pauseUntil = performance.now() + 5000;
    });
    const up = () => { drag = false; };
    canvas.addEventListener('pointerup', up);
    canvas.addEventListener('pointercancel', up);
    canvas.addEventListener('wheel', (e) => {
      e.preventDefault();
      st.zoom = Math.min(4, Math.max(0.5, st.zoom * Math.exp(-e.deltaY * 0.0012)));
    }, { passive: false });
    canvas.addEventListener('dblclick', () => { st.zoom = 1; });
  }

  /* ---------------------------------------------------------------- geometry */
  function setGeometry(gg) {
    const key = [gg.d, gg.L, gg.W, gg.H, gg.footLen, gg.toe, gg.morph, gg.h].map((v) => v.toPrecision(5)).join(',');
    g = gg;
    if (key === gKey) return;
    gKey = key;
    geoInfo = buildBody(gg);
    const I = geoInfo;
    buildLanguid(gg, I);
    buildIris(gg, I);
    rim.position.set(0, gg.L, 0); rim.scale.setScalar(I.r);
    toeRim.position.set(0, -gg.footLen, 0); toeRim.scale.setScalar(I.rt);
    const e = Math.max(0.01 * I.r, 0.00025);
    labEdge.position.set(0, gg.W, I.zc); labEdge.scale.set(2 * I.half, e, e);
    const zf = I.zc + 0.004 * I.r;
    mouthFrame.geometry.setAttribute('position', new T.Float32BufferAttribute([-I.half, 0, zf, I.half, 0, zf, I.half, gg.W, zf, -I.half, gg.W, zf], 3));
    mouthFrame.geometry.computeBoundingSphere();
    topRing.position.set(0, gg.L + 0.002 * I.r, 0); topRing.scale.setScalar(I.r * 1.04);
    haze.scale.set(0.93 * I.r, gg.L, 0.93 * I.r);
    flueGlow.position.set(0, 0.0004 * I.r, I.zc - I.hv / 2); flueGlow.scale.set(2 * I.half * 0.98, e * 0.6, I.hv);
    glow.scale.set(0.86 * I.r, gg.L, 0.86 * I.r);
  }

  /* ---------------------------------------------------------------- ghosts */
  function setGhosts(list) {
    while (ghosts.length < list.length) {
      const mat = new T.ShaderMaterial({ vertexShader: GHOST_VS, fragmentShader: GHOST_FS, transparent: true, depthWrite: false, blending: T.AdditiveBlending, side: T.DoubleSide,
        uniforms: { uC: { value: new T.Color(0.25, 0.85, 1.1) }, uA: { value: 0.5 } } });
      const grp = new T.Group();
      grp.add(new T.Mesh(ghostGeoBody, mat), new T.Mesh(ghostGeoFoot, mat));
      grp.userData.mat = mat;
      ghostG.add(grp);
      ghosts.push(grp);
    }
    if (!g) return;
    const hMain = g.footLen + g.L;
    let x = g.d * 2.2 + 0.05 * hMain;
    for (let i = 0; i < ghosts.length; i++) {
      const gr = ghosts[i];
      const it = list[i];
      gr.visible = !!it;
      if (!it) continue;
      const r = it.geom.d / 2;
      x += r;
      // toe on the floor (all feet stand on the same chest), leaning like the main pipe
      gr.position.set(x, -g.footLen, -0.2 * hMain - 0.05);
      gr.rotation.set(TILT_X, 0, TILT_Z);
      x += r + Math.max(0.6 * it.geom.d, 0.03 * hMain);
      gr.children[0].scale.set(r, it.geom.L, r);
      gr.children[0].position.y = it.geom.footLen;
      gr.children[1].scale.set(r, it.geom.footLen, r);
      gr.children[1].position.y = it.geom.footLen;
      gr.userData.mat.uniforms.uA.value = 0.18 + 0.55 * Math.min(1, it.level);
      gr.updateMatrixWorld(true);
      const tp = new T.Vector3(0, it.geom.footLen + it.geom.L, 0).applyMatrix4(gr.matrixWorld);
      gr.userData.top = { x: tp.x, y: tp.y, z: tp.z };
      gr.userData.label = it.label;
    }
  }

  /* ---------------------------------------------------------------- per-frame physics */
  const tmpC = [0, 0, 0];
  function update(dt, vm, G) {
    if (!st.ready || !g) return;
    st.time += dt;
    const I = geoInfo;
    // rotation
    if (st.autoRotate && performance.now() > st.pauseUntil) st.rotY += dt * 2 * Math.PI / SPIN_PERIOD;
    pipeG.rotation.y = st.rotY;                 // spin about the pipe's own (leaning) axis
    tiltG.position.set(0, -g.footLen, 0);       // pivot at the toe
    pipeG.position.set(0, g.footLen, 0);
    bodyMat.userData.uXray.value = st.xray;

    // ---- standing-wave texture (64 samples along the bore)
    const on = vm.on;
    glowMat.uniforms.uOn.value = on;
    glowMat.uniforms.uIdle.value = 1 - on;
    if (vm.pInst) {
      const n = vm.pInst.length, pm = Math.max(1e-6, vm.pmax);
      for (let i = 0; i < 64; i++) {
        const xf = i / 63 * (n - 1), i0 = Math.floor(xf), i1 = Math.min(n - 1, i0 + 1), f = xf - i0;
        const p = (vm.pInst[i0] * (1 - f) + vm.pInst[i1] * f) / pm;
        const e = (vm.env[i0] * (1 - f) + vm.env[i1] * f) / pm;
        glowData[i * 4] = Math.round(Math.min(1, Math.max(0, 0.5 + 0.5 * p)) * 255);
        glowData[i * 4 + 1] = Math.round(Math.min(1, Math.max(0, e)) * 255);
      }
    } else {
      for (let i = 0; i < 64; i++) { glowData[i * 4] = 128; glowData[i * 4 + 1] = 0; }
    }
    glowTex.needsUpdate = true;

    // ---- jet sheet
    OKL.gasColor(G.gas, G.tempC, tmpC);
    jetMat.uniforms.uC.value.setRGB(tmpC[0] * 2.0, tmpC[1] * 2.0, tmpC[2] * 2.0);
    jetMat.uniforms.uI.value = 0.9 * vm.jetOn;
    const jp = jetGeo.attributes.position.array, cols = 7, rows = 49;
    for (let rI = 0; rI < rows; rI++) {
      const yn = rI / (rows - 1);                  // PlaneGeometry rows go top -> bottom
      const y = g.W * (1 - yn);
      const z = I.zc + g.y0 - vm.etaAt(1 - yn);
      for (let c = 0; c < cols; c++) {
        const k = (rI * cols + c) * 3;
        jp[k] = (c / (cols - 1) - 0.5) * 2 * I.half * 0.97;
        jp[k + 1] = y;
        jp[k + 2] = z;
      }
    }
    jetGeo.attributes.position.needsUpdate = true;
    flueGlow.material.opacity = 0.25 + 0.75 * vm.jetOn;

    // ---- parameter effects (the pipe length only follows scale and length glide)
    const kp = Math.exp(-dt / 0.6);
    for (const k in st.pulse) st.pulse[k] *= kp;
    // cut-up / labium: the mouth window is outlined while it moves
    mouthFrame.material.opacity = Math.min(1, 1.2 * st.pulse.mouth);
    labEdge.material.color.setRGB(0.6 + 2.2 * st.pulse.mouth, 2.6 - 0.6 * st.pulse.mouth, 3.4 - 2.6 * st.pulse.mouth);
    // open <-> stopped: the top rim and the iris light up while it closes or opens
    topRing.material.opacity = Math.min(1, 1.2 * st.pulse.top);
    irisMat.emissive.setRGB(0.9, 0.5, 0.12).multiplyScalar(0.5 * st.pulse.top);
    // gas / temperature: tinted haze in the bore, warm or cold tint on the metal
    const gasAmt = Math.min(1, Math.abs(G.gas));
    const heat = Math.min(1, Math.max(0, (G.tempC - 20) / 60)), cold = Math.min(1, Math.max(0, (20 - G.tempC) / 50));
    hazeMat.uniforms.uC.value.setRGB(tmpC[0] * 1.6, tmpC[1] * 1.6, tmpC[2] * 1.6);
    hazeMat.uniforms.uA.value = 0.17 * Math.max(gasAmt, heat, cold) + 0.15 * st.pulse.atmos;
    hazeMat.uniforms.uT.value = st.time;
    haze.visible = hazeMat.uniforms.uA.value > 0.003;
    bodyMat.emissive.setRGB(0.07 * heat + 0.008 * cold, 0.024 * heat + 0.022 * cold, 0.005 * heat + 0.055 * cold);

    // ---- side hole
    const sh = G.sideHole;
    holeG.visible = sh > 0.02;
    let yHole = 0, rHole = 0;
    if (holeG.visible) {
      const y0 = g.W + I.A * 1.2, y1 = g.L - 1.2 * I.r;
      yHole = y0 + (Math.max(y0, y1) - y0) * sh;
      rHole = 0.24 * I.r;
      holeG.position.set(I.r * 1.002, yHole, 0);
      holeG.scale.setScalar(rHole);
    }

    updateParticles(dt, vm, G, I, yHole, rHole);

    // ---- MALLET: at the wall when it struck, then back off (the stroke itself lasts under a millisecond)
    const M = st.mallet;
    malletG.visible = !!M.on; hitRing.visible = !!M.on;
    if (M.on) {
      if (M.strikes !== M.seen) { M.seen = M.strikes; M.hitT = 0; }
      M.hitT += dt;
      const r = I.r, hr = M.headR, yS = Math.min(g.L, Math.max(0, M.y));
      const rest = Math.max(4 * hr, 2.5 * r);
      const gap = M.hitT < 0.05 ? 0 : rest * (1 - Math.exp(-(M.hitT - 0.05) / 0.12));
      const z = r + hr + gap;
      malletHead.position.set(0, yS, z); malletHead.scale.setScalar(hr);
      malletHeadMat.color.setHex(M.color);
      malletHeadMat.metalness = M.metal;
      const len = Math.max(0.12, 10 * hr), sr = Math.max(0.0025, 0.12 * hr);
      malletStick.scale.set(sr, len, sr);
      malletStick.position.set(0, yS + MDIR.y * (hr + len / 2), z + MDIR.z * (hr + len / 2));
      malletStick.quaternion.setFromUnitVectors(MUP, MDIR);
      const fl = Math.max(0, 1 - M.hitT / 0.5);
      hitRing.material.opacity = 0.9 * fl;
      hitRing.position.set(0, yS, r * 1.003);
      hitRing.scale.setScalar(r * (0.15 + 1.6 * (1 - fl)));
    }

    // ---- floor & holo pad
    const hTot = g.footLen + g.L;
    floor.position.set(0, -g.footLen - 0.0005 * hTot, 0);
    floor.scale.set(hTot * 8, 1, hTot * 8);
    floorMat.uniforms.uCell.value = niceStep(hTot / 12);
    floorMat.uniforms.uFade.value = hTot * 1.6;
    floorMat.uniforms.uR.value = I.r;
    floorMat.uniforms.uTime.value = st.time;

    updateCameras(dt);
  }

  function niceStep(x) {
    const p = Math.pow(10, Math.floor(Math.log10(x))), m = x / p;
    return (m < 1.5 ? 1 : m < 3.5 ? 2 : m < 7.5 ? 5 : 10) * p;
  }

  function updateParticles(dt, vm, G, I, yHole, rHole) {
    const pos = ptsGeo.attributes.position.array, col = ptsGeo.attributes.aColor.array;
    const siz = ptsGeo.attributes.aSize.array, alp = ptsGeo.attributes.aAlpha.array;
    const c = OKL.gasColor(G.gas, G.tempC, tmpC);
    const W = g.W, half = I.half, zc = I.zc, r = I.r;
    const turb = G.noise / 0.008;
    // slow motion: one period of the sound lasts vm.strobe seconds on screen
    const simPerWall = vm.f > 0 ? 1 / (vm.f * vm.strobe) : 0;
    const vJet = Math.min(vm.Uj * simPerWall, W * 6);     // m per wall second (capped for readability)
    const wind = vm.jetOn;
    const b = Math.max(1e-5, vm.b);

    // ---- jet particles
    for (let i = 0; i < NJ; i++) {
      const k = i * 3;
      if (P.age[i] >= 1 || wind < 0.02) {
        if (wind < 0.02) { alp[i] = 0; P.age[i] = 1; pos[k] = pos[k + 1] = pos[k + 2] = 0; if (rnd() > 0.02) continue; }
        P.age[i] = 0; P.br[i] = 0;
        P.x[i] = (rnd() * 2 - 1) * half * 0.95; P.y[i] = rnd() * W * 0.05; P.life[i] = 1;
        P.seed[i] = rnd();
      }
      let x = P.x[i], y = P.y[i], z;
      const j = (P.seed[i] - 0.5) * b * 2.2 * (0.6 + 0.4 * turb) + (rnd() - 0.5) * b * 0.8 * turb;
      if (P.br[i] === 0) {
        y += vJet * dt * (0.85 + 0.3 * P.seed[i]);
        const yn = Math.min(1, y / W);
        z = zc + g.y0 - vm.etaAt(yn) + j;
        if (y >= W) {
          const rel = (vm.etaAt(1) - g.y0 - j) / b;
          const pin = 0.5 * (1 + Math.tanh(rel));
          P.br[i] = rnd() < pin ? 1 : -1;
          const sp = vJet * 0.75;
          P.vx[i] = (rnd() - 0.5) * sp * 0.2;
          P.vy[i] = sp * (0.55 + 0.2 * rnd());
          P.vz[i] = P.br[i] * -sp * (0.55 + 0.3 * rnd());
          P.age[i] = 0.001; P.z[i] = z;
        }
        P.y[i] = y;
        pos[k] = x; pos[k + 1] = Math.min(y, W); pos[k + 2] = z;
        alp[i] = 0.75 * wind;
      } else {
        P.age[i] += dt * 1.6 * (vJet / Math.max(W, 1e-6)) * 0.12 + dt * 0.5;
        P.vy[i] *= 0.985; P.vz[i] *= 0.97;
        if (P.br[i] < 0) { P.vz[i] += vJet * 0.05 * dt * 10; P.vy[i] += vJet * 0.02 * dt * 10; }
        P.x[i] += P.vx[i] * dt; P.y[i] += P.vy[i] * dt; P.z[i] += P.vz[i] * dt;
        if (P.br[i] > 0 && P.z[i] < zc - 1.6 * r) P.age[i] = 1;
        pos[k] = P.x[i]; pos[k + 1] = P.y[i]; pos[k + 2] = P.z[i];
        alp[i] = 0.7 * wind * Math.max(0, 1 - P.age[i]);
      }
      col[k] = c[0]; col[k + 1] = c[1]; col[k + 2] = c[2];
      if (P.br[i] > 0) { col[k] = c[0] * 0.7 + 0.3; col[k + 1] = c[1] * 0.9 + 0.1; col[k + 2] = c[2]; }
      siz[i] = Math.max(b * 3.2, 0.007 * r);
    }

    // ---- foot particles (toe hole -> flueway)
    const footVis = Math.min(1, wind * 1.2);
    for (let q = 0; q < NF; q++) {
      const i = NJ + q, k = i * 3;
      P.u[i] += dt * (0.18 + 0.5 * wind) * (0.7 + 0.6 * P.seed[i]);
      if (P.u[i] >= 1) { P.u[i] -= 1; P.seed[i] = rnd(); P.rx[i] = (rnd() * 2 - 1); P.rz[i] = rnd() * 2 - 1; }
      const u = P.u[i];
      const y = -g.footLen * (1 - u);
      const rr = r + (I.rt - r) * (1 - u);
      const conv = Math.pow(u, 3);
      const x0 = P.rx[i] * rr * 0.7, z0 = P.rz[i] * rr * 0.7;
      const x1 = P.rx[i] * half * 0.9, z1 = zc - I.hv * 0.5;
      pos[k] = x0 + (x1 - x0) * conv; pos[k + 1] = y; pos[k + 2] = z0 + (z1 - z0) * conv;
      col[k] = c[0]; col[k + 1] = c[1]; col[k + 2] = c[2];
      alp[i] = 0.32 * footVis * Math.min(1, u * 5) * (P.seed[i] < 0.25 + 0.75 * wind ? 1 : 0);
      siz[i] = 0.022 * r;
    }

    // ---- bore parcels (displacement exaggerated, colour = pressure)
    const hasP = !!vm.pInst;
    const nx = hasP ? vm.pInst.length : 0, pm = Math.max(1e-6, vm.pmax);
    const amp = 0.04 * g.L;
    for (let q = 0; q < NB; q++) {
      const i = NJ + NF + q, k = i * 3;
      const yr = P.ry[i];
      let p = 0, xi = 0;
      if (hasP) {
        const xf = yr * (nx - 1), i0 = Math.floor(xf), i1 = Math.min(nx - 1, i0 + 1), f = xf - i0;
        p = (vm.pInst[i0] * (1 - f) + vm.pInst[i1] * f) / pm;
        if (vm.xi) xi = vm.xi[i0] * (1 - f) + vm.xi[i1] * f;
      }
      pos[k] = P.rx[i] * r; pos[k + 1] = yr * g.L + xi * amp * vm.on; pos[k + 2] = P.rz[i] * r;
      const ap = Math.abs(p) * vm.on;
      if (p >= 0) { col[k] = 1.0; col[k + 1] = 0.62; col[k + 2] = 0.18; } else { col[k] = 0.25; col[k + 1] = 0.55; col[k + 2] = 1.0; }
      col[k] = col[k] * ap + c[0] * 0.35 * (1 - ap);
      col[k + 1] = col[k + 1] * ap + c[1] * 0.35 * (1 - ap);
      col[k + 2] = col[k + 2] * ap + c[2] * 0.35 * (1 - ap);
      alp[i] = 0.08 + 0.75 * ap + 0.12 * vm.on;
      siz[i] = 0.05 * r;
    }

    // ---- leak from the side hole
    for (let q = 0; q < NL; q++) {
      const i = NJ + NF + NB + q, k = i * 3;
      if (rHole <= 0) { alp[i] = 0; continue; }
      P.age[i] += dt * (0.6 + 1.6 * vm.on);
      if (P.age[i] >= 1) { P.age[i] = 0; P.seed[i] = rnd(); P.vx[i] = (rnd() - 0.5) * 2; P.vz[i] = (rnd() - 0.5) * 2; }
      const a = P.age[i];
      const reach = r * (0.3 + 2.2 * vm.on);
      pos[k] = r + a * reach;
      pos[k + 1] = yHole + P.vx[i] * rHole * (0.6 + a * 2);
      pos[k + 2] = P.vz[i] * rHole * (0.6 + a * 2);
      col[k] = 1.0; col[k + 1] = 0.72; col[k + 2] = 0.3;
      alp[i] = (1 - a) * 0.8 * Math.max(0.15, vm.on);
      siz[i] = 0.05 * r;
    }

    ptsGeo.attributes.position.needsUpdate = true;
    ptsGeo.attributes.aColor.needsUpdate = true;
    ptsGeo.attributes.aSize.needsUpdate = true;
    ptsGeo.attributes.aAlpha.needsUpdate = true;
  }

  /* ---------------------------------------------------------------- cameras */
  const vTmp = { a: null, b: null, c: null, d: null, e: null };
  function updateCameras(dt) {
    const k = 1 - Math.exp(-dt * 4.5);
    const hTot = g.footLen + g.L;
    const tanH = Math.tan(T.MathUtils.degToRad(camMain.fov / 2));
    const I = geoInfo;
    tiltG.updateMatrixWorld(true);
    // the leaning axis in world space: toe (pivot) and top
    const A = vTmp.c.set(0, 0, 0).applyMatrix4(tiltG.matrixWorld);
    const B = vTmp.d.set(0, hTot, 0).applyMatrix4(tiltG.matrixWorld);
    const axis = vTmp.e.copy(B).sub(A).normalize();
    const cX = (A.x + B.x) / 2, cY = (A.y + B.y) / 2, cZ = (A.z + B.z) / 2;
    const spanV = Math.abs(B.y - A.y) + 2 * I.r * Math.abs(axis.x);
    const spanH = Math.abs(B.x - A.x) + 2 * I.r;
    // full view rig: fit the leaning pipe, centred a little left of the middle (HUD room on the right)
    const Df = Math.max((spanV * 1.16 / 2) / tanH, (spanH / 2) / (tanH * camMain.aspect * 0.5)) / st.zoom;
    st.camD += (Df - st.camD) * k;
    const vw = 2 * st.camD * tanH * camMain.aspect;
    // mouth rig (world position of the mouth centre)
    const mw = st.mouthWorld.set(0, g.W * 0.55, I.zc).applyMatrix4(pipeG.matrixWorld);
    const fitM = Math.max(g.W * 2.0, 1.35 * I.half);
    const Dm = fitM / Math.tan(T.MathUtils.degToRad(15)) / (st.view === 'mouth' ? st.zoom : 1);
    st.camDM += (Dm - st.camDM) * k;
    const dir = vTmp.b.set(Math.sin(1.0), 0.32, Math.cos(1.0)).normalize().transformDirection(pipeG.matrixWorld);

    const full = st.view === 'full' ? camMain : camPip;
    const mouth = st.view === 'full' ? camPip : camMain;
    const txF = st.view === 'full' ? cX + 0.07 * vw : cX;
    st.tgt.x += (txF - st.tgt.x) * k; st.tgt.y += (cY - st.tgt.y) * k; st.tgt.z += (cZ - st.tgt.z) * k;
    const tanF = Math.tan(T.MathUtils.degToRad(full.fov / 2));
    const Dfull = st.view === 'full' ? st.camD : Math.max((spanV * 1.12 / 2) / tanF, (spanH / 2) / (tanF * full.aspect * 0.8));
    full.up.set(0, 1, 0);
    full.position.set(st.tgt.x, st.tgt.y + 0.09 * Dfull, st.tgt.z + Dfull);
    full.lookAt(st.tgt.x, st.tgt.y, st.tgt.z);
    full.near = Dfull / 200; full.far = Dfull * 20; full.updateProjectionMatrix();

    mouth.up.copy(axis);                        // keep the mouth upright in its view
    mouth.position.copy(mw).addScaledVector(dir, st.camDM);
    mouth.lookAt(mw);
    mouth.near = st.camDM / 300; mouth.far = Math.max(st.camDM * 40, hTot * 6); mouth.updateProjectionMatrix();
  }

  /* ---------------------------------------------------------------- render */
  function pointScale(cam, hpx) { return hpx * pr / (2 * Math.tan(T.MathUtils.degToRad(cam.fov / 2))); }

  function render() {
    if (!st.ready || !g) return;
    ptsMat.uniforms.uScale.value = pointScale(camMain, ch);
    composer.render();
    // picture-in-picture
    const p = st.pip;
    const y = ch - p.y - p.h;
    renderer.setScissorTest(true);
    renderer.setViewport(p.x, y, p.w, p.h);
    renderer.setScissor(p.x, y, p.w, p.h);
    renderer.setClearColor(0x02060a, 1);
    renderer.clear(true, true, false);
    ptsMat.uniforms.uScale.value = pointScale(camPip, p.h);
    const fv = floor.visible; floor.visible = st.view !== 'full';
    renderer.render(scene, camPip);
    floor.visible = fv;
    renderer.setScissorTest(false);
    renderer.setViewport(0, 0, cw, ch);
    renderer.setClearColor(0x050b12, 1);
  }

  /* ---------------------------------------------------------------- projection for the HUD */
  const pv = { x: 0, y: 0, z: 0 };
  function projectLocal(x, y, z, out) {
    const v = vTmp.a.set(x, y, z).applyMatrix4(pipeG.matrixWorld).project(camMain);
    out = out || {};
    out.x = (v.x + 1) / 2 * cw; out.y = (1 - v.y) / 2 * ch; out.z = v.z;
    return out;
  }
  /* a point in the leaning, non-spinning pipe frame (y = 0 at the languid) */
  function projectTilt(x, y, z, out) {
    const v = vTmp.a.set(x, y + (g ? g.footLen : 0), z).applyMatrix4(tiltG.matrixWorld).project(camMain);
    out = out || {};
    out.x = (v.x + 1) / 2 * cw; out.y = (1 - v.y) / 2 * ch; out.z = v.z;
    return out;
  }
  function projectWorld(x, y, z, out) {
    const v = vTmp.a.set(x, y, z).project(camMain);
    out = out || {};
    out.x = (v.x + 1) / 2 * cw; out.y = (1 - v.y) / 2 * ch; out.z = v.z;
    return out;
  }
  /* is the mouth facing the camera? (cos of the angle) */
  function mouthFacing() {
    const n = vTmp.b.set(0, 0, 1).transformDirection(pipeG.matrixWorld);
    const toCam = vTmp.a.copy(camMain.position).sub(st.mouthWorld).normalize();
    return n.dot(toCam);
  }

  return {
    init, resize, setGeometry, setGhosts, update, render, projectLocal, projectTilt, projectWorld, mouthFacing,
    setPipBottom(px) { st.pipBottom = px; layoutPip(); },
    get state() { return st; }, get info() { return geoInfo; }, get geom() { return g; }, ghosts,
    pulse(name) { if (st.pulse[name] !== undefined) st.pulse[name] = 1; },
    setXray(v) { st.xray = v; }, setAutoRotate(on) { st.autoRotate = on; }, setView(v) { st.view = v; },
    setMallet(m) { Object.assign(st.mallet, m); },
  };
})();
