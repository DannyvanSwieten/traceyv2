// Right panel: parameter editor for the selected SOP node. Driven by the
// catalog's param specs — each parameter renders an input typed by its
// declared ParamType (float / int / bool / vec3 / string).

import { Component, For, Show, createMemo } from 'solid-js';
import {
  ParamSpec,
  ParamType,
  ParamValue,
  ParamValueVec3,
  lookupCatalog,
  SopNode,
} from '../../lib/sop_graph';
import { currentGraph, selectedNode, setParam } from '../../stores/sops';
import { openVopEditor } from '../../stores/vops';
import { autoKey, setKeyAtPlayhead } from '../../stores/timeline';
import * as api from '../../lib/api';

export const SopNodeInspector: Component = () => {
  const node = createMemo<SopNode | null>(() => {
    const uid = selectedNode();
    if (uid === null) return null;
    // Selection lives in the current sub-graph (canvas only allows selecting
    // nodes it renders). Scanning the root graph would miss nodes inside a
    // subnet the user has dived into.
    return currentGraph().nodes.find((n) => n.uid === uid) ?? null;
  });

  const entry = createMemo(() => {
    const n = node();
    return n ? lookupCatalog(n.kind) : undefined;
  });

  // Render the union of catalog params (declared by the registry) + extra
  // params present on the live node. The latter cover attribute_vop's
  // "promote to host" path — promoted params don't exist in the catalog
  // because the catalog is per-kind and these are per-instance — so we
  // synthesise a ParamSpec from the live param's type.
  const renderedParams = createMemo<ParamSpec[]>(() => {
    const n = node();
    if (!n) return [];
    const fromCatalog = entry()?.params ?? [];
    const known = new Set(fromCatalog.map((p) => p.name));
    const extras: ParamSpec[] = [];
    for (const [name, value] of Object.entries(n.params)) {
      if (known.has(name)) continue;
      extras.push({ name, type: value.type as ParamType, default: '' });
    }
    return [...fromCatalog, ...extras];
  });

  // Promoted host params come from the host SOP's `extra.promotions` block.
  // We surface them in the inspector with a tiny "↑" affordance — clicking
  // demotes the param, restoring catalog-only state. Catalog-declared params
  // are not in this set.
  const promotedHostParamNames = createMemo<Set<string>>(() => {
    const n = node();
    if (!n || n.kind !== 'attribute_vop') return new Set();
    const set = new Set<string>();
    const extra = (n as { extra?: { promotions?: { host_param_name?: string }[] } }).extra;
    if (extra?.promotions) {
      for (const p of extra.promotions) {
        if (p.host_param_name) set.add(p.host_param_name);
      }
    }
    return set;
  });

  const demote = async (hostName: string) => {
    const n = node();
    if (!n) return;
    try {
      await api.send<boolean>('vop_demote_param', {
        host_uid: n.uid,
        host_param_name: hostName,
      });
    } catch (e) {
      console.warn('vop_demote_param failed:', e);
    }
  };

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
              <For each={renderedParams()}>
                {(spec) => (
                  <ParamRow
                    node={n()}
                    spec={spec}
                    promoted={promotedHostParamNames().has(spec.name)}
                    onDemote={() => demote(spec.name)}
                  />
                )}
              </For>
            </div>
            <Show when={n().kind === 'attribute_vop'}>
              <button
                class="sop-inspector-action"
                type="button"
                onClick={() => openVopEditor(n().uid)}
              >
                Edit VOP Graph
              </button>
            </Show>
          </>
        )}
      </Show>
    </div>
  );
};

interface ParamRowProps {
  node: SopNode;
  spec: ParamSpec;
  // True if this row corresponds to a VOP param that was promoted to the
  // host attribute_vop SOP node. The row gets a small affordance to demote
  // (strip the promotion + this param).
  promoted?: boolean;
  onDemote?: () => void;
}

const ParamRow: Component<ParamRowProps> = (props) => {
  const cur = () => props.node.params[props.spec.name];

  // patch(v) writes the new value into the SOP graph store. When auto-key
  // is on it also writes a keyframe at the current playhead for the changed
  // component. `component` defaults to 0 (scalar params); the vec3 case
  // passes the axis index explicitly.
  function patch(v: ParamValue, component: number = 0) {
    setParam(props.node.uid, props.spec.name, v);
    if (!autoKey()) return;
    let numeric: number | null = null;
    switch (v.type) {
      case 'float': numeric = v.value; break;
      case 'int':   numeric = v.value; break;
      case 'bool':  numeric = v.value ? 1 : 0; break;
      case 'vec3':  numeric = v.value[component]; break;
      // 'string' isn't animatable — channels are numeric.
    }
    if (numeric === null) return;
    setKeyAtPlayhead({
      nodeUid: props.node.uid,
      paramName: props.spec.name,
      component,
      value: numeric,
    }).catch((e) => console.warn('autokey failed:', e));
  }

  const DemoteBtn = () => (
    <Show when={props.promoted}>
      <button
        type="button"
        class="sop-param-demote"
        title={`Demote ${props.spec.name} (remove the promotion + this host param)`}
        onClick={() => props.onDemote?.()}
      >
        ↓
      </button>
    </Show>
  );

  // Number/text inputs use `onChange` (fires on blur / Enter / spinner) not
  // `onInput` (fires per keystroke). Two reasons:
  //   1. parseFloat("0.") === 0, so patching every keystroke would reset the
  //      controlled value back to "0" and prevent typing fractional numbers.
  //   2. Commit-on-leave matches Houdini and avoids a cook per character.
  //
  // `title` on every input pulls double duty: it's the param name on
  // hover (Houdini behaviour) and it satisfies the a11y lint rule that
  // expects every form element to have some textual labelling beyond the
  // sibling <label> (which isn't `htmlFor`-linked).
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
          <DemoteBtn />
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
          <DemoteBtn />
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
          <DemoteBtn />
        </div>
      );
    case 'vec3': {
      const v = () => (cur() as ParamValueVec3 | undefined)?.value ?? ([0, 0, 0] as [number, number, number]);
      const setComp = (i: 0 | 1 | 2, val: number) => {
        const cv = v().slice() as [number, number, number];
        cv[i] = val;
        patch({ type: 'vec3', value: cv }, i);
      };
      return (
        <div class="sop-param-row sop-param-vec3">
          <label>{props.spec.name}</label>
          <div class="sop-param-vec3-fields">
            <input type="number" step="0.01" value={v()[0]}
                   title={`${props.spec.name}.x`}
                   onChange={(e) => setComp(0, parseFloat(e.currentTarget.value) || 0)} />
            <input type="number" step="0.01" value={v()[1]}
                   title={`${props.spec.name}.y`}
                   onChange={(e) => setComp(1, parseFloat(e.currentTarget.value) || 0)} />
            <input type="number" step="0.01" value={v()[2]}
                   title={`${props.spec.name}.z`}
                   onChange={(e) => setComp(2, parseFloat(e.currentTarget.value) || 0)} />
          </div>
          <DemoteBtn />
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
          <DemoteBtn />
        </div>
      );
  }
};
