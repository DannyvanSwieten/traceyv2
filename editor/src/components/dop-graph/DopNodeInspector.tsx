// Right panel: parameter editor for the selected DOP node. Same row UI
// as SopNodeInspector / VopNodeInspector — both stores ship the same
// ParamValue shape. Subset of the VOP inspector: no promote button,
// no input-defaults section (DOP nodes don't expose port-input
// constants in v1). Range sliders are supported because particle params
// (rate, lifetime, jitter) benefit from them.
//
// pop_force gets a special "Edit Force VOP…" button that opens the
// existing per-host VOP editor against this DOP node's uid.

import { Component, For, Match, Show, Switch, createMemo } from 'solid-js';
import {
  ParamValue,
  DopNode,
  ParamSpec,
  lookupCatalog,
} from '../../lib/dop_graph';
import { dopGraph, selectedNode, setParam } from '../../stores/dops';
import { openVopEditor } from '../../stores/vops';
import { sopGraph } from '../../stores/sops';
import type { ParamValueVec3, SopGraph as SopGraphType, SopNode as SopNodeType } from '../../lib/sop_graph';
import { NumberInput } from '../number-input/NumberInput';

// Walk every SOP node in the live graph (including subnet children).
// Used by the "sop_node" picker to populate its option list.
// dop_import is excluded — a pop_source reading from a dop_import that's
// reading from this same DOP graph would create a sim-cook cycle.
function listSopNodesForPicker(g: SopGraphType | null | undefined): SopNodeType[] {
  if (!g) return [];
  const out: SopNodeType[] = [];
  const walk = (graph: SopGraphType) => {
    for (const n of graph.nodes) {
      if (n.kind !== 'dop_import') out.push(n);
      if (n.subgraph) walk(n.subgraph);
    }
  };
  walk(g);
  return out;
}

function sopNodeDisplayName(n: SopNodeType): string {
  // Many terminals (object_output / instance / instance_vop / scatter)
  // expose a user-visible `name` param. Fall back to "<kind> #<uid>"
  // when no name is set so the user can still pick.
  const p = n.params['name'];
  if (p && p.type === 'string' && typeof p.value === 'string' && p.value.length > 0) {
    return `${p.value} (${n.kind})`;
  }
  return `${n.kind} #${n.uid}`;
}

export const DopNodeInspector: Component = () => {
  const node = createMemo<DopNode | null>(() => {
    const uid = selectedNode();
    if (uid === null) return null;
    return dopGraph().nodes.find((n) => n.uid === uid) ?? null;
  });

  const entry = createMemo(() => {
    const n = node();
    return n ? lookupCatalog(n.kind) : undefined;
  });

  return (
    <div class="sop-inspector">
      <Show when={node()} fallback={<div class="sop-inspector-empty">Select a DOP node</div>}>
        {(n) => (
          <>
            <div class="sop-inspector-header">
              <div class="sop-inspector-title">{entry()?.label ?? n().kind}</div>
              <div class="sop-inspector-uid">uid {n().uid}</div>
            </div>
            <Show when={n().kind === 'pop_force'}>
              <div class="sop-inspector-action">
                <button
                  type="button"
                  class="sop-inspector-edit-btn"
                  onClick={() => openVopEditor(n().uid)}
                >
                  Edit Force VOP…
                </button>
              </div>
            </Show>
            <div class="sop-inspector-params">
              {/* `<For>` keys rows by reference identity. The catalog's
                  params array is stable across setParam ticks (the
                  catalog signal doesn't change when a node's stored
                  value changes), so slider drag pointer-capture survives.
                  When the user selects a DIFFERENT node kind, entry()
                  returns a new CatalogEntry → new params array → For
                  remounts each row with the right spec, fixing the
                  "ParamRow's switch(spec.type) was evaluated at first
                  mount and stays as the wrong layout" bug we saw with
                  the old `<Index>` (Index keys by position and reuses
                  the existing component instance, which means switching
                  from pop_source (rate=float-slider at idx 0) to
                  pop_wind (direction=vec3 at idx 0) kept the slider). */}
              <For each={entry()?.params ?? []}>
                {(spec) => <ParamRow node={n()} spec={spec} />}
              </For>
            </div>
          </>
        )}
      </Show>
    </div>
  );
};

interface ParamRowProps {
  node: DopNode;
  spec: ParamSpec;
}

