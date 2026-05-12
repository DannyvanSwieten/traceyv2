// Right panel: parameter editor for the selected VOP node. Mirrors
// SopNodeInspector — the param-row UI is identical because both stores ship
// the same ParamValue shape.

import { Component, For, Show, createMemo } from 'solid-js';
import {
  ParamValue,
  VopNode,
  lookupCatalog,
} from '../../lib/vop_graph';
import {
  vopGraph,
  selectedNode,
  setParam,
  promoteParamToHost,
} from '../../stores/vops';
import type { ParamSpec } from '../../lib/vop_graph';
import type { ParamValueVec3 } from '../../lib/sop_graph';

export const VopNodeInspector: Component = () => {
  const node = createMemo<VopNode | null>(() => {
    const uid = selectedNode();
    if (uid === null) return null;
    return vopGraph().nodes.find((n) => n.uid === uid) ?? null;
  });

  const entry = createMemo(() => {
    const n = node();
    return n ? lookupCatalog(n.kind) : undefined;
  });

  return (
    <div class="sop-inspector">
      <Show when={node()} fallback={<div class="sop-inspector-empty">Select a VOP node</div>}>
        {(n) => (
          <>
            <div class="sop-inspector-header">
              <div class="sop-inspector-title">{entry()?.label ?? n().kind}</div>
              <div class="sop-inspector-uid">uid {n().uid}</div>
            </div>
            <div class="sop-inspector-params">
              <For each={entry()?.params ?? []}>
                {(spec) => (
                  <ParamRow
                    node={n()}
                    spec={spec}
                    onPromote={async () => {
                      const hostName = await promoteParamToHost(n().uid, spec.name);
                      if (!hostName) {
                        console.warn('promote failed');
                      }
                    }}
                  />
                )}
              </For>
            </div>
          </>
        )}
      </Show>
    </div>
  );
};

interface ParamRowProps {
  node: VopNode;
  spec: ParamSpec;
  // Promote this VOP-side param up to the host attribute_vop SOP node so it
  // can be animated through the existing SOP keyframe/dopesheet path.
  onPromote?: () => void;
}

const ParamRow: Component<ParamRowProps> = (props) => {
  const cur = () => props.node.params[props.spec.name];

  function patch(v: ParamValue) {
    setParam(props.node.uid, props.spec.name, v);
  }

  const PromoteBtn = () => (
    <Show when={props.spec.type !== 'string'}>
      <button
        type="button"
        class="vop-param-promote"
        title={`Promote ${props.spec.name} to the host attribute_vop SOP — makes it animatable`}
        onClick={() => props.onPromote?.()}
      >
        ↑
      </button>
    </Show>
  );

  // Commit on blur / Enter / spinner (onChange), not per keystroke (onInput).
  // See SopNodeInspector for the full rationale — short version: parseFloat
  // of partial input ("0.") collapses to 0 and the controlled value would
  // overwrite the user's typing, making fractional numbers untypeable.
  switch (props.spec.type) {
    case 'float':
      return (
        <div class="sop-param-row">
          <label>{props.spec.name}</label>
          <input
            type="number"
            step="0.01"
            title={props.spec.name}
            value={(cur() as { value: number } | undefined)?.value ?? 0}
            onChange={(e) =>
              patch({ type: 'float', value: parseFloat(e.currentTarget.value) || 0 })
            }
          />
          <PromoteBtn />
        </div>
      );
    case 'int':
      return (
        <div class="sop-param-row">
          <label>{props.spec.name}</label>
          <input
            type="number"
            step="1"
            title={props.spec.name}
            value={(cur() as { value: number } | undefined)?.value ?? 0}
            onChange={(e) =>
              patch({ type: 'int', value: parseInt(e.currentTarget.value, 10) || 0 })
            }
          />
          <PromoteBtn />
        </div>
      );
    case 'bool':
      return (
        <div class="sop-param-row">
          <label>{props.spec.name}</label>
          <input
            type="checkbox"
            title={props.spec.name}
            checked={(cur() as { value: boolean } | undefined)?.value ?? false}
            onChange={(e) =>
              patch({ type: 'bool', value: e.currentTarget.checked })
            }
          />
          <PromoteBtn />
        </div>
      );
    case 'vec3': {
      const v = () =>
        (cur() as ParamValueVec3 | undefined)?.value ?? ([0, 0, 0] as [number, number, number]);
      const setComp = (i: 0 | 1 | 2, val: number) => {
        const cv = v().slice() as [number, number, number];
        cv[i] = val;
        patch({ type: 'vec3', value: cv });
      };
      return (
        <div class="sop-param-row sop-param-vec3">
          <label>{props.spec.name}</label>
          <div class="sop-param-vec3-fields">
            <input type="number" step="0.01" title={`${props.spec.name} x`} value={v()[0]}
                   onChange={(e) => setComp(0, parseFloat(e.currentTarget.value) || 0)} />
            <input type="number" step="0.01" title={`${props.spec.name} y`} value={v()[1]}
                   onChange={(e) => setComp(1, parseFloat(e.currentTarget.value) || 0)} />
            <input type="number" step="0.01" title={`${props.spec.name} z`} value={v()[2]}
                   onChange={(e) => setComp(2, parseFloat(e.currentTarget.value) || 0)} />
          </div>
          <PromoteBtn />
        </div>
      );
    }
    case 'string':
      return (
        <div class="sop-param-row">
          <label>{props.spec.name}</label>
          <input
            type="text"
            title={props.spec.name}
            value={(cur() as { value: string } | undefined)?.value ?? ''}
            onChange={(e) => patch({ type: 'string', value: e.currentTarget.value })}
          />
        </div>
      );
  }
};
