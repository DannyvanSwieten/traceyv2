import { Component, For, Show } from 'solid-js';
import {
  Node,
  Vec4Tuple,
  ConstantNode,
  ParameterNode,
  BinaryOpNode,
  UnaryOpNode,
  TernaryOpNode,
  MATERIAL_OUTPUT_PORTS,
  inputPortCount,
  inputPortName,
} from '../../lib/material_graph';
import { materialGraph, selectedNode, setInputDefault, updateNode } from '../../stores/materials';
import { NumberInput } from '../number-input/NumberInput';
import './NodeInspector.css';

const BINARY_OPS = ['Add', 'Sub', 'Mul', 'Div', 'Dot3', 'Cross'];
const UNARY_OPS = ['Neg', 'Saturate', 'Normalize3', 'Length3', 'Splat'];
const TERNARY_OPS = ['Mix', 'Clamp'];

function findNode(uid: number | null): Node | undefined {
  if (uid === null) return undefined;
  return materialGraph().nodes.find((n) => n.uid === uid);
}

// Per-(node, port) editor-type hint. MaterialOutput's slots have known
// semantics (Albedo is colour, IOR is scalar, …) so the inspector swaps
// the generic Vec4 editor for a colour picker or a single-float field.
// Everything else keeps the Vec4 editor — the four-lane register is the
// canonical shape.
type EditorType = 'float' | 'color' | 'vec4';
function inputEditorType(node: Node, portIdx: number): EditorType {
  if (node.kind === 'MaterialOutput') {
    const portName = MATERIAL_OUTPUT_PORTS[portIdx];
    if (portName === 'Albedo' || portName === 'Emission') return 'color';
    if (
      portName === 'Metallic' || portName === 'Roughness' ||
      portName === 'Alpha'    || portName === 'IOR' ||
      portName === 'Transmission'
    ) return 'float';
  }
  return 'vec4';
}

// Known numeric ranges for MaterialOutput's scalar inputs, so the inspector
// renders a slider (with a numeric readout) instead of a bare field — the
// same affordance the SOP inspector gives ranged params. Null → plain field.
interface FloatRange { min: number; max: number; step: number }
function materialFloatRange(node: Node, portIdx: number): FloatRange | null {
  if (node.kind !== 'MaterialOutput') return null;
  switch (MATERIAL_OUTPUT_PORTS[portIdx]) {
    case 'Metallic':
    case 'Roughness':
    case 'Alpha':
    case 'Transmission':
      return { min: 0, max: 1, step: 0.01 };
    case 'IOR':
      return { min: 1, max: 3, step: 0.01 };
    default:
      return null;
  }
}

// Float editor that adapts to the Vec4Editor signature: reads `.x`, writes
// `[v, v, v, v]` so the splatted value is identical regardless of which
// lane downstream picks up.
interface ScalarEditorProps {
  label: string;
  value: () => Vec4Tuple;
  onChange: (next: Vec4Tuple) => void;
}
const FloatEditor: Component<ScalarEditorProps> = (props) => {
  return (
    <div class="inspector-vec4">
      <div class="inspector-row-label">{props.label}</div>
      <div class="inspector-vec4-row">
        <label class="inspector-vec4-cell">
          <NumberInput
            step={0.01}
            title={props.label}
            value={() => props.value()[0]}
            onCommit={(v) => props.onChange([v, v, v, v] as Vec4Tuple)}
          />
        </label>
      </div>
    </div>
  );
};

// Slider + numeric readout for a ranged scalar input. Same splat semantics as
// FloatEditor (reads `.x`, writes `[v,v,v,v]`).
const SliderEditor: Component<ScalarEditorProps & FloatRange> = (props) => {
  const set = (v: number) => props.onChange([v, v, v, v] as Vec4Tuple);
  return (
    <div class="inspector-vec4">
      <div class="inspector-row-label">{props.label}</div>
      <div class="inspector-slider-row">
        <input
          type="range"
          class="inspector-slider"
          aria-label={props.label}
          min={props.min}
          max={props.max}
          step={props.step}
          value={props.value()[0]}
          onInput={(e) => set(parseFloat(e.currentTarget.value) || 0)}
        />
        <NumberInput
          class="inspector-slider-readout"
          step={props.step}
          title={`${props.label} (value)`}
          value={() => props.value()[0]}
          onCommit={set}
        />
      </div>
    </div>
  );
};

