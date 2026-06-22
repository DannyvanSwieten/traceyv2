// Right panel: parameter editor for the selected VOP node. Mirrors
// SopNodeInspector — the param-row UI is identical because both stores ship
// the same ParamValue shape.

import { Component, For, Index, Match, Show, Switch, createMemo } from 'solid-js';
import {
  ParamValue,
  VopNode,
  InputDefault,
  lookupCatalog,
} from '../../lib/vop_graph';
import {
  vopGraph,
  selectedNode,
  setParam,
  setInputDefault,
  promoteParamToHost,
} from '../../stores/vops';
import type { ParamSpec, PortSpec } from '../../lib/vop_graph';
import type { ParamValueVec3 } from '../../lib/sop_graph';
import { humanizeParamName } from '../../lib/param_label';
import { NumberInput } from '../number-input/NumberInput';

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
              {/* Index (not For) keeps the row DOM stable across setParam
                  store updates — critical for sliders, whose pointer
                  capture would otherwise die on every drag tick. See
                  SopNodeInspector for the full rationale. */}
              <Index each={entry()?.params ?? []}>
                {(spec) => (
                  <ParamRow
                    node={n()}
                    spec={spec()}
                    onPromote={async () => {
                      const hostName = await promoteParamToHost(n().uid, spec().name);
                      if (!hostName) {
                        console.warn('promote failed');
                      }
                    }}
                  />
                )}
              </Index>
              <InputDefaultsSection node={n()} />
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
  const displayName = () => humanizeParamName(props.spec.name);

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

  // Catalog UI hints (see lib/sop_graph.ts ParamSpec): `range` turns a
  // number input into a slider with a readout; `options` turns a string/int
  // input into a named dropdown. Mirrors SopNodeInspector.
  const range = () => props.spec.range;
  const options = () => props.spec.options;

  // Reactive type dispatch — see SopNodeInspector's matching block for
  // the full rationale. tl;dr: <Switch>/<Match> re-evaluates when
  // spec.type flips, so the row swaps to the right layout without
  // forcing a remount of the parent Index loop.
  const floatValue  = () => (cur() as { value: number } | undefined)?.value ?? 0;
  const intValue    = () => (cur() as { value: number } | undefined)?.value ?? 0;
  const boolValue   = () => (cur() as { value: boolean } | undefined)?.value ?? false;
  const stringValue = () => (cur() as { value: string } | undefined)?.value ?? '';
  const vec3Value   = () =>
    (cur() as ParamValueVec3 | undefined)?.value ?? ([0, 0, 0] as [number, number, number]);
  const setVec3Comp = (i: 0 | 1 | 2, val: number) => {
    const cv = vec3Value().slice() as [number, number, number];
    cv[i] = val;
    patch({ type: 'vec3', value: cv });
  };

  return (
    <Switch>
      <Match when={props.spec.type === 'float'}>
        <Show
          when={(() => { const r = range(); return r && r.min !== r.max ? r : null; })()}
          fallback={
            <div class="sop-param-row">
              <label>{displayName()}</label>
              <NumberInput
                step={0.01}
                title={props.spec.name}
                value={floatValue}
                onCommit={(v) => patch({ type: 'float', value: v })}
              />
              <PromoteBtn />
            </div>
          }
        >
          {(r) => {
            const step = r().step > 0 ? r().step : (r().max - r().min) / 200;
            return (
              <div class="sop-param-row sop-param-slider-row">
                <label>{displayName()}</label>
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
                <PromoteBtn />
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
                  <label>{displayName()}</label>
                  <NumberInput
                    step={1}
                    decimals={0}
                    title={props.spec.name}
                    value={intValue}
                    onCommit={(v) => patch({ type: 'int', value: Math.round(v) })}
                  />
                  <PromoteBtn />
                </div>
              }
            >
              {(r) => {
                const step = r().step > 0 ? r().step : 1;
                return (
                  <div class="sop-param-row sop-param-slider-row">
                    <label>{displayName()}</label>
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
                    <PromoteBtn />
                  </div>
                );
              }}
            </Show>
          }
        >
          {(opts) => (
            <div class="sop-param-row">
              <label>{displayName()}</label>
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
              <PromoteBtn />
            </div>
          )}
        </Show>
      </Match>

      <Match when={props.spec.type === 'bool'}>
        <div class="sop-param-row">
          <label>{displayName()}</label>
          <input
            type="checkbox"
            title={props.spec.name}
            checked={boolValue()}
            onChange={(e) => patch({ type: 'bool', value: e.currentTarget.checked })}
          />
          <PromoteBtn />
        </div>
      </Match>

      <Match when={props.spec.type === 'vec3'}>
        <div class="sop-param-row sop-param-vec3">
          <label>{displayName()}</label>
          <div class="sop-param-vec3-fields">
            <NumberInput step={0.01} title={`${props.spec.name} x`}
                         value={() => vec3Value()[0]}
                         onCommit={(n) => setVec3Comp(0, n)} />
            <NumberInput step={0.01} title={`${props.spec.name} y`}
                         value={() => vec3Value()[1]}
                         onCommit={(n) => setVec3Comp(1, n)} />
            <NumberInput step={0.01} title={`${props.spec.name} z`}
                         value={() => vec3Value()[2]}
                         onCommit={(n) => setVec3Comp(2, n)} />
          </div>
          <PromoteBtn />
        </div>
      </Match>

      <Match when={props.spec.type === 'string'}>
        <Show
          when={(() => { const o = options(); return o && o.length > 0 ? o : null; })()}
          fallback={
            <div class="sop-param-row">
              <label>{displayName()}</label>
              <input
                type="text"
                title={props.spec.name}
                value={stringValue()}
                onChange={(e) => patch({ type: 'string', value: e.currentTarget.value })}
              />
            </div>
          }
        >
          {(opts) => (
            <div class="sop-param-row">
              <label>{displayName()}</label>
              <select
                title={props.spec.name}
                value={stringValue()}
                onChange={(e) => patch({ type: 'string', value: e.currentTarget.value })}
              >
                <For each={opts()}>
                  {(label) => <option value={label}>{label}</option>}
                </For>
              </select>
            </div>
          )}
        </Show>
      </Match>
    </Switch>
  );
};

