/**
 * Lightweight kinematic sketch of the Mini Pupper for the calibration view.
 *
 * This is NOT a mesh viewer. It is a stick-figure forward-kinematics model
 * plus an orthographic projection, small enough to live in the firmware's
 * flash next to the rest of the PWA. Everything is in millimetres in the
 * body frame: x forward, y to the robot's LEFT, z up.
 *
 * Geometry is lifted from the simulator model
 * (mpx-sim/src/mpx_sim/model/urdf/sku_1_urdf_fixed.urdf and pupper.xml):
 *   - hip (abduction) joints at (±48.5, ±31.5, −0.6) mm, axis along body X
 *   - thigh pivot sits 12.5 mm outboard of the hip axis (+3.65 mm at the calf)
 *   - thigh 50 mm, shin ≈ 55 mm, both pitching about body Y
 * and from the firmware (robot.h / servo_map.json): centred servos == a
 * 70 mm stand with L1=50, L2=56, which puts the thigh 52.5° behind vertical
 * and the shin 45° ahead of it. Mini Pupper's knees all point backwards.
 */

/* ── Body (mm, half extents) ──────────────────────────────────────────
 * Kept a touch narrower than the hip spacing so the hip joints sit on the
 * flanks (as the servos do) — that also keeps the painter's sort honest:
 * a leg is either wholly in front of its side of the body or wholly behind. */
export const BODY = { hx: 70, hy: 30, top: 18, bottom: -14 };

/* ── Leg geometry (mm) ───────────────────────────────────────────── */
export const HIP_X = 48.5;
export const HIP_Y = 31.5;
export const HIP_Z = -0.6;
export const ABD_OUT = 16;   // hip axis → thigh plane
export const L1 = 50;        // thigh
export const L2 = 56;        // shin
export const TH1_NEUTRAL = 52.5; // thigh behind vertical, deg
export const TH2_NEUTRAL = 45;   // shin ahead of vertical, deg
export const FOOT_Z = HIP_Z - 70; // ground plane at the neutral stand

/* ── Legs ─────────────────────────────────────────────────────────────
 *   f: +1 front / −1 rear      s: +1 left / −1 right
 *   yaw: camera heading that puts this leg nearest the viewer. */
export const LEGS = [
  { id: "fr", label: "FR", name: "Front right", f:  1, s: -1, yaw:   42 },
  { id: "fl", label: "FL", name: "Front left",  f:  1, s:  1, yaw:  -42 },
  { id: "rr", label: "RR", name: "Rear right",  f: -1, s: -1, yaw:  138 },
  { id: "rl", label: "RL", name: "Rear left",   f: -1, s:  1, yaw: -138 },
];

/* Servo-positive → joint-positive sign per (joint, leg).
 *
 * Mirrors SK_SIGN in main/robot/stanford_kinematics.cc: hip/knee servos are
 * mirrored left↔right (output shaft faces outboard), abduction servos are
 * mirrored front↔rear. Joint-positive here means: abduction swings the foot
 * towards the robot's LEFT, hip/knee swing the segment BACKWARDS. If a servo
 * on the real robot turns the other way, flip its entry here — nothing else
 * depends on it. */
export const SIGN = {
  abd:  { fr: -1, fl: -1, rr:  1, rl:  1 },
  hip:  { fr:  1, fl: -1, rr:  1, rl: -1 },
  knee: { fr:  1, fl: -1, rr:  1, rl: -1 },
};

const D2R = Math.PI / 180;

/**
 * Forward kinematics for one leg.
 * @param leg  entry of LEGS
 * @param ang  { abd, hip, knee } joint deltas from neutral, degrees (already signed)
 * @returns { H, T, K, F } 3-vectors: hip axis, thigh pivot, knee, foot
 */
export function legPoints(leg, ang) {
  const a = (ang.abd || 0) * D2R;
  const sa = Math.sin(a), ca = Math.cos(a);

  const H = [leg.f * HIP_X, leg.s * HIP_Y, HIP_Z];
  // leg-plane "down" and "outboard" after abduction about body X
  const dy = sa, dz = -ca;
  const T = [H[0], H[1] + ABD_OUT * leg.s * ca, H[2] + ABD_OUT * leg.s * sa];

  const seg = (from, len, thetaDeg) => {
    const t = thetaDeg * D2R, st = Math.sin(t), ct = Math.cos(t);
    // theta positive = backwards (−x), measured from leg-plane down
    return [from[0] - len * st, from[1] + len * ct * dy, from[2] + len * ct * dz];
  };

  const th1 = TH1_NEUTRAL + (ang.hip || 0);
  const th2 = -TH2_NEUTRAL + (ang.hip || 0) + (ang.knee || 0); // shin follows the thigh
  const K = seg(T, L1, th1);
  const F = seg(K, L2, th2);
  return { H, T, K, F };
}

/**
 * Orthographic camera. yaw 0 looks at the robot's face; positive yaw walks
 * the camera round to the robot's right. pitch is elevation above the floor.
 */
export function makeCamera(yawDeg, pitchDeg, scale, cx, cy, zCenter = -24) {
  const y = yawDeg * D2R, p = pitchDeg * D2R;
  const cyaw = Math.cos(y), syaw = Math.sin(y);
  const cp = Math.cos(p), sp = Math.sin(p);
  return {
    scale, cx, cy, zCenter, cp, sp,
    // unit vector (body frame) pointing at the camera — for face culling
    dir: [cyaw * cp, -syaw * cp, sp],
    /** body-frame point → { x, y } screen px + depth (bigger = nearer) */
    project(pt) {
      const x1 = pt[0] * cyaw - pt[1] * syaw;
      const y1 = pt[0] * syaw + pt[1] * cyaw;
      const z = pt[2] - zCenter;
      return {
        x: cx + scale * y1,
        y: cy - scale * (z * cp - x1 * sp),
        d: x1 * cp + z * sp,
      };
    },
  };
}

/* ── Body box: vertices + outward faces (CCW seen from outside) ────── */
const { hx, hy, top, bottom } = BODY;
export const BODY_VERTS = [
  [ hx,  hy, top], [ hx, -hy, top], [-hx, -hy, top], [-hx,  hy, top],
  [ hx,  hy, bottom], [ hx, -hy, bottom], [-hx, -hy, bottom], [-hx,  hy, bottom],
];
export const BODY_FACES = [
  { id: "top",    idx: [0, 1, 2, 3], n: [ 0,  0,  1] },
  { id: "bottom", idx: [4, 7, 6, 5], n: [ 0,  0, -1] },
  { id: "front",  idx: [0, 4, 5, 1], n: [ 1,  0,  0] },
  { id: "rear",   idx: [2, 6, 7, 3], n: [-1,  0,  0] },
  { id: "left",   idx: [0, 3, 7, 4], n: [ 0,  1,  0] },
  { id: "right",  idx: [1, 5, 6, 2], n: [ 0, -1,  0] },
];

/* Front "face" display: a dark screen with two eyes, drawn just proud of x = hx. */
const sx = hx + 0.6;
export const SCREEN = [[sx, 22, 13], [sx, -22, 13], [sx, -22, -8], [sx, 22, -8]];
export const EYES = [
  [[sx + 0.4,  15, 9], [sx + 0.4,   5, 9], [sx + 0.4,   5, -4], [sx + 0.4,  15, -4]],
  [[sx + 0.4,  -5, 9], [sx + 0.4, -15, 9], [sx + 0.4, -15, -4], [sx + 0.4,  -5, -4]],
];

export const dot3 = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
export const pts = (list) => list.map((p) => `${p.x.toFixed(1)},${p.y.toFixed(1)}`).join(" ");
