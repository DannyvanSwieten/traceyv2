import { Component, createSignal, createEffect, For, Show, Accessor, onMount } from 'solid-js';
import * as api from '../../lib/api';
import {
  createBlankMaterialInLibrary,
  materialLibraryEntries,
  refreshMaterialLibrary,
  setMaterialEditorOpen,
} from '../../stores/materials';
import { KeyframeDot } from '../keyframe-dot/KeyframeDot';
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

  const handleInputChange = (
    actor: Actor,
    field: 'position' | 'scale',
    axis: 'x' | 'y' | 'z',
    inputValue: string
  ) => {
    const value = parseFloat(inputValue);
    if (!isNaN(value)) {
      updateTransform(actor, field, axis, value);
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
    inputValue: string,
  ) => {
    const value = parseFloat(inputValue);
    if (Number.isNaN(value)) return;
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
              <div class="transform-group">
                <span class="transform-label">Position</span>
                <div class="transform-inputs">
                  <div class="transform-input-row">
                    <label for={`pos-x-${actor().id}`}>X</label>
                    <input
                      id={`pos-x-${actor().id}`}
                      type="number"
                      step="0.1"
                      value={actor().transform.position.x.toFixed(3)}
                      onChange={(e) =>
                        handleInputChange(actor(), 'position', 'x', e.currentTarget.value)
                      }
                    />
                    <KeyframeDot
                      nodeUid={actor().sop_node_uid}
                      paramName="translate"
                      component={0}
                      value={() => actor().transform.position.x}
                    />
                  </div>
                  <div class="transform-input-row">
                    <label for={`pos-y-${actor().id}`}>Y</label>
                    <input
                      id={`pos-y-${actor().id}`}
                      type="number"
                      step="0.1"
                      value={actor().transform.position.y.toFixed(3)}
                      onChange={(e) =>
                        handleInputChange(actor(), 'position', 'y', e.currentTarget.value)
                      }
                    />
                    <KeyframeDot
                      nodeUid={actor().sop_node_uid}
                      paramName="translate"
                      component={1}
                      value={() => actor().transform.position.y}
                    />
                  </div>
                  <div class="transform-input-row">
                    <label for={`pos-z-${actor().id}`}>Z</label>
                    <input
                      id={`pos-z-${actor().id}`}
                      type="number"
                      step="0.1"
                      value={actor().transform.position.z.toFixed(3)}
                      onChange={(e) =>
                        handleInputChange(actor(), 'position', 'z', e.currentTarget.value)
                      }
                    />
                    <KeyframeDot
                      nodeUid={actor().sop_node_uid}
                      paramName="translate"
                      component={2}
                      value={() => actor().transform.position.z}
                    />
                  </div>
                </div>
              </div>
              <div class="transform-group">
                <span class="transform-label">Rotation (deg)</span>
                <div class="transform-inputs">
                  <div class="transform-input-row">
                    <label for={`rot-x-${actor().id}`}>X</label>
                    <input
                      id={`rot-x-${actor().id}`}
                      type="number"
                      step="1"
                      value={rotationEuler(actor())[0].toFixed(2)}
                      onChange={(e) =>
                        handleRotationChange(actor(), 0, e.currentTarget.value)
                      }
                    />
                    <KeyframeDot
                      nodeUid={actor().sop_node_uid}
                      paramName="rotate_euler_deg"
                      component={0}
                      value={() => rotationEuler(actor())[0]}
                    />
                  </div>
                  <div class="transform-input-row">
                    <label for={`rot-y-${actor().id}`}>Y</label>
                    <input
                      id={`rot-y-${actor().id}`}
                      type="number"
                      step="1"
                      value={rotationEuler(actor())[1].toFixed(2)}
                      onChange={(e) =>
                        handleRotationChange(actor(), 1, e.currentTarget.value)
                      }
                    />
                    <KeyframeDot
                      nodeUid={actor().sop_node_uid}
                      paramName="rotate_euler_deg"
                      component={1}
                      value={() => rotationEuler(actor())[1]}
                    />
                  </div>
                  <div class="transform-input-row">
                    <label for={`rot-z-${actor().id}`}>Z</label>
                    <input
                      id={`rot-z-${actor().id}`}
                      type="number"
                      step="1"
                      value={rotationEuler(actor())[2].toFixed(2)}
                      onChange={(e) =>
                        handleRotationChange(actor(), 2, e.currentTarget.value)
                      }
                    />
                    <KeyframeDot
                      nodeUid={actor().sop_node_uid}
                      paramName="rotate_euler_deg"
                      component={2}
                      value={() => rotationEuler(actor())[2]}
                    />
                  </div>
                </div>
              </div>
              <div class="transform-group">
                <span class="transform-label">Scale</span>
                <div class="transform-inputs">
                  <div class="transform-input-row">
                    <label for={`scale-x-${actor().id}`}>X</label>
                    <input
                      id={`scale-x-${actor().id}`}
                      type="number"
                      step="0.1"
                      value={actor().transform.scale.x.toFixed(3)}
                      onChange={(e) =>
                        handleInputChange(actor(), 'scale', 'x', e.currentTarget.value)
                      }
                    />
                    <KeyframeDot
                      nodeUid={actor().sop_node_uid}
                      paramName="scale"
                      component={0}
                      value={() => actor().transform.scale.x}
                    />
                  </div>
                  <div class="transform-input-row">
                    <label for={`scale-y-${actor().id}`}>Y</label>
                    <input
                      id={`scale-y-${actor().id}`}
                      type="number"
                      step="0.1"
                      value={actor().transform.scale.y.toFixed(3)}
                      onChange={(e) =>
                        handleInputChange(actor(), 'scale', 'y', e.currentTarget.value)
                      }
                    />
                    <KeyframeDot
                      nodeUid={actor().sop_node_uid}
                      paramName="scale"
                      component={1}
                      value={() => actor().transform.scale.y}
                    />
                  </div>
                  <div class="transform-input-row">
                    <label for={`scale-z-${actor().id}`}>Z</label>
                    <input
                      id={`scale-z-${actor().id}`}
                      type="number"
                      step="0.1"
                      value={actor().transform.scale.z.toFixed(3)}
                      onChange={(e) =>
                        handleInputChange(actor(), 'scale', 'z', e.currentTarget.value)
                      }
                    />
                    <KeyframeDot
                      nodeUid={actor().sop_node_uid}
                      paramName="scale"
                      component={2}
                      value={() => actor().transform.scale.z}
                    />
                  </div>
                </div>
              </div>
            </div>

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
          </>
        )}
      </Show>
    </div>
  );
};