const ParamRow: Component<ParamRowProps> = (props) => {
  const cur = () => props.node.params[props.spec.name];

  function patch(v: ParamValue) {
    setParam(props.node.uid, props.spec.name, v);
  }

  const range = () => props.spec.range;
  const options = () => props.spec.options;

  // Reactive type dispatch via <Switch>/<Match> — see SopNodeInspector
  // for the full rationale. Fixes the "ParamRow's switch ran once at
  // mount and stuck with the wrong layout when the row was reused
  // for a different node kind" bug (e.g. pop_source.rate slider stayed
  // rendered as a slider when the row was reassigned to pop_wind.direction
  // which is vec3).
  const floatValue  = () => (cur() as { value: number } | undefined)?.value ?? 0;
  const intValue    = () => (cur() as { value: number } | undefined)?.value ?? 0;
  const boolValue   = () => (cur() as { value: boolean } | undefined)?.value ?? false;
  const stringValue = () => (cur() as { value: string } | undefined)?.value ?? '';
  const vec3Value   = () =>
    ((cur() as ParamValueVec3 | undefined)?.value ?? [0, 0, 0]) as [number, number, number];
  const setVec3Axis = (axis: 0 | 1 | 2, v: number) => {
    const cv = vec3Value();
    const next: [number, number, number] = [cv[0], cv[1], cv[2]];
    next[axis] = v;
    patch({ type: 'vec3', value: next });
  };

  return (
    <Switch>
      <Match when={props.spec.type === 'float'}>
        <Show
          when={(() => { const r = range(); return r && r.min !== r.max ? r : null; })()}
          fallback={
            <div class="sop-param-row">
              <label>{props.spec.name}</label>
              <NumberInput
                step={0.01}
                title={props.spec.name}
                value={floatValue}
                onCommit={(v) => patch({ type: 'float', value: v })}
              />
            </div>
          }
        >
          {(r) => {
            const step = r().step > 0 ? r().step : (r().max - r().min) / 200;
            return (
              <div class="sop-param-row sop-param-slider-row">
                <label>{props.spec.name}</label>
                <div class="sop-param-slider-group">
                  <input
                    type="range"
                    class="sop-param-slider"
                    min={r().min}
                    max={r().max}
                    step={step}
                    value={floatValue()}
                    onInput={(e) =>
                      patch({ type: 'float', value: parseFloat(e.currentTarget.value) })
                    }
                  />
                  <NumberInput
                    class="sop-param-slider-readout"
                    step={step}
                    title={`${props.spec.name} (value)`}
                    value={floatValue}
                    onCommit={(v) => patch({ type: 'float', value: v })}
                  />
                </div>
              </div>
            );
          }}
        </Show>
      </Match>

      <Match when={props.spec.type === 'int' && props.spec.picker === 'sop_node'}>
        {/* SOP-node picker. Dropdown lists every SOP in the live graph
            (recursing into subnets) so the user can bind a DOP param
            to "the cooked output of SOP node N". The stored value is
            the uid; the dropdown label is the node's `name` param when
            present, else "<kind> #<uid>". `0` is the "(none)" sentinel.
            See pop_source's emit_mode="geometry" path. */}
        <div class="sop-param-row">
          <label>{props.spec.name}</label>
          <select
            title={props.spec.name}
            value={String(intValue())}
            onChange={(e) =>
              patch({ type: 'int', value: parseInt(e.currentTarget.value, 10) || 0 })
            }
          >
            <option value="0">— (none) —</option>
            <For each={listSopNodesForPicker(sopGraph())}>
              {(sn) => <option value={String(sn.uid)}>{sopNodeDisplayName(sn)}</option>}
            </For>
          </select>
        </div>
      </Match>

      <Match when={props.spec.type === 'int'}>
        <Show
          when={(() => { const o = options(); return o && o.length > 0 ? o : null; })()}
          fallback={
            <div class="sop-param-row">
              <label>{props.spec.name}</label>
              <NumberInput
                step={1}
                decimals={0}
                title={props.spec.name}
                value={intValue}
                onCommit={(v) => patch({ type: 'int', value: Math.round(v) })}
              />
            </div>
          }
        >
          {(opts) => (
            <div class="sop-param-row">
              <label>{props.spec.name}</label>
              <select
                title={props.spec.name}
                value={String(intValue())}
                onChange={(e) =>
                  patch({ type: 'int', value: parseInt(e.currentTarget.value, 10) || 0 })
                }
              >
                <For each={opts()}>
                  {(o) => <option value={o}>{o}</option>}
                </For>
              </select>
            </div>
          )}
        </Show>
      </Match>

      <Match when={props.spec.type === 'bool'}>
        <div class="sop-param-row">
          <label>{props.spec.name}</label>
          <input
            type="checkbox"
            title={props.spec.name}
            checked={boolValue()}
            onChange={(e) => patch({ type: 'bool', value: e.currentTarget.checked })}
          />
        </div>
      </Match>

      <Match when={props.spec.type === 'vec3'}>
        <div class="sop-param-row sop-param-vec3-row">
          <label>{props.spec.name}</label>
          <div class="sop-param-vec3-group">
            <For each={[0, 1, 2] as const}>
              {(idx) => (
                <NumberInput
                  step={0.01}
                  title={`${props.spec.name} ${idx === 0 ? 'x' : idx === 1 ? 'y' : 'z'}`}
                  value={() => vec3Value()[idx]}
                  onCommit={(v) => setVec3Axis(idx, v)}
                />
              )}
            </For>
          </div>
        </div>
      </Match>

      <Match when={props.spec.type === 'string'}>
        <div class="sop-param-row">
          <label>{props.spec.name}</label>
          <input
            type="text"
            title={props.spec.name}
            value={stringValue()}
            onInput={(e) => patch({ type: 'string', value: e.currentTarget.value })}
          />
        </div>
      </Match>
    </Switch>
  );
};
