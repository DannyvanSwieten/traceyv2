import { Component, createSignal, createEffect, For, Show, Accessor, onMount } from 'solid-js';
import * as api from '../../lib/api';
import {
  createBlankMaterialInLibrary,
  materialLibraryEntries,
  refreshMaterialLibrary,
  setMaterialEditorOpen,
} from '../../stores/materials';
import { KeyframeDot } from '../keyframe-dot/KeyframeDot';
import { NumberInput } from '../number-input/NumberInput';
import { ColorSwatch } from '../color-swatch/ColorSwatch';
import { MaterialOverride } from './MaterialOverride';
import { sopGraph } from '../../stores/sops';
import { findNodeRecursive } from '../../lib/sop_graph';
import { autoKey, setKeyAtPlayhead } from '../../stores/timeline';
import './ActorProperties.css';

export type InstanceInfo = api.InstanceInfo;
export type Transform = api.Transform;
export type Actor = api.Actor;

interface ActorPropertiesProps {
  selectedActorId: Accessor<number | null>;
  actors: Accessor<Actor[]>;
  onTransformChange?: (actorId: number, transform: Transform) => void;
}

export const ActorProperties: Component<ActorPropertiesProps> = (props) => {
  const [instances, setInstances] = createSignal<InstanceInfo[]>([]);
  const [isLoading, setIsLoading] = createSignal(false);
  const [materialBusy, setMaterialBusy] = createSignal(false);

  const selectedActor = () => {
    const id = props.selectedActorId();
    if (id === null) return null;
    return props.actors().find((a) => a.id === id) || null;
  };

  // The shared library signal is hydrated by whichever component mounts
  // first; trigger a fetch here so the dropdown is populated even if the
  // material modal hasn't been opened yet.
  onMount(refreshMaterialLibrary);

  createEffect(async () => {
    const id = props.selectedActorId();
    if (id === null) {
      setInstances([]);
      return;
    }

    setIsLoading(true);
    try {
      const actorInstances = await api.getActorInstances(id);
      setInstances(actorInstances);
    } catch (error) {
      console.error('Failed to load actor instances:', error);
      setInstances([]);
    } finally {
      setIsLoading(false);
    }
  });

  // Light-component editing. The native handler is a patch handler:
  // missing keys leave their stored value alone, so each control sends
  // only the field it touched. We always refetch the actor list after
  // each edit so the UI keeps a single source of truth even when the
  // server clamps or normalises a value (none of the current fields do,
  // but the path is the same as setActorMaterial below).
  const refreshActorsAfterLight = async () => {
    try {
      const next = await api.getAllActors();
      // The parent's actors signal is read-only here, so we mutate the
      // selected actor in place — Solid will pick up the change because
      // the selector closes over the same object reference the prop list
      // holds. Other rows are unchanged. (A future cleanup could pass a
      // setActors callback down through props for full immutability.)
      const cur = next.find((a) => a.id === props.selectedActorId());
      const local = selectedActor();
      if (cur && local) Object.assign(local, cur);
    } catch (e) {
      console.warn('actor refresh after light edit failed:', e);
    }
  };
  const editLight = async (patch: api.LightParamPatch) => {
    const id = props.selectedActorId();
    if (id === null) return;
    try {
      await api.setLightParams(id, patch);
      await refreshActorsAfterLight();
    } catch (e) {
      console.error('setLightParams failed:', e);
    }
  };
  const onMaterialPick = async (actorId: number, libraryName: string) => {
    // Sentinel: "+ New Material…" branch creates a fresh blank graph
    // in the library, assigns it to the actor, and pops the dock open
    // so the user lands in the editor ready to tweak. Auto-named to
    // skip the inline name input — the user can rename in the library
    // panel afterwards.
    if (libraryName === '__new__') {
      setMaterialBusy(true);
      try {
        const name = await createBlankMaterialInLibrary();
        await api.setActorMaterial(actorId, name);
        const actor = selectedActor();
        if (actor) actor.material_assigned = true;
        setMaterialEditorOpen(true);
      } catch (e) {
        console.error('createBlankMaterialInLibrary failed:', e);
      } finally {
        setMaterialBusy(false);
      }
      return;
    }
    setMaterialBusy(true);
    try {
      await api.setActorMaterial(actorId, libraryName);
      // Reflect the change in the local actors() list. The server already
      // updated state and recompiled the scene; we just touch the cached
      // material_assigned flag so the UI matches without a full refetch.
      const actor = selectedActor();
      if (actor) actor.material_assigned = libraryName.length > 0;
    } catch (e) {
      console.error('setActorMaterial failed:', e);
    } finally {
      setMaterialBusy(false);
    }
  };

  const updateTransform = async (
    actor: Actor,
    field: 'position' | 'scale',
    axis: 'x' | 'y' | 'z',
    value: number
  ) => {
    const newTransform: Transform = {
      position: { ...actor.transform.position },
      rotation: { ...actor.transform.rotation },
      scale: { ...actor.transform.scale },
    };
    newTransform[field][axis] = value;

    try {
      await api.setActorTransform(actor.id, newTransform);
      props.onTransformChange?.(actor.id, newTransform);
      // Auto-key: if the playbar's AK toggle is on, write a keyframe for
      // the edited component on the matching SOP node. Maps the inspector
      // field name to the SOP param name (`position` → `translate`, `scale`
      // → `scale`) and the axis to a 0..2 component index. No-op when the
      // actor has no SOP source.
      if (autoKey() && actor.sop_node_uid != null) {
        const componentIdx = axis === 'x' ? 0 : axis === 'y' ? 1 : 2;
        const paramName = field === 'position' ? 'translate' : 'scale';
        await setKeyAtPlayhead({
          nodeUid: actor.sop_node_uid,
          paramName,
          component: componentIdx,
          value,
        });
      }
    } catch (error) {
      console.error('Failed to update transform:', error);
    }
  };

  // Read the actor's `rotate_euler_deg` straight off its source SOP node so
  // the inspector mirrors the storage shape exactly (no quat → euler
  // conversion to gimbal-mangle the user's edits). Returns [0,0,0] when the
  // actor has no SOP source — those actors aren't rotatable through the
  // current UI anyway.
  const rotationEuler = (actor: Actor): [number, number, number] => {
    if (actor.sop_node_uid == null) return [0, 0, 0];
    const node = findNodeRecursive(sopGraph(), actor.sop_node_uid);
    const p = node?.params['rotate_euler_deg'];
    if (p && p.type === 'vec3') return p.value;
    return [0, 0, 0];
  };

  const handleRotationChange = async (
    actor: Actor,
    axis: 0 | 1 | 2,
    value: number,
  ) => {
    if (!Number.isFinite(value)) return;
    const cur = rotationEuler(actor);
    const next: [number, number, number] = [cur[0], cur[1], cur[2]];
    next[axis] = value;
    try {
      await api.setActorRotationEuler(actor.id, { x: next[0], y: next[1], z: next[2] });
      // Auto-key the rotated axis on rotate_euler_deg's channel.
      if (autoKey() && actor.sop_node_uid != null) {
        await setKeyAtPlayhead({
          nodeUid: actor.sop_node_uid,
          paramName: 'rotate_euler_deg',
          component: axis,
          value,
        });
      }
    } catch (e) {
      console.error('Failed to update rotation:', e);
    }
  };

  return (
    <div class="actor-properties">
      <Show
        when={selectedActor()}
        fallback={<div class="no-selection">No actor selected</div>}
      >
        {(actor) => (
          <>
            <div class="property-section">
              <h4>Actor Info</h4>
              <div class="property-row">
                <span class="property-label">Name</span>
                <span class="property-value">{actor().name}</span>
              </div>
              <div class="property-row">
                <span class="property-label">ID</span>
                <span class="property-value">{actor().id}</span>
              </div>
            </div>

            <div class="property-section">
              <h4>Transform</h4>
              <For each={[
                { field: 'position' as const, label: 'Position',     step: 0.1, paramName: 'translate' },
                { field: 'scale'    as const, label: 'Scale',        step: 0.1, paramName: 'scale' },
              ]}>
                {(g) => (
                  <div class="transform-group">
                    <span class="transform-label">{g.label}</span>
                    <div class="transform-inputs">
                      <For each={['x', 'y', 'z'] as const}>
                        {(axis, idx) => (
                          <div class="transform-input-row">
                            <label>{axis.toUpperCase()}</label>
                            <NumberInput
                              step={g.step}
                              title={`${g.label} ${axis.toUpperCase()}`}
                              value={() => actor().transform[g.field][axis]}
                              onCommit={(v) => updateTransform(actor(), g.field, axis, v)}
                            />
                            <KeyframeDot
                              nodeUid={actor().sop_node_uid}
                              paramName={g.paramName}
                              component={idx()}
                              value={() => actor().transform[g.field][axis]}
                            />
                          </div>
                        )}
                      </For>
                    </div>
                  </div>
                )}
              </For>
              <div class="transform-group">
                <span class="transform-label">Rotation (deg)</span>
                <div class="transform-inputs">
                  <For each={[0, 1, 2] as const}>
                    {(idx) => (
                      <div class="transform-input-row">
                        <label>{idx === 0 ? 'X' : idx === 1 ? 'Y' : 'Z'}</label>
                        <NumberInput
                          step={1}
                          title={`Rotation ${idx === 0 ? 'X' : idx === 1 ? 'Y' : 'Z'} (deg)`}
                          value={() => rotationEuler(actor())[idx]}
                          onCommit={(v) => {
                            void handleRotationChange(actor(), idx, v);
                          }}
                        />
                        <KeyframeDot
                          nodeUid={actor().sop_node_uid}
                          paramName="rotate_euler_deg"
                          component={idx}
                          value={() => rotationEuler(actor())[idx]}
                        />
                      </div>
                    )}
                  </For>
                </div>
              </div>
            </div>

            <Show when={actor().light}>
              {(light) => {
                const t = () => light().type;  // 0=Point 1=Distant 2=Dome 3=Area
                return (
                  <div class="property-section">
                    <h4>Light</h4>
                    <div class="property-row">
                      <span class="property-label">Type</span>
                      <select
                        class="material-picker"
                        title="Light type"
                        value={String(light().type)}
                        onChange={(e) =>
                          editLight({ type: parseInt(e.currentTarget.value, 10) })
                        }
                      >
                        <option value="2">Dome (environment)</option>
                        <option value="1">Sun (directional)</option>
                        <option value="0">Point</option>
                        <option value="3">Area (rectangle)</option>
                      </select>
                    </div>
                    <div class="transform-group">
                      <div class="light-color-header">
                        <span class="transform-label">Color</span>
                        <ColorSwatch
                          class="light-color-swatch"
                          title="Light colour (linear RGB, clamped to LDR in the picker)"
                          value={() => light().color}
                          onCommit={(rgb) => editLight({ color: rgb })}
                        />
                      </div>
                      <div class="transform-inputs">
                        <For each={['x', 'y', 'z'] as const}>
                          {(axis) => (
                            <div class="transform-input-row">
                              <label>{axis === 'x' ? 'R' : axis === 'y' ? 'G' : 'B'}</label>
                              <NumberInput
                                step={0.05}
                                min={0}
                                title={`Light color ${axis.toUpperCase()}`}
                                value={() => light().color[axis]}
                                onCommit={(v) => {
                                  const cur = light().color;
                                  editLight({ color: { ...cur, [axis]: v } });
                                }}
                              />
                            </div>
                          )}
                        </For>
                      </div>
                    </div>
                    <div class="property-row">
                      <span class="property-label">Intensity</span>
                      <NumberInput
                        step={0.1}
                        min={0}
                        title="Light intensity"
                        value={() => light().intensity}
                        onCommit={(v) => editLight({ intensity: v })}
                      />
                    </div>
                    {/* Type-conditional rows */}
                    <Show when={t() === 2 /* Dome */}>
                      <For each={[
                        { field: 'sky_color' as const,     label: 'Sky' },
                        { field: 'horizon_color' as const, label: 'Horizon' },
                        { field: 'ground_color' as const,  label: 'Ground' },
                      ]}>
                        {(g) => (
                          <div class="transform-group">
                            <div class="light-color-header">
                              <span class="transform-label">{g.label}</span>
                              <ColorSwatch
                                class="light-color-swatch"
                                title={`${g.label} colour`}
                                value={() => light()[g.field]}
                                onCommit={(rgb) =>
                                  editLight({ [g.field]: rgb } as api.LightParamPatch)
                                }
                              />
                            </div>
                            <div class="transform-inputs">
                              <For each={['x', 'y', 'z'] as const}>
                                {(axis) => (
                                  <div class="transform-input-row">
                                    <label>{axis === 'x' ? 'R' : axis === 'y' ? 'G' : 'B'}</label>
                                    <NumberInput
                                      step={0.05}
                                      min={0}
                                      title={`${g.label} color ${axis.toUpperCase()}`}
                                      value={() => light()[g.field][axis]}
                                      onCommit={(v) => {
                                        const cur = light()[g.field];
                                        editLight({ [g.field]: { ...cur, [axis]: v } } as api.LightParamPatch);
                                      }}
                                    />
                                  </div>
                                )}
                              </For>
                            </div>
                          </div>
                        )}
                      </For>
                      <div class="property-row">
                        <span class="property-label">HDRI</span>
                        <input
                          type="text"
                          placeholder="(procedural)"
                          title="HDRI path (empty = procedural sky)"
                          value={light().hdri_path}
                          onChange={(e) =>
                            editLight({ hdri_path: e.currentTarget.value })
                          }
                        />
                      </div>
                    </Show>
                    <Show when={t() === 0 /* Point */}>
                      <div class="property-row">
                        <span class="property-label">Radius</span>
                        <NumberInput
                          step={0.05}
                          min={0}
                          title="Point-light soft radius"
                          value={() => light().radius}
                          onCommit={(v) => editLight({ radius: v })}
                        />
                      </div>
                    </Show>
                    <Show when={t() === 3 /* Area */}>
                      <div class="transform-group">
                        <span class="transform-label">Size</span>
                        <div class="transform-inputs">
                          <div class="transform-input-row">
                            <label>W</label>
                            <NumberInput
                              step={0.1}
                              min={0}
                              title="Area light width"
                              value={() => light().size.x}
                              onCommit={(v) => editLight({ size: { x: v, y: light().size.y } })}
                            />
                          </div>
                          <div class="transform-input-row">
                            <label>H</label>
                            <NumberInput
                              step={0.1}
                              min={0}
                              title="Area light height"
                              value={() => light().size.y}
                              onCommit={(v) => editLight({ size: { x: light().size.x, y: v } })}
                            />
                          </div>
                        </div>
                      </div>
                    </Show>
                  </div>
                );
              }}
            </Show>

            <Show when={!actor().light}>
            <div class="property-section">
              <h4>Material</h4>
              <div class="property-row">
                <span class="property-label">Library</span>
                <select
                  class="material-picker"
                  title="Material library graph"
                  value={actor().material_assigned ? '__assigned__' : ''}
                  disabled={materialBusy()}
                  onChange={(e) => onMaterialPick(actor().id, e.currentTarget.value)}
                  onFocus={refreshMaterialLibrary}
                >
                  <option value="__new__">+ New Material…</option>
                  <option value="">— passthrough —</option>
                  {/* Two groups: project-scoped materials (ship with
                      the project file) and global materials (palette
                      shared across projects). A project entry of the
                      same name shadows a global one at cook time, so
                      we list them by-scope; the select still passes
                      just the name to setActorMaterial since the
                      cook-side resolve_material_path handles scope
                      precedence consistently. */}
                  <Show when={materialLibraryEntries().some((e) => e.scope === 'project')}>
                    <optgroup label="Project">
                      <For each={materialLibraryEntries().filter((e) => e.scope === 'project')}>
                        {(entry) => <option value={entry.name}>{entry.name}</option>}
                      </For>
                    </optgroup>
                  </Show>
                  <Show when={materialLibraryEntries().some((e) => e.scope === 'global')}>
                    <optgroup label="Global">
                      <For each={materialLibraryEntries().filter((e) => e.scope === 'global')}>
                        {(entry) => <option value={entry.name}>{entry.name}</option>}
                      </For>
                    </optgroup>
                  </Show>
                </select>
              </div>
              <Show when={actor().material_assigned}>
                <div class="property-hint">
                  Active library graph drives this actor's material program.
                  Pick "passthrough" to revert.
                </div>
              </Show>
            </div>

            {/* Inline per-object material override, edited here rather than on
                the Object Output SOP node. Only shows for actors emitted by an
                object_output SOP (the component self-gates on the source node). */}
            <MaterialOverride nodeUid={actor().sop_node_uid} />

            <div class="property-section">
              <h4>Instances ({instances().length})</h4>
              <Show
                when={!isLoading()}
                fallback={<div class="loading">Loading...</div>}
              >
                <Show
                  when={instances().length > 0}
                  fallback={<div class="no-instances">No mesh instances</div>}
                >
                  <For each={instances()}>
                    {(instance, index) => (
                      <div class="instance-item">
                        <div class="instance-header">Instance {index() + 1}</div>
                        <div class="instance-row">
                          <span class="instance-label">Mesh</span>
                          <span class="instance-value">{instance.object_ref || 'N/A'}</span>
                        </div>
                        <div class="instance-row">
                          <span class="instance-label">Shader</span>
                          <span class="instance-value">{instance.shader_id || 'Default'}</span>
                        </div>
                      </div>
                    )}
                  </For>
                </Show>
              </Show>
            </div>
            </Show>
          </>
        )}
      </Show>
    </div>
  );
};
