import { Component, Show } from 'solid-js';
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
} from '../../lib/material_graph';
import { materialGraph, selectedNode, updateNode } from '../../stores/materials';
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
        {(n) => {
          const cur = n();
          return (
            <div class="node-inspector-body">
              <div class="inspector-row inspector-row-readonly">
                <div class="inspector-row-label">Kind</div>
                <div class="inspector-row-value">{cur.kind}</div>
              </div>
              <div class="inspector-row inspector-row-readonly">
                <div class="inspector-row-label">UID</div>
                <div class="inspector-row-value">{cur.uid}</div>
              </div>

              <Show when={cur.kind === 'Constant'}>
                {(() => {
                  const c = cur as ConstantNode;
                  return (
                    <Vec4Editor
                      label="Value"
                      value={() => (findNode(cur.uid) as ConstantNode).value}
                      onChange={(next) => updateNode<ConstantNode>(c.uid, { value: next })}
                    />
                  );
                })()}
              </Show>

              <Show when={cur.kind === 'Parameter'}>
                {(() => {
                  const p = cur as ParameterNode;
                  return (
                    <>
                      <div class="inspector-row">
                        <div class="inspector-row-label">Name</div>
                        <input
                          class="inspector-text"
                          type="text"
                          value={(findNode(cur.uid) as ParameterNode).name}
                          onInput={(e) =>
                            updateNode<ParameterNode>(p.uid, { name: e.currentTarget.value })
                          }
                        />
                      </div>
                      <Vec4Editor
                        label="Default"
                        value={() => (findNode(cur.uid) as ParameterNode).default}
                        onChange={(next) =>
                          updateNode<ParameterNode>(p.uid, { default: next })
                        }
                      />
                    </>
                  );
                })()}
              </Show>

              <Show when={cur.kind === 'InputAttribute'}>
                <OpSelect
                  label="Op"
                  options={INPUT_ATTRIBUTE_OPS}
                  value={() => (findNode(cur.uid) as InputAttributeNode).op}
                  onChange={(op) => updateNode<InputAttributeNode>(cur.uid, { op })}
                />
              </Show>

              <Show when={cur.kind === 'SurfaceAttribute'}>
                <OpSelect
                  label="Op"
                  options={SURFACE_ATTRIBUTE_OPS}
                  value={() => (findNode(cur.uid) as SurfaceAttributeNode).op}
                  onChange={(op) => updateNode<SurfaceAttributeNode>(cur.uid, { op })}
                />
              </Show>

              <Show when={cur.kind === 'BinaryOp'}>
                <OpSelect
                  label="Op"
                  options={BINARY_OPS}
                  value={() => (findNode(cur.uid) as BinaryOpNode).op}
                  onChange={(op) => updateNode<BinaryOpNode>(cur.uid, { op })}
                />
              </Show>

              <Show when={cur.kind === 'UnaryOp'}>
                <OpSelect
                  label="Op"
                  options={UNARY_OPS}
                  value={() => (findNode(cur.uid) as UnaryOpNode).op}
                  onChange={(op) => updateNode<UnaryOpNode>(cur.uid, { op })}
                />
              </Show>

              <Show when={cur.kind === 'TernaryOp'}>
                <OpSelect
                  label="Op"
                  options={TERNARY_OPS}
                  value={() => (findNode(cur.uid) as TernaryOpNode).op}
                  onChange={(op) => updateNode<TernaryOpNode>(cur.uid, { op })}
                />
              </Show>

              <Show when={cur.kind === 'Output'}>
                <OpSelect
                  label="Op"
                  options={OUTPUT_OPS}
                  value={() => (findNode(cur.uid) as OutputNode).op}
                  onChange={(op) => updateNode<OutputNode>(cur.uid, { op })}
                />
              </Show>
            </div>
          );
        }}
      </Show>
    </div>
  );
};
