// Right panel: parameter editor for the selected SOP node. Driven by the
// catalog's param specs — each parameter renders an input typed by its
// declared ParamType (float / int / bool / vec3 / string).

import { Component, For, Index, Match, Show, Switch, createMemo } from 'solid-js';
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
import { NumberInput } from '../number-input/NumberInput';

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
  //
  // For promoted params, we ALSO forward the VOP-side ParamSpec hints
  // (range / options) so a promoted "frequency" slider stays a slider
  // on the host SOP. The host emits these alongside each promotion in
  // `extra.promotions` — see attribute_vop_sop.cpp serializeExtraJson().
  const renderedParams = createMemo<ParamSpec[]>(() => {
    const n = node();
    if (!n) return [];
    const fromCatalog = entry()?.params ?? [];
    const known = new Set(fromCatalog.map((p) => p.name));
    // Index promotions by host param name so we can pull range / options
    // when synthesising the extra ParamSpec.
    type Promo = {
      host_param_name?: string;
      range?: { min: number; max: number; step: number };
      options?: string[];
    };
    const extra = (n as { extra?: { promotions?: Promo[] } }).extra;
    const promotionByHostName = new Map<string, Promo>();
    if (extra?.promotions) {
      for (const p of extra.promotions) {
        if (p.host_param_name) promotionByHostName.set(p.host_param_name, p);
      }
    }
    const extras: ParamSpec[] = [];
    for (const [name, value] of Object.entries(n.params)) {
      if (known.has(name)) continue;
      const promo = promotionByHostName.get(name);
      extras.push({
        name,
        type: value.type as ParamType,
        default: '',
        range: promo?.range,
        options: promo?.options,
      });
    }
    return [...fromCatalog, ...extras];
  });

  // Promoted host params come from the host SOP's `extra.promotions` block.
  // We surface them in the inspector with a tiny "↑" affordance — clicking
  // demotes the param, restoring catalog-only state. Catalog-declared params
  // are not in this set.
  const promotedHostParamNames = createMemo<Set<string>>(() => {
    const n = node();
    if (!n || (n.kind !== 'attribute_vop' && n.kind !== 'instance_vop')) return new Set();
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
              {/* Index (not For): the rendered-params array gets a fresh
                  identity on every setParam (the memo synthesises new
                  ParamSpec objects for promoted extras), and For would
                  unmount/remount each row on every drag tick — killing
                  the slider's pointer capture mid-drag. Index keeps the
                  row mounted as long as the array length is stable, and
                  reactivity propagates spec changes via the accessor. */}
              <Index each={renderedParams()}>
                {(spec) => (
                  <ParamRow
                    node={n()}
                    spec={spec()}
                    promoted={promotedHostParamNames().has(spec().name)}
                    onDemote={() => demote(spec().name)}
                  />
                )}
              </Index>
            </div>
            <Show when={n().kind === 'attribute_vop' || n().kind === 'instance_vop'}>
              <button
                class="sop-inspector-action"
                type="button"
                onClick={() => openVopEditor(n().uid)}
              >
                {n().kind === 'instance_vop' ? 'Edit Instance VOP…' : 'Edit VOP Graph'}
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
  // Render hints from the catalog: a `range` field turns numeric inputs
  // into a slider with a live numeric readout; an `options` array turns
  // a string/int input into a named dropdown. Both fall back to the
  // plain input when the hint is absent.
  const range = () => props.spec.range;
  const options = () => props.spec.options;

  // Reactive type dispatch via <Switch>/<Match>. The previous code used
  // a plain `switch (props.spec.type)` which evaluated ONCE at mount —
  // perfect for a fresh row, broken when the same ParamRow instance is
  // reused for a DIFFERENT node kind whose param at the same position
  // has a different type (e.g. switching from pop_source/rate=float to
  // pop_wind/direction=vec3 left the float-slider JSX in place,
  // rendering 3 number boxes for a slider param). <Match when={...}>
  // re-evaluates when spec.type flips, so the row swaps to the right
  // layout without forcing a remount (which would kill mid-drag slider
  // pointer capture).
  const floatValue   = () => (cur() as { value: number } | undefined)?.value ?? 0;
  const intValue     = () => (cur() as { value: number } | undefined)?.value ?? 0;
  const boolValue    = () => (cur() as { value: boolean } | undefined)?.value ?? false;
  const stringValue  = () => (cur() as { value: string } | undefined)?.value ?? '';
  const vec3Value    = () =>
    (cur() as ParamValueVec3 | undefined)?.value ?? ([0, 0, 0] as [number, number, number]);
  const setVec3Comp = (i: 0 | 1 | 2, val: number) => {
    const cv = vec3Value().slice() as [number, number, number];
    cv[i] = val;
    patch({ type: 'vec3', value: cv }, i);
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
              <DemoteBtn />
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
                    title={props.spec.name}
                    min={r().min}
                    max={r().max}
                    step={step}
                    value={floatValue()}
                    onInput={(e) =>
                      patch({ type: 'float', value: parseFloat(e.currentTarget.value) || 0 })
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
                <DemoteBtn />
              </div>
            );
          }}
        </Show>
      </Match>

      <Match when={props.spec.type === 'int'}>
        <Show
          when={(() => { const o = options(); return o && o.length > 0 ? o : null; })()}
          fallback={
            <Show
              when={(() => { const r = range(); return r && r.min !== r.max ? r : null; })()}
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
                  <DemoteBtn />
                </div>
              }
            >
              {(r) => {
                const step = r().step > 0 ? r().step : 1;
                return (
                  <div class="sop-param-row sop-param-slider-row">
                    <label>{props.spec.name}</label>
                    <div class="sop-param-slider-group">
                      <input
                        type="range"
                        class="sop-param-slider"
                        title={props.spec.name}
                        min={r().min}
                        max={r().max}
                        step={step}
                        value={intValue()}
                        onInput={(e) =>
                          patch({ type: 'int', value: parseInt(e.currentTarget.value, 10) || 0 })
                        }
                      />
                      <NumberInput
                        class="sop-param-slider-readout"
                        step={step}
                        decimals={0}
                        title={`${props.spec.name} (value)`}
                        value={intValue}
                        onCommit={(v) => patch({ type: 'int', value: Math.round(v) })}
                      />
                    </div>
                    <DemoteBtn />
                  </div>
                );
              }}
            </Show>
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
                  {(label, i) => <option value={String(i())}>{label}</option>}
                </For>
              </select>
              <DemoteBtn />
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
          <DemoteBtn />
        </div>
      </Match>

      <Match when={props.spec.type === 'vec3'}>
        <div class="sop-param-row sop-param-vec3">
          <label>{props.spec.name}</label>
          <div class="sop-param-vec3-fields">
            <NumberInput step={0.01} title={`${props.spec.name}.x`}
                         value={() => vec3Value()[0]}
                         onCommit={(n) => setVec3Comp(0, n)} />
            <NumberInput step={0.01} title={`${props.spec.name}.y`}
                         value={() => vec3Value()[1]}
                         onCommit={(n) => setVec3Comp(1, n)} />
            <NumberInput step={0.01} title={`${props.spec.name}.z`}
                         value={() => vec3Value()[2]}
                         onCommit={(n) => setVec3Comp(2, n)} />
          </div>
          <DemoteBtn />
        </div>
      </Match>

      <Match when={props.spec.type === 'string'}>
        <Show
          when={(() => { const o = options(); return o && o.length > 0 ? o : null; })()}
          fallback={
            <div class="sop-param-row">
              <label>{props.spec.name}</label>
              <input
                type="text"
                title={props.spec.name}
                value={stringValue()}
                onChange={(e) => patch({ type: 'string', value: e.currentTarget.value })}
              />
              <DemoteBtn />
            </div>
          }
        >
          {(opts) => (
            <div class="sop-param-row">
              <label>{props.spec.name}</label>
              <select
                title={props.spec.name}
                value={stringValue()}
                onChange={(e) => patch({ type: 'string', value: e.currentTarget.value })}
              >
                <For each={opts()}>
                  {(label) => <option value={label}>{label}</option>}
                </For>
              </select>
              <DemoteBtn />
            </div>
          )}
        </Show>
      </Match>
    </Switch>
  );
};
