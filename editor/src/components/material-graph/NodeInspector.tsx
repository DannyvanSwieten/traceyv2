import { Component, For, Show } from 'solid-js';
import {
  Node,
  Vec4Tuple,
  ConstantNode,
  ParameterNode,
  InputAttributeNode,
  SurfaceAttributeNode,
  BinaryOpNode,
  UnaryOpNode,
  TernaryOpNode,
  OutputNode,
  inputPortCount,
  inputPortName,
} from '../../lib/material_graph';
import { materialGraph, selectedNode, setInputDefault, updateNode } from '../../stores/materials';
import './NodeInspector.css';

const INPUT_ATTRIBUTE_OPS = [
  'LoadInputAlbedo',
  'LoadInputMetallic',
  'LoadInputRoughness',
  'LoadInputEmission',
  'LoadInputNormal',
];
const SURFACE_ATTRIBUTE_OPS = [
  'LoadPosition',
  'LoadNormal',
  'LoadTangent',
  'LoadViewDir',
  'LoadUV0',
  'LoadUV1',
];
const BINARY_OPS = ['Add', 'Sub', 'Mul', 'Div', 'Dot3', 'Cross'];
const UNARY_OPS = ['Neg', 'Saturate', 'Normalize3', 'Length3', 'Splat'];
const TERNARY_OPS = ['Mix', 'Clamp'];
const OUTPUT_OPS = [
  'WriteAlbedo',
  'WriteMetallic',
  'WriteRoughness',
  'WriteEmission',
  'WriteNormal',
  'WriteAlpha',
  'WriteIor',
  'WriteTransmission',
];

function findNode(uid: number | null): Node | undefined {
  if (uid === null) return undefined;
  return materialGraph().nodes.find((n) => n.uid === uid);
}

interface Vec4EditorProps {
  label: string;
  value: () => Vec4Tuple;
  onChange: (next: Vec4Tuple) => void;
}

const Vec4Editor: Component<Vec4EditorProps> = (props) => {
  const setComponent = (idx: 0 | 1 | 2 | 3, raw: string) => {
    const parsed = parseFloat(raw);
    if (Number.isNaN(parsed)) return;
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
            <input
              type="number"
              step="0.01"
              value={props.value()[idx as 0 | 1 | 2 | 3]}
              onInput={(e) => setComponent(idx as 0 | 1 | 2 | 3, e.currentTarget.value)}
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

            <Show when={n().kind === 'InputAttribute'}>
              <OpSelect
                label="Op"
                options={INPUT_ATTRIBUTE_OPS}
                value={() => (findNode(n().uid) as InputAttributeNode).op}
                onChange={(op) => updateNode<InputAttributeNode>(n().uid, { op })}
              />
            </Show>

            <Show when={n().kind === 'SurfaceAttribute'}>
              <OpSelect
                label="Op"
                options={SURFACE_ATTRIBUTE_OPS}
                value={() => (findNode(n().uid) as SurfaceAttributeNode).op}
                onChange={(op) => updateNode<SurfaceAttributeNode>(n().uid, { op })}
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

            <Show when={n().kind === 'Output'}>
              <OpSelect
                label="Op"
                options={OUTPUT_OPS}
                value={() => (findNode(n().uid) as OutputNode).op}
                onChange={(op) => updateNode<OutputNode>(n().uid, { op })}
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
  return (
    <Vec4Editor
      label={inputPortName(props.node.kind, props.portIdx) || `in ${props.portIdx}`}
      value={stored}
      onChange={(next) => setInputDefault(props.node.uid, props.portIdx, next)}
    />
  );
};
