// Right panel: parameter editor for the selected SOP node. Driven by the
// catalog's param specs — each parameter renders an input typed by its
// declared ParamType (float / int / bool / vec3 / string).

import { Component, For, Show, createMemo } from 'solid-js';
import {
  ParamSpec,
  ParamValue,
  ParamValueVec3,
  lookupCatalog,
  SopNode,
} from '../../lib/sop_graph';
import { sopGraph, selectedNode, setParam } from '../../stores/sops';

export const SopNodeInspector: Component = () => {
  const node = createMemo<SopNode | null>(() => {
    const uid = selectedNode();
    if (uid === null) return null;
    return sopGraph().nodes.find((n) => n.uid === uid) ?? null;
  });

  const entry = createMemo(() => {
    const n = node();
    return n ? lookupCatalog(n.kind) : undefined;
  });

  return (
    <div class="sop-inspector">
      <Show when={node()} fallback={<div class="sop-inspector-empty">Select a node</div>}>
        {(n) => (
          <>
            <div class="sop-inspector-header">
              <div class="sop-inspector-title">{entry()?.label ?? n().kind}</div>
              <div class="sop-inspector-uid">uid {n().uid}</div>
            </div>
            <div class="sop-inspector-params">
              <For each={entry()?.params ?? []}>
                {(spec) => (
                  <ParamRow node={n()} spec={spec} />
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
  node: SopNode;
  spec: ParamSpec;
}

const ParamRow: Component<ParamRowProps> = (props) => {
  const cur = () => props.node.params[props.spec.name];

  function patch(v: ParamValue) {
    setParam(props.node.uid, props.spec.name, v);
  }

  switch (props.spec.type) {
    case 'float':
      return (
        <div class="sop-param-row">
          <label>{props.spec.name}</label>
          <input
            type="number"
            step="0.01"
            value={(cur() as { value: number } | undefined)?.value ?? 0}
            onInput={(e) =>
              patch({ type: 'float', value: parseFloat(e.currentTarget.value) || 0 })
            }
          />
        </div>
      );
    case 'int':
      return (
        <div class="sop-param-row">
          <label>{props.spec.name}</label>
          <input
            type="number"
            step="1"
            value={(cur() as { value: number } | undefined)?.value ?? 0}
            onInput={(e) =>
              patch({ type: 'int', value: parseInt(e.currentTarget.value, 10) || 0 })
            }
          />
        </div>
      );
    case 'bool':
      return (
        <div class="sop-param-row">
          <label>{props.spec.name}</label>
          <input
            type="checkbox"
            checked={(cur() as { value: boolean } | undefined)?.value ?? false}
            onChange={(e) =>
              patch({ type: 'bool', value: e.currentTarget.checked })
            }
          />
        </div>
      );
    case 'vec3': {
      const v = () => (cur() as ParamValueVec3 | undefined)?.value ?? ([0, 0, 0] as [number, number, number]);
      const setComp = (i: 0 | 1 | 2, val: number) => {
        const cv = v().slice() as [number, number, number];
        cv[i] = val;
        patch({ type: 'vec3', value: cv });
      };
      return (
        <div class="sop-param-row sop-param-vec3">
          <label>{props.spec.name}</label>
          <div class="sop-param-vec3-fields">
            <input type="number" step="0.01" value={v()[0]}
                   onInput={(e) => setComp(0, parseFloat(e.currentTarget.value) || 0)} />
            <input type="number" step="0.01" value={v()[1]}
                   onInput={(e) => setComp(1, parseFloat(e.currentTarget.value) || 0)} />
            <input type="number" step="0.01" value={v()[2]}
                   onInput={(e) => setComp(2, parseFloat(e.currentTarget.value) || 0)} />
          </div>
        </div>
      );
    }
    case 'string':
      return (
        <div class="sop-param-row">
          <label>{props.spec.name}</label>
          <input
            type="text"
            value={(cur() as { value: string } | undefined)?.value ?? ''}
            onInput={(e) => patch({ type: 'string', value: e.currentTarget.value })}
          />
        </div>
      );
  }
};