// Colour picker variant: reads xyz as linear RGB in [0,1], renders via the
// native <input type="color">. Round-trips through 8-bit hex which is lossy
// at the bottom of the channel range — fine for material defaults, but
// don't expect it to preserve sub-0.004 values across edit cycles.
const ColorEditor: Component<ScalarEditorProps> = (props) => {
  const clamp01 = (x: number) => Math.max(0, Math.min(1, x));
  const toHex = () => {
    const v = props.value();
    const r = Math.round(clamp01(v[0]) * 255);
    const g = Math.round(clamp01(v[1]) * 255);
    const b = Math.round(clamp01(v[2]) * 255);
    return `#${[r, g, b].map((c) => c.toString(16).padStart(2, '0')).join('')}`;
  };
  const fromHex = (hex: string) => {
    const m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex);
    if (!m) return;
    const r = parseInt(m[1], 16) / 255;
    const g = parseInt(m[2], 16) / 255;
    const b = parseInt(m[3], 16) / 255;
    const cur = props.value();
    props.onChange([r, g, b, cur[3] ?? 1] as Vec4Tuple);
  };
  return (
    <div class="inspector-vec4">
      <div class="inspector-row-label">{props.label}</div>
      <div class="inspector-vec4-row">
        <input
          class="inspector-color"
          type="color"
          aria-label={props.label}
          value={toHex()}
          onInput={(e) => fromHex(e.currentTarget.value)}
        />
      </div>
    </div>
  );
};

interface Vec4EditorProps {
  label: string;
  value: () => Vec4Tuple;
  onChange: (next: Vec4Tuple) => void;
}

const Vec4Editor: Component<Vec4EditorProps> = (props) => {
  const setComponent = (idx: 0 | 1 | 2 | 3, parsed: number) => {
    const cur = props.value();
    const next: Vec4Tuple = [cur[0], cur[1], cur[2], cur[3]] as Vec4Tuple;
    next[idx] = parsed;
    props.onChange(next);
  };
  return (
    <div class="inspector-vec4">
      <div class="inspector-row-label">{props.label}</div>
      <div class="inspector-vec4-row">
        {(['x', 'y', 'z', 'w'] as const).map((axis, idx) => (
          <label class="inspector-vec4-cell">
            <span>{axis}</span>
            <NumberInput
              step={0.01}
              title={`${props.label} ${axis}`}
              value={() => props.value()[idx as 0 | 1 | 2 | 3]}
              onCommit={(v) => setComponent(idx as 0 | 1 | 2 | 3, v)}
            />
          </label>
        ))}
      </div>
    </div>
  );
};

interface OpSelectProps {
  label: string;
  options: string[];
  value: () => string;
  onChange: (next: string) => void;
}

const OpSelect: Component<OpSelectProps> = (props) => (
  <div class="inspector-row">
    <div class="inspector-row-label">{props.label}</div>
    <select
      class="inspector-select"
      value={props.value()}
      onChange={(e) => props.onChange(e.currentTarget.value)}
    >
      {props.options.map((opt) => (
        <option value={opt}>{opt}</option>
      ))}
    </select>
  </div>
);

