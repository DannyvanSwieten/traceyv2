import { Component, Accessor, Setter } from 'solid-js';
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

export const CameraControls: Component<CameraControlsProps> = (props) => {
  const handleChange = (axis: 'x' | 'y' | 'z', value: string) => {
    const numValue = parseFloat(value);
    if (!isNaN(numValue)) {
      const newPos = { ...props.position() };
      newPos[axis] = numValue;
      props.onPositionChange(newPos);
    }
  };

  return (
    <div class="camera-controls">
      <h4>Camera Position</h4>
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
