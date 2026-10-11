/**
 * pupper3d — the 3D Mini Pupper on the Calibration page.
 *
 * Built from the robot's own MuJoCo model (minipupperesp/sim) plus the head
 * from the SKU-1 URDF, decimated to ~12k triangles (public/pupper_model.json):
 * each body keeps its MuJoCo pose and hinge axis, so the legs bend exactly the
 * way the real joints do. `map` in the file turns a servo offset in degrees
 * into the joint angle it produces (the same mapping as pupper_map.py).
 *
 * This file is loaded only when Calibration opens, so three.js is not part of
 * the app's first download.
 */
import {
  WebGLRenderer, Scene, PerspectiveCamera, HemisphereLight, DirectionalLight,
  Mesh, CircleGeometry, MeshBasicMaterial, MeshStandardMaterial, Group,
  BufferGeometry, BufferAttribute, Vector3, Color, PlaneGeometry, Matrix4,
  CanvasTexture, Raycaster, Vector2,
} from "three";

const LEG_OF_UI = { fl: "FL", fr: "FR", rl: "RL", rr: "RR" };
const UI_OF_LEG = { FL: "fl", FR: "fr", RL: "rl", RR: "rr" };
const PART_OF_JOINT = { abd: "hip", hip: "thigh", knee: "calf" };
// Camera heading that puts each leg nearest the viewer (front is +x, left +y).
const LEG_HEADING = { fl: 0.65, fr: -0.65, rl: Math.PI - 0.65, rr: -(Math.PI - 0.65) };

function b64(s, T) {
  const bin = atob(s);
  const u = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) u[i] = bin.charCodeAt(i);
  return new T(u.buffer);
}

/**
 * @param {HTMLCanvasElement} canvas
 * @param {string} url  model JSON
 * @param {{ onPickLeg?: (leg: string) => void }} opts
 */