export const NodeInspector: Component = () => {
  const node = () => findNode(selectedNode());

  return (
    <div class="node-inspector">
      <div class="node-inspector-header">Inspector</div>
      <Show when={node()} fallback={<div class="node-inspector-empty">Select a node to edit its parameters.</div>}>
        {(n) => (
          // Every read goes through n() rather than a captured snapshot:
          // Solid's non-keyed <Show> re-runs this callback only when the
          // truthiness of `when` flips, not when it changes from one node
          // to another. Capturing `n()` into a const froze the uid at the
          // first selected node, so subsequent selections kept editing &
          // displaying the old node ("the same modification appears on
          // every node box").
          <div class="node-inspector-body">
            <div class="inspector-row inspector-row-readonly">
              <div class="inspector-row-label">Kind</div>
              <div class="inspector-row-value">{n().kind}</div>
            </div>
            <div class="inspector-row inspector-row-readonly">
              <div class="inspector-row-label">UID</div>
              <div class="inspector-row-value">{n().uid}</div>
            </div>

            <Show when={n().kind === 'Constant'}>
              <Vec4Editor
                label="Value"
                value={() => (findNode(n().uid) as ConstantNode).value}
                onChange={(next) => updateNode<ConstantNode>(n().uid, { value: next })}
              />
            </Show>

            <Show when={n().kind === 'Parameter'}>
              <div class="inspector-row">
                <div class="inspector-row-label">Name</div>
                <input
                  class="inspector-text"
                  type="text"
                  aria-label="Parameter name"
                  placeholder="param"
                  value={(findNode(n().uid) as ParameterNode).name}
                  onInput={(e) =>
                    updateNode<ParameterNode>(n().uid, { name: e.currentTarget.value })
                  }
                />
              </div>
              <Vec4Editor
                label="Default"
                value={() => (findNode(n().uid) as ParameterNode).default}
                onChange={(next) => updateNode<ParameterNode>(n().uid, { default: next })}
              />
            </Show>

            <Show when={n().kind === 'BinaryOp'}>
              <OpSelect
                label="Op"
                options={BINARY_OPS}
                value={() => (findNode(n().uid) as BinaryOpNode).op}
                onChange={(op) => updateNode<BinaryOpNode>(n().uid, { op })}
              />
            </Show>

            <Show when={n().kind === 'UnaryOp'}>
              <OpSelect
                label="Op"
                options={UNARY_OPS}
                value={() => (findNode(n().uid) as UnaryOpNode).op}
                onChange={(op) => updateNode<UnaryOpNode>(n().uid, { op })}
              />
            </Show>

            <Show when={n().kind === 'TernaryOp'}>
              <OpSelect
                label="Op"
                options={TERNARY_OPS}
                value={() => (findNode(n().uid) as TernaryOpNode).op}
                onChange={(op) => updateNode<TernaryOpNode>(n().uid, { op })}
              />
            </Show>
            <InputDefaultsSection node={n()} />
          </div>
        )}
      </Show>
    </div>
  );
};

// Per-input constant editors (Houdini-style "type into the input").
// One Vec4Editor per *unconnected* input port. Connected ports are
// hidden — the wire is the source of truth. Reads/writes the node's
// `input_defaults[port]` map, which the compiler honours when no wire
// is present.
const InputDefaultsSection: Component<{ node: Node }> = (props) => {
  const ports = (): number[] => {
    const n = inputPortCount(props.node.kind);
    return Array.from({ length: n }, (_, i) => i);
  };
  // Reactive: reading materialGraph().connections inside the closure
  // keeps Solid tracking it, so the editor appears/disappears as the
  // user wires up the inputs.
  const isConnected = (portIdx: number): boolean =>
    materialGraph().connections.some(
      (c) => c.to_node === props.node.uid && c.to_port === portIdx,
    );
  return (
    <Show when={ports().length > 0}>
      <div class="inspector-section-label">Inputs</div>
      <For each={ports()}>
        {(portIdx) => (
          <Show when={!isConnected(portIdx)}>
            <InputDefaultRow node={props.node} portIdx={portIdx} />
          </Show>
        )}
      </For>
    </Show>
  );
};

const InputDefaultRow: Component<{ node: Node; portIdx: number }> = (props) => {
  const stored = (): Vec4Tuple => {
    const live = findNode(props.node.uid);
    const v = live?.input_defaults?.[String(props.portIdx)];
    return (v ?? [0, 0, 0, 1]) as Vec4Tuple;
  };
  const label = () =>
    inputPortName(props.node.kind, props.portIdx) || `in ${props.portIdx}`;
  const onChange = (next: Vec4Tuple) =>
    setInputDefault(props.node.uid, props.portIdx, next);
  const kind = () => inputEditorType(props.node, props.portIdx);
  const range = () => materialFloatRange(props.node, props.portIdx);
  return (
    <Show
      when={kind() === 'color'}
      fallback={
        <Show
          when={kind() === 'float'}
          fallback={<Vec4Editor label={label()} value={stored} onChange={onChange} />}
        >
          {/* Ranged scalars (Metallic/Roughness/Alpha/Transmission/IOR) get a
              slider; any other float keeps the bare numeric field. */}
          <Show
            when={range()}
            fallback={<FloatEditor label={label()} value={stored} onChange={onChange} />}
          >
            {(r) => (
              <SliderEditor
                label={label()}
                value={stored}
                onChange={onChange}
                min={r().min}
                max={r().max}
                step={r().step}
              />
            )}
          </Show>
        </Show>
      }
    >
      <ColorEditor label={label()} value={stored} onChange={onChange} />
    </Show>
  );
};
