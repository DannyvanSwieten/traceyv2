import { Component, Accessor, For } from 'solid-js';
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
              <label>{axis.toUpperCase()}</label>
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
    </div>
  );
};