export async function createPupperView(canvas, url, { onPickLeg } = {}) {
  const data = await (await fetch(url)).json();

  const renderer = new WebGLRenderer({ canvas, antialias: true, alpha: false });
  renderer.setClearColor(0xffffff);
  renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
  const scene = new Scene();
  const cam = new PerspectiveCamera(30, 1, 0.01, 10);
  cam.up.set(0, 0, 1);
  scene.add(new HemisphereLight(0xffffff, 0x8a8478, 0.95));
  const key = new DirectionalLight(0xffffff, 0.75);
  key.position.set(0.4, -0.6, 1.0);
  scene.add(key);
  scene.add(new Mesh(new CircleGeometry(0.16, 48), new MeshBasicMaterial({ color: 0xf0ede6 })));

  const objs = {};
  const pickables = [];
  for (const b of data.bodies) {
    const g = new Group();
    g.position.set(b.pos[0], b.pos[1], b.pos[2]);
    g.quaternion.set(b.quat[1], b.quat[2], b.quat[3], b.quat[0]);
    const jg = new Group();
    g.add(jg);
    const mats = [];
    const leg = b.name.split("_")[0];
    for (const p of b.parts) {
      const q = b64(p.pos, Int16Array);
      const pos = new Float32Array(q.length);
      for (let i = 0; i < q.length; i++) { const a = i % 3; pos[i] = p.lo[a] + (q[i] + 32767) * p.scale[a]; }
      const geo = new BufferGeometry();
      geo.setAttribute("position", new BufferAttribute(pos, 3));
      geo.setIndex(new BufferAttribute(b64(p.idx, p.idx32 ? Uint32Array : Uint16Array), 1));
      geo.computeVertexNormals();
      const c = data.materials[p.mat];
      const m = new MeshStandardMaterial({ color: new Color(c[0], c[1], c[2]), roughness: 0.55, metalness: 0.1 });
      mats.push(m);
      const mesh = new Mesh(geo, m);
      if (UI_OF_LEG[leg]) { mesh.userData.leg = UI_OF_LEG[leg]; pickables.push(mesh); }
      jg.add(mesh);
    }
    objs[b.name] = { g, jg, axis: b.axis ? new Vector3(...b.axis).normalize() : null, mats };
    if (b.parent) objs[b.parent].jg.add(g); else scene.add(g);
  }
  objs.base.g.position.set(0, 0, 0.105);

  // The screen: Pupper's eyes drawn into a texture on the head.
  let screen = null;
  if (data.screen) {
    const S = data.screen;
    const cv = document.createElement("canvas");
    cv.width = 256; cv.height = 192;
    const tex = new CanvasTexture(cv);
    const plane = new Mesh(new PlaneGeometry(S.w, S.h), new MeshBasicMaterial({ map: tex }));
    const n = new Vector3(...S.normal), up = new Vector3(...S.up);
    const right = new Vector3().crossVectors(up, n);
    plane.quaternion.setFromRotationMatrix(new Matrix4().makeBasis(right, up, n));
    plane.position.set(...S.center);
    objs.base.jg.add(plane);
    screen = { ctx: cv.getContext("2d"), tex, gx: 0, gy: 0, key: "" };
  }

  // ── State the page drives ──
  let offsets = { fl: {}, fr: {}, rl: {}, rr: {} };
  let selected = { leg: "fl", joint: "hip" };
  let az = -0.75, el = 0.42, azGoal = az;
  let wiggleUntil = 0;

  function drawEyes(t) {
    if (!screen) return;
    const leg = selected.leg;
    const tx = leg === "fl" || leg === "rl" ? 1 : -1;
    const ty = leg === "fl" || leg === "fr" ? 0.6 : -0.4;
    screen.gx += (tx - screen.gx) * 0.12;
    screen.gy += (ty - screen.gy) * 0.12;
    const ph = t % 3.2;
    const lid = Math.max(0.08, ph < 0.09 ? 1 - ph / 0.09 : ph < 0.18 ? (ph - 0.09) / 0.09 : 1);
    const key = screen.gx.toFixed(3) + screen.gy.toFixed(3) + lid.toFixed(2);
    if (key === screen.key) return;
    screen.key = key;
    const c = screen.ctx, W = 256, H = 192;
    c.fillStyle = "#FFE605"; c.fillRect(0, 0, W, H);
    c.fillStyle = "#111111";
    const ew = 46, eh = 92 * lid, gap = 34;
    const cx = W / 2 + screen.gx * 26, cy = H / 2 + screen.gy * 18;
    for (const sx of [-1, 1]) {
      const x = cx + sx * (gap / 2 + ew / 2) - ew / 2, y = cy - eh / 2, r = Math.min(14, eh / 2);
      c.beginPath();
      c.moveTo(x + r, y); c.arcTo(x + ew, y, x + ew, y + eh, r); c.arcTo(x + ew, y + eh, x, y + eh, r);
      c.arcTo(x, y + eh, x, y, r); c.arcTo(x, y, x + ew, y, r); c.closePath(); c.fill();
    }
    screen.tex.needsUpdate = true;
  }

  function frame(t) {
    const wig = t < wiggleUntil ? Math.sin(t * 9) * 0.06 : 0;
    const glowOn = 0.32 + 0.22 * Math.sin(t * 6);
    for (const L of ["FR", "FL", "RR", "RL"]) {
      const M = data.map[L];
      const ui = UI_OF_LEG[L];
      const o = offsets[ui] || {};
      const oa = o.abd || 0, oh = o.hip || 0, ok = o.knee || 0;
      const sel = ui === selected.leg;
      const j = selected.joint;
      const q = {
        hip: data.stand[L + "_hip"] + M.hip * oa + (sel && j === "abd" ? wig : 0),
        thigh: data.stand[L + "_thigh"] + M.thigh * oh + (sel && j === "hip" ? wig : 0),
        calf: data.stand[L + "_calf"] + M.calf_from_knee * ok + M.calf_from_thigh * oh + (sel && j === "knee" ? wig : 0),
      };
      for (const part of ["hip", "thigh", "calf"]) {
        const ob = objs[L + "_" + part];
        if (ob.axis) ob.jg.setRotationFromAxisAngle(ob.axis, q[part]);
        const lit = sel && PART_OF_JOINT[j] === part;
        const glow = lit ? glowOn : 0;
        for (const m of ob.mats) m.emissive.setRGB(glow, glow * 0.45, 0);
      }
    }
    drawEyes(t);
    az += (azGoal - az) * 0.12;
    const R = 0.42;
    cam.position.set(R * Math.cos(el) * Math.cos(az), R * Math.cos(el) * Math.sin(az), 0.07 + R * Math.sin(el));
    cam.lookAt(0, 0, 0.07);
    renderer.render(scene, cam);
  }

  // ── Size, loop ──
  function resize() {
    const w = canvas.clientWidth || 1, h = canvas.clientHeight || 1;
    renderer.setSize(w, h, false);
    cam.aspect = w / h;
    cam.updateProjectionMatrix();
  }
  const ro = new ResizeObserver(resize);
  ro.observe(canvas);
  resize();

  let raf = 0, dead = false;
  const loop = (ms) => {
    if (dead) return;
    if (document.visibilityState === "visible") frame(ms / 1000);
    raf = requestAnimationFrame(loop);
  };
  raf = requestAnimationFrame(loop);

  // ── Orbit by dragging; a tap on a leg picks it ──
  let drag = null;
  const ray = new Raycaster();
  function down(e) {
    drag = { x: e.clientX, y: e.clientY, az: azGoal, el, moved: false };
    try { canvas.setPointerCapture(e.pointerId); } catch {}
  }
  function move(e) {
    if (!drag) return;
    const dx = e.clientX - drag.x, dy = e.clientY - drag.y;
    if (!drag.moved && Math.hypot(dx, dy) < 6) return;
    drag.moved = true;
    azGoal = az = drag.az - dx * 0.01;
    el = Math.max(0.05, Math.min(1.2, drag.el + dy * 0.01));
  }
  function up(e) {
    if (drag && !drag.moved && onPickLeg) {
      const r = canvas.getBoundingClientRect();
      ray.setFromCamera(new Vector2(((e.clientX - r.left) / r.width) * 2 - 1, -((e.clientY - r.top) / r.height) * 2 + 1), cam);
      const hit = ray.intersectObjects(pickables, false)[0];
      if (hit) onPickLeg(hit.object.userData.leg);
    }
    drag = null;
  }
  canvas.addEventListener("pointerdown", down);
  canvas.addEventListener("pointermove", move);
  canvas.addEventListener("pointerup", up);
  canvas.addEventListener("pointercancel", () => (drag = null));

  return {
    setOffsets(next) { offsets = next; },
    select(leg, joint, { swing = false } = {}) {
      const changedLeg = leg !== selected.leg;
      selected = { leg, joint };
      wiggleUntil = performance.now() / 1000 + 1.6;
      if (swing && changedLeg) {
        // shortest way round to the leg's heading
        const target = LEG_HEADING[leg];
        azGoal = az + ((((target - az) % (2 * Math.PI)) + 3 * Math.PI) % (2 * Math.PI) - Math.PI);
        el = 0.42;
      }
    },
    nudge() { wiggleUntil = 0; },
    dispose() {
      dead = true;
      cancelAnimationFrame(raf);
      ro.disconnect();
      canvas.removeEventListener("pointerdown", down);
      canvas.removeEventListener("pointermove", move);
      canvas.removeEventListener("pointerup", up);
      renderer.dispose();
    },
  };
}

export { LEG_OF_UI };
