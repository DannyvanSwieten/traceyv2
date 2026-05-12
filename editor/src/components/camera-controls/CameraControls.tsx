import { Component, Accessor, For } from 'solid-js';
import * as api from '../../lib/api';
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
  const handleChange = (axis: 'x' | 'y' | 'z', value: string) => {
    const numValue = parseFloat(value);
    if (!isNaN(numValue)) {
      const newPos = { ...props.position() };
      newPos[axis] = numValue;
      props.onPositionChange(newPos);
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
        <div class="camera-input-row">
          <label for="cam-x">X</label>
          <input
            id="cam-x"
            type="number"
            step="0.1"
            value={props.position().x.toFixed(2)}
            onChange={(e) => handleChange('x', e.currentTarget.value)}
          />
        </div>
        <div class="camera-input-row">
          <label for="cam-y">Y</label>
          <input
            id="cam-y"
            type="number"
            step="0.1"
            value={props.position().y.toFixed(2)}
            onChange={(e) => handleChange('y', e.currentTarget.value)}
          />
        </div>
        <div class="camera-input-row">
          <label for="cam-z">Z</label>
          <input
            id="cam-z"
            type="number"
            step="0.1"
            value={props.position().z.toFixed(2)}
            onChange={(e) => handleChange('z', e.currentTarget.value)}
          />
        </div>
      </div>
    </div>
  );
};