// ── Input-constant editors ────────────────────────────────────────────────
// Houdini-style "type into the input port" UX: every unconnected input
// gets a small editor backed by node.input_defaults[port]. When the user
// wires the input, the row disappears (the wire is the value); when the
// user disconnects, the stored constant is still there and the editor
// reappears with the last value.
const InputDefaultsSection: Component<{ node: VopNode }> = (props) => {
  const inputs = (): PortSpec[] => lookupCatalog(props.node.kind)?.inputs ?? [];
  // Reactive: rebuild whenever connections change. Looking up
  // vopGraph().connections inside the closure keeps Solid tracking it.
  const isConnected = (portIdx: number): boolean =>
    vopGraph().connections.some(
      (c) => c.to_node === props.node.uid && c.to_port === portIdx,
    );

  return (
    <Show when={inputs().length > 0}>
      <div class="sop-inspector-section-label">Inputs</div>
      <Index each={inputs()}>
        {(port, i) => (
          <Show when={!isConnected(i)}>
            <InputDefaultRow node={props.node} port={port()} portIdx={i} />
          </Show>
        )}
      </Index>
    </Show>
  );
};

interface InputDefaultRowProps {
  node: VopNode;
  port: PortSpec;
  portIdx: number;
}

const InputDefaultRow: Component<InputDefaultRowProps> = (props) => {
  const stored = (): InputDefault | undefined =>
    props.node.input_defaults?.[String(props.portIdx)];

  // Inspector defaults — used when the user hasn't typed anything yet, so
  // the editor shows a sensible starting value rather than "undefined".
  // Matches the per-node zero fallback in evaluate().
  const dt = props.port.data_type ?? 'unknown';

  const set = (v: InputDefault | undefined) =>
    setInputDefault(props.node.uid, props.portIdx, v);

  if (dt === 'float' || dt === 'int') {
    const value = () => {
      const s = stored();
      if (s && (s.type === 'float' || s.type === 'int')) return s.value;
      return 0;
    };
    const isInt = dt === 'int';
    return (
      <div class="sop-param-row">
        <label>{props.port.name}</label>
        <NumberInput
          step={isInt ? 1 : 0.01}
          decimals={isInt ? 0 : 2}
          title={`${props.port.name} (unwired constant)`}
          value={value}
          onCommit={(n) =>
            set({ type: isInt ? 'int' : 'float', value: isInt ? Math.round(n) : n })
          }
        />
      </div>
    );
  }
  if (dt === 'vec3') {
    const v = (): [number, number, number] => {
      const s = stored();
      if (s && s.type === 'vec3') return s.value;
      return [0, 0, 0];
    };
    const setComp = (i: 0 | 1 | 2, val: number) => {
      const cv = v().slice() as [number, number, number];
      cv[i] = val;
      set({ type: 'vec3', value: cv });
    };
    return (
      <div class="sop-param-row sop-param-vec3">
        <label>{props.port.name}</label>
        <div class="sop-param-vec3-fields">
          <NumberInput step={0.01} title={`${props.port.name}.x`}
                       value={() => v()[0]}
                       onCommit={(n) => setComp(0, n)} />
          <NumberInput step={0.01} title={`${props.port.name}.y`}
                       value={() => v()[1]}
                       onCommit={(n) => setComp(1, n)} />
          <NumberInput step={0.01} title={`${props.port.name}.z`}
                       value={() => v()[2]}
                       onCommit={(n) => setComp(2, n)} />
        </div>
      </div>
    );
  }
  // Unknown / unsupported port type — skip the editor rather than show
  // a broken control. Bool / vec2 / vec4 / matrix can be added when a
  // node needs them.
  return null;
};
