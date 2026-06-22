import { Component, Accessor, For, createSignal, onMount } from 'solid-js';
import * as api from '../../lib/api';
import { NumberInput } from '../number-input/NumberInput';
import './CameraControls.css';

export interface CameraPosition {
  x: number;
  y: number;
  z: number;
}

interface CameraControlsProps {
  position: Accessor<CameraPosition>;
  onPositionChange: (pos: CameraPosition) => void;
}

// Standard DCC view presets. Pivot + zoom are preserved server-side so each
// button just changes the angle, not what's framed.
const VIEW_PRESETS: { label: string; view: api.CameraView; hint: string }[] = [
  { label: 'Top',     view: 'top',     hint: 'Bird’s-eye view (−Y forward)' },
  { label: 'Front',   view: 'front',   hint: 'Camera on +Z, looking −Z' },
  { label: 'Right',   view: 'right',   hint: 'Camera on +X, looking −X' },
  { label: 'Back',    view: 'back',    hint: 'Camera on −Z, looking +Z' },
  { label: 'Left',    view: 'left',    hint: 'Camera on −X, looking +X' },
  { label: 'Bottom',  view: 'bottom',  hint: 'Camera under the scene, looking up' },
  { label: 'Persp',   view: 'persp',   hint: '3/4 perspective onto origin' },
];

export const CameraControls: Component<CameraControlsProps> = (props) => {
  const setAxis = (axis: 'x' | 'y' | 'z', numValue: number) => {
    const newPos = { ...props.position() };
    newPos[axis] = numValue;
    props.onPositionChange(newPos);
  };

  // Thin-lens depth of field (R4). Local mirror of the camera's aperture /
  // focal distance, seeded from the server and written back via set_camera
  // (which restarts the path-tracer accumulation). aperture 0 = pinhole.
  const [aperture, setAperture] = createSignal(0);
  const [focalDistance, setFocalDistance] = createSignal(5);
  // Motion-blur shutter (R4) — fraction of the frame interval; applied on export.
  const [shutter, setShutter] = createSignal(0);
  onMount(async () => {
    try {
      const cam = await api.getCamera();
      setAperture(cam.aperture ?? 0);
      setFocalDistance(cam.focal_distance ?? 5);
      setShutter(cam.shutter ?? 0);
    } catch { /* ignore */ }
  });
  const commitDof = async (
    next: { aperture?: number; focal_distance?: number; shutter?: number },
  ) => {
    try {
      const cam = await api.getCamera();
      await api.setCamera({ ...cam, ...next });
    } catch (e) {
      console.warn('set_camera (lens/shutter) failed:', e);
    }
  };

  const applyPreset = async (view: api.CameraView) => {
    try {
      await api.setCameraView(view);
      // Pull the resulting camera back so the XYZ inputs reflect the new
      // pose. Best-effort; the field values are only a hint anyway.
      try {
        const cam = await api.getCamera();
        props.onPositionChange({ x: cam.position.x, y: cam.position.y, z: cam.position.z });
      } catch { /* ignore */ }
    } catch (e) {
      console.warn('setCameraView failed:', e);
    }
  };

  return (
    <div class="camera-controls">
      <h4>Camera</h4>
      <div class="camera-view-presets">
        <For each={VIEW_PRESETS}>
          {(p) => (
            <button
              type="button"
              class="camera-view-preset"
              title={p.hint}
              onClick={() => applyPreset(p.view)}
            >
              {p.label}
            </button>
          )}
        </For>
      </div>
      <div class="camera-inputs">
        <For each={['x', 'y', 'z'] as const}>
          {(axis) => (
            <div class="camera-input-row">
              <label class="axis">{axis.toUpperCase()}</label>
              <NumberInput
                step={0.1}
                title={`Camera ${axis.toUpperCase()}`}
                value={() => props.position()[axis]}
                onCommit={(v) => setAxis(axis, v)}
              />
            </div>
          )}
        </For>
      </div>
      <h4>Depth of Field</h4>
      <div class="camera-inputs">
        <div class="camera-input-row">
          <label title="Lens radius — 0 disables DOF (pinhole)">Aperture</label>
          <NumberInput
            step={0.01}
            min={0}
            title="Aperture (lens radius; 0 = pinhole)"
            value={() => aperture()}
            onCommit={(v) => {
              const a = Math.max(0, v);
              setAperture(a);
              void commitDof({ aperture: a });
            }}
          />
        </div>
        <div class="camera-input-row">
          <label title="Distance at which the scene is in perfect focus">Focal Dist</label>
          <NumberInput
            step={0.1}
            min={0}
            title="Focal distance (in-focus depth along the view direction)"
            value={() => focalDistance()}
            onCommit={(v) => {
              const f = Math.max(0, v);
              setFocalDistance(f);
              void commitDof({ focal_distance: f });
            }}
          />
        </div>
      </div>
      <h4>Motion Blur</h4>
      <div class="camera-inputs">
        <div class="camera-input-row">
          <label title="Shutter open fraction of the frame interval — 0 disables motion blur. Applied on sequence/EXR export.">Shutter</label>
          <NumberInput
            step={0.05}
            min={0}
            max={1}
            title="Shutter (fraction of a frame; 0 = off). Blurs moving objects on export."
            value={() => shutter()}
            onCommit={(v) => {
              const s = Math.max(0, Math.min(1, v));
              setShutter(s);
              void commitDof({ shutter: s });
            }}
          />
        </div>
      </div>
    </div>
  );
};
