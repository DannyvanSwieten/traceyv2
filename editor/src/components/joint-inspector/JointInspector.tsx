import { Component, For, createMemo } from 'solid-js';
import { NumberInput } from '../number-input/NumberInput';
import { sopGraph, setParamByUid } from '../../stores/sops';
import { findNodeRecursive } from '../../lib/sop_graph';
import './JointInspector.css';

// FK joint posing. Appears when a skeleton joint is picked in the viewport.
// The X/Y/Z fields are the joint's local-rotation override (degrees) on top of
// the clip/bind pose, stored in the owning gltf_import node's `pose_overrides`
// param ("jointIndex ex ey ez …"). We read + write that param through the
// frontend graph (setParamByUid), so edits go through schedulePush — which
// means they're UNDOABLE and re-cook the skinning, exactly like any other
// param edit. 0,0,0 clears the override.
interface JointInspectorProps {
  joint: () => number;
  importNode: () => number;
}

function parsePose(s: string): Map<number, [number, number, number]> {
  const m = new Map<number, [number, number, number]>();
  const t = s.trim().split(/\s+/).filter((x) => x.length);
  for (let i = 0; i + 3 < t.length; i += 4) {
    const j = parseInt(t[i], 10);
    if (!Number.isFinite(j)) continue;
    m.set(j, [Number(t[i + 1]) || 0, Number(t[i + 2]) || 0, Number(t[i + 3]) || 0]);
  }
  return m;
}

function serializePose(m: Map<number, [number, number, number]>): string {
  const parts: string[] = [];
  for (const [j, [x, y, z]] of m) {
    if (x === 0 && y === 0 && z === 0) continue; // omit no-op overrides
    parts.push(`${j} ${x} ${y} ${z}`);
  }
  return parts.join(' ');
}

export const JointInspector: Component<JointInspectorProps> = (props) => {
  // Current pose_overrides string of the owning gltf_import node, read live
  // from the graph signal so undo / external edits re-render the fields.
  const poseStr = createMemo(() => {
    const n = findNodeRecursive(sopGraph(), props.importNode());
    const p = n?.params['pose_overrides'];
    return p && p.type === 'string' ? p.value : '';
  });
  const euler = createMemo<[number, number, number]>(
    () => parsePose(poseStr()).get(props.joint()) ?? [0, 0, 0],
  );

  const setAxis = (axis: number, value: number) => {
    const m = parsePose(poseStr());
    const cur = m.get(props.joint()) ?? [0, 0, 0];
    const next: [number, number, number] = [cur[0], cur[1], cur[2]];
    next[axis] = value;
    if (next[0] === 0 && next[1] === 0 && next[2] === 0) m.delete(props.joint());
    else m.set(props.joint(), next);
    setParamByUid(props.importNode(), 'pose_overrides', {
      type: 'string',
      value: serializePose(m),
    });
  };

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
                value={() => euler()[i()]}
                onCommit={(v) => setAxis(i(), v)}
              />
            </div>
          )}
        </For>
      </div>
    </div>
  );
};
