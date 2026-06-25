import { Component, For } from 'solid-js';
import { NumberInput } from '../number-input/NumberInput';
import './JointInspector.css';

// FK joint posing. Appears when a skeleton joint is picked in the viewport.
// The X/Y/Z fields are the joint's local-rotation override (degrees) on top of
// the clip/bind pose; committing one calls onRotate, which drives the native
// set_joint_pose IPC → re-skin → live deform. 0,0,0 = no override.
interface JointInspectorProps {
  joint: () => number;
  rotation: () => [number, number, number];
  onRotate: (axis: number, value: number) => void;
}

export const JointInspector: Component<JointInspectorProps> = (props) => {
  return (
    <div class="joint-inspector">
      <h4>Joint {props.joint()}</h4>
      <div class="joint-inspector-rows">
        <For each={['Rot X', 'Rot Y', 'Rot Z']}>
          {(label, i) => (
            <div class="joint-inspector-row">
              <label class="axis">{label}</label>
              <NumberInput
                step={1}
                title={`${label} (degrees, on top of the clip pose)`}
                value={() => props.rotation()[i()]}
                onCommit={(v) => props.onRotate(i(), v)}
              />
            </div>
          )}
        </For>
      </div>
    </div>
  );
};
