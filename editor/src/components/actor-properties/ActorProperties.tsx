import { Component, createSignal, createEffect, For, Show, Accessor } from 'solid-js';
import { invoke } from '@tauri-apps/api/core';
import './ActorProperties.css';

export interface InstanceInfo {
  object_ref: string;
  shader_id: string;
  has_local_transform: boolean;
  local_transform?: {
    position: { x: number; y: number; z: number };
    rotation: { w: number; x: number; y: number; z: number };
    scale: { x: number; y: number; z: number };
  };
}

export interface Transform {
  position: { x: number; y: number; z: number };
  rotation: { w: number; x: number; y: number; z: number };
  scale: { x: number; y: number; z: number };
}

export interface Actor {
  id: number;
  name: string;
  transform: Transform;
  children: number[];
}

interface ActorPropertiesProps {
  selectedActorId: Accessor<number | null>;
  actors: Accessor<Actor[]>;
  onTransformChange?: (actorId: number, transform: Transform) => void;
}

export const ActorProperties: Component<ActorPropertiesProps> = (props) => {
  const [instances, setInstances] = createSignal<InstanceInfo[]>([]);
  const [isLoading, setIsLoading] = createSignal(false);

  const selectedActor = () => {
    const id = props.selectedActorId();
    if (id === null) return null;
    return props.actors().find((a) => a.id === id) || null;
  };

  createEffect(async () => {
    const id = props.selectedActorId();
    if (id === null) {
      setInstances([]);
      return;
    }

    setIsLoading(true);
    try {
      const actorInstances = await invoke<InstanceInfo[]>('get_actor_instances', {
        actorId: id,
      });
      setInstances(actorInstances);
    } catch (error) {
      console.error('Failed to load actor instances:', error);
      setInstances([]);
    } finally {
      setIsLoading(false);
    }
  });

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
      await invoke('set_actor_transform', {
        actorId: actor.id,
        transform: newTransform,
      });
      props.onTransformChange?.(actor.id, newTransform);
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
                  </div>
                </div>
              </div>
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
