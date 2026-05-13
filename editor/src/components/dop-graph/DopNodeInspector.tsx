// Right panel: parameter editor for the selected DOP node. Same row UI
// as SopNodeInspector / VopNodeInspector — both stores ship the same
// ParamValue shape. Subset of the VOP inspector: no promote button,
// no input-defaults section (DOP nodes don't expose port-input
// constants in v1). Range sliders are supported because particle params
// (rate, lifetime, jitter) benefit from them.
//
// pop_force gets a special "Edit Force VOP…" button that opens the
// existing per-host VOP editor against this DOP node's uid.

import { Component, Index, Show, createMemo } from 'solid-js';
import {
  ParamValue,
  DopNode,
  ParamSpec,
  lookupCatalog,
} from '../../lib/dop_graph';
import { dopGraph, selectedNode, setParam } from '../../stores/dops';
import { openVopEditor } from '../../stores/vops';
import type { ParamValueVec3 } from '../../lib/sop_graph';

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
              {/* Index keeps row DOM stable so slider drags don't die
                  on every setParam tick. */}
              <Index each={entry()?.params ?? []}>
                {(spec) => <ParamRow node={n()} spec={spec()} />}
              </Index>
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

  switch (props.spec.type) {
    case 'float': {
      const value = () => (cur() as { value: number } | undefined)?.value ?? 0;
      const r = range();
      if (r && r.min !== r.max) {
        const step = r.step > 0 ? r.step : (r.max - r.min) / 200;
        return (
          <div class="sop-param-row sop-param-slider-row">
            <label>{props.spec.name}</label>
            <div class="sop-param-slider-group">
              <input
                type="range"
                class="sop-param-slider"
                min={r.min}
                max={r.max}
                step={step}
                value={value()}
                onInput={(e) =>
                  patch({ type: 'float', value: parseFloat(e.currentTarget.value) })
                }
              />
              <input
                type="number"
                class="sop-param-slider-readout"
                value={value()}
                step={step}
                onInput={(e) =>
                  patch({ type: 'float', value: parseFloat(e.currentTarget.value) || 0 })
                }
              />
            </div>
          </div>
        );
      }
      return (
        <div class="sop-param-row">
          <label>{props.spec.name}</label>
          <input
            type="number"
            value={value()}
            onInput={(e) =>
              patch({ type: 'float', value: parseFloat(e.currentTarget.value) || 0 })
            }
          />
        </div>
      );
    }
    case 'int': {
      const value = () => (cur() as { value: number } | undefined)?.value ?? 0;
      const opts = options();
      if (opts && opts.length > 0) {
        return (
          <div class="sop-param-row">
            <label>{props.spec.name}</label>
            <select
              value={String(value())}
              onChange={(e) =>
                patch({ type: 'int', value: parseInt(e.currentTarget.value, 10) || 0 })
              }
            >
              <Index each={opts}>
                {(o) => <option value={o()}>{o()}</option>}
              </Index>
            </select>
          </div>
        );
      }
      return (
        <div class="sop-param-row">
          <label>{props.spec.name}</label>
          <input
            type="number"
            step={1}
            value={value()}
            onInput={(e) =>
              patch({ type: 'int', value: parseInt(e.currentTarget.value, 10) || 0 })
            }
          />
        </div>
      );
    }
    case 'bool': {
      const value = () => (cur() as { value: boolean } | undefined)?.value ?? false;
      return (
        <div class="sop-param-row">
          <label>{props.spec.name}</label>
          <input
            type="checkbox"
            checked={value()}
            onChange={(e) => patch({ type: 'bool', value: e.currentTarget.checked })}
          />
        </div>
      );
    }
    case 'vec3': {
      const value = () =>
        ((cur() as ParamValueVec3 | undefined)?.value ?? [0, 0, 0]) as [number, number, number];
      function patchAxis(axis: 0 | 1 | 2, v: number) {
        const cv = value();
        const next: [number, number, number] = [cv[0], cv[1], cv[2]];
        next[axis] = v;
        patch({ type: 'vec3', value: next });
      }
      return (
        <div class="sop-param-row sop-param-vec3-row">
          <label>{props.spec.name}</label>
          <div class="sop-param-vec3-group">
            <Index each={[0, 1, 2] as const}>
              {(idx) => (
                <input
                  type="number"
                  value={value()[idx()]}
                  onInput={(e) =>
                    patchAxis(idx(), parseFloat(e.currentTarget.value) || 0)
                  }
                />
              )}
            </Index>
          </div>
        </div>
      );
    }
    case 'string': {
      const value = () => (cur() as { value: string } | undefined)?.value ?? '';
      return (
        <div class="sop-param-row">
          <label>{props.spec.name}</label>
          <input
            type="text"
            value={value()}
            onInput={(e) => patch({ type: 'string', value: e.currentTarget.value })}
          />
        </div>
      );
    }
  }
};
