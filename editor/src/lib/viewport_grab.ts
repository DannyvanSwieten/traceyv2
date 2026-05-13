// Modal grab/rotate/scale state machine. Blender-style ergonomics:
//   G  → enter Grab (translate) on the selected actor
//   R  → enter Rotate
//   S  → enter Scale
//   X / Y / Z → constrain to that axis (toggle off by re-pressing same)
//   Enter / left-click  → commit the current transform
//   Esc / right-click   → cancel, revert to the original transform
//
// Mouse delta drives the operation. Screen-space → world-space conversion is
// a coarse approximation (no view-projection plumbing yet): one screen pixel
// ≈ camera_distance / focal_length world units, with the active axis as the
// world direction. Good enough for blocking-out; precise gizmos can come
// when we wire the native camera matrices through.

import { createSignal } from 'solid-js';
import * as api from './api';

export type GrabKind = 'translate' | 'rotate' | 'scale';
export type Axis = 'x' | 'y' | 'z' | null;

export interface GrabState {
  kind: GrabKind;
  axis: Axis;
  actorId: number;
  startX: number;
  startY: number;
  origin: api.Transform;
  // Cached at grab-start so per-frame math doesn't re-fetch the camera.
  camera: api.Camera;
}

const [active, setActive] = createSignal<GrabState | null>(null);
export const grabState = active;
export const isGrabActive = () => active() !== null;

// Sensitivity in "world-units per screen-pixel" for translate. Conservative
// default; the user can hold Shift for precision (multiplier in the
// pointermove handler) when that lands.
const TRANSLATE_SENSITIVITY = 0.01;
const ROTATE_SENSITIVITY = 0.005;  // radians per pixel
const SCALE_SENSITIVITY = 0.005;   // (1 + dx * sens) per pixel

function quatToYawPitchRoll(q: api.Quat): { y: number; p: number; r: number } {
  // ZYX intrinsic (yaw=Z, pitch=Y, roll=X). Sufficient for the camera
  // basis we use elsewhere — not the right convention for general
  // actor rotation, but the grab path operates on world-axis deltas
  // and doesn't need to recover Euler angles.
  void q;
  return { y: 0, p: 0, r: 0 };
}

function quatMul(a: api.Quat, b: api.Quat): api.Quat {
  return {
    w: a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    x: a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
    y: a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
    z: a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
  };
}

function axisAngleQuat(ax: number, ay: number, az: number, angle: number): api.Quat {
  const half = angle * 0.5;
  const s = Math.sin(half);
  return { w: Math.cos(half), x: ax * s, y: ay * s, z: az * s };
}

// Distance from the camera to the actor, used to scale translate delta so
// dragging the same pixel distance moves the same fraction of the viewport
// regardless of dolly position. Falls back to 1 when the camera is bogus.
function cameraDistance(actor: api.Vec3, cam: api.Camera): number {
  const dx = actor.x - cam.position.x;
  const dy = actor.y - cam.position.y;
  const dz = actor.z - cam.position.z;
  const d = Math.sqrt(dx * dx + dy * dy + dz * dz);
  return d > 0.01 ? d : 1.0;
}

// Begin a new modal operation. Captures the actor's original transform so
// Esc can revert. Calls api.getCamera() async — the operation is "armed"
// the moment this returns; the first pointermove finalises it.
export async function startGrab(
  kind: GrabKind,
  actorId: number,
  transform: api.Transform,
  pointerX: number,
  pointerY: number,
): Promise<void> {
  const camera = await api.getCamera();
  setActive({
    kind, axis: null, actorId,
    startX: pointerX, startY: pointerY,
    origin: structuredClone(transform),
    camera,
  });
}

// Per-pointermove update. Returns the new transform to apply (caller writes
// it through api.setActorTransform).
export function updateGrab(pointerX: number, pointerY: number): api.Transform | null {
  const g = active();
  if (!g) return null;
  const dx = pointerX - g.startX;
  const dy = pointerY - g.startY;

  switch (g.kind) {
    case 'translate': {
      const dist = cameraDistance(g.origin.position, g.camera);
      const scale = TRANSLATE_SENSITIVITY * dist;
      // Screen-x → +world-axis; screen-y → -world-axis (mouse up = +Y world).
      const out = structuredClone(g.origin);
      if (g.axis === 'x' || g.axis === null) out.position.x += dx * scale;
      if (g.axis === 'y' || g.axis === null) out.position.y -= dy * scale;
      if (g.axis === 'z') out.position.z += dx * scale;
      return out;
    }
    case 'rotate': {
      const angle = dx * ROTATE_SENSITIVITY;
      let ax = 0, ay = 1, az = 0; // default: yaw around world Y
      if (g.axis === 'x') { ax = 1; ay = 0; az = 0; }
      else if (g.axis === 'z') { ax = 0; ay = 0; az = 1; }
      const deltaQ = axisAngleQuat(ax, ay, az, angle);
      const out = structuredClone(g.origin);
      out.rotation = quatMul(deltaQ, g.origin.rotation);
      return out;
    }
    case 'scale': {
      const factor = Math.max(0.01, 1 + dx * SCALE_SENSITIVITY);
      const out = structuredClone(g.origin);
      if (g.axis === null) {
        out.scale.x = g.origin.scale.x * factor;
        out.scale.y = g.origin.scale.y * factor;
        out.scale.z = g.origin.scale.z * factor;
      } else if (g.axis === 'x') out.scale.x = g.origin.scale.x * factor;
      else if (g.axis === 'y')   out.scale.y = g.origin.scale.y * factor;
      else if (g.axis === 'z')   out.scale.z = g.origin.scale.z * factor;
      return out;
    }
  }
  // Unreachable; exhaustiveness silenced.
  void quatToYawPitchRoll;
  return null;
}

// Reset the grab's anchor pointer position. Called once after the native
// pointer broadcast starts streaming so the coord system in startX/Y
// matches the per-tick (mouse_x, mouse_y) values from the engine — they
// don't agree with browser clientX/Y because the Metal viewport has its
// own local-pixel coord space.
export function setOrigin(x: number, y: number): void {
  setActive((g) => g ? { ...g, startX: x, startY: y } : g);
}

export function setAxis(axis: Axis): void {
  setActive((g) => {
    if (!g) return g;
    // Re-pressing the same axis clears the constraint.
    return { ...g, axis: g.axis === axis ? null : axis };
  });
}

export function commitGrab(): GrabState | null {
  const g = active();
  setActive(null);
  return g;
}

export async function cancelGrab(): Promise<void> {
  const g = active();
  setActive(null);
  if (g) {
    try { await api.setActorTransform(g.actorId, g.origin); }
    catch (e) { console.error('grab revert failed:', e); }
  }
}
