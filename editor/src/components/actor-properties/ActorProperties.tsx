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

  // Convert quaternion to Euler angles (in degrees)
  const quatToEuler = (q: { w: number; x: number; y: number; z: number }) => {
    // Roll (x-axis rotation)
    const sinr_cosp = 2 * (q.w * q.x + q.y * q.z);
    const cosr_cosp = 1 - 2 * (q.x * q.x + q.y * q.y);
    const roll = Math.atan2(sinr_cosp, cosr_cosp);

    // Pitch (y-axis rotation)
    const sinp = 2 * (q.w * q.y - q.z * q.x);
    const pitch = Math.abs(sinp) >= 1
      ? Math.sign(sinp) * Math.PI / 2
      : Math.asin(sinp);

    // Yaw (z-axis rotation)
    const siny_cosp = 2 * (q.w * q.z + q.x * q.y);
    const cosy_cosp = 1 - 2 * (q.y * q.y + q.z * q.z);
    const yaw = Math.atan2(siny_cosp, cosy_cosp);

    return {
      x: roll * 180 / Math.PI,
      y: pitch * 180 / Math.PI,
      z: yaw * 180 / Math.PI,
    };
  };

  // Convert Euler angles (in degrees) to quaternion
  const eulerToQuat = (euler: { x: number; y: number; z: number }) => {
    const roll = euler.x * Math.PI / 180;
    const pitch = euler.y * Math.PI / 180;
    const yaw = euler.z * Math.PI / 180;

    const cy = Math.cos(yaw * 0.5);
    const sy = Math.sin(yaw * 0.5);
    const cp = Math.cos(pitch * 0.5);
    const sp = Math.sin(pitch * 0.5);
    const cr = Math.cos(roll * 0.5);
    const sr = Math.sin(roll * 0.5);

    return {
      w: cr * cp * cy + sr * sp * sy,
      x: sr * cp * cy - cr * sp * sy,
      y: cr * sp * cy + sr * cp * sy,
      z: cr * cp * sy - sr * sp * cy,
    };
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

  const updateRotation = async (
    actor: Actor,
    axis: 'x' | 'y' | 'z',
    degrees: number
  ) => {
    // Get current Euler angles
    const currentEuler = quatToEuler(actor.transform.rotation);

    // Update the specified axis
    const newEuler = { ...currentEuler, [axis]: degrees };

    // Convert back to quaternion
    const newQuat = eulerToQuat(newEuler);

    const newTransform: Transform = {
      position: { ...actor.transform.position },
      rotation: newQuat,
      scale: { ...actor.transform.scale },
    };

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

  const handleRotationChange = (
    actor: Actor,
    axis: 'x' | 'y' | 'z',
    inputValue: string
  ) => {
    const degrees = parseFloat(inputValue);
    if (!isNaN(degrees)) {
      updateRotation(actor, axis, degrees);
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
                <span class="transform-label">Rotation</span>
                <div class="transform-inputs">
                  <div class="transform-input-row">
                    <label for={`rot-x-${actor().id}`}>X</label>
                    <input
                      id={`rot-x-${actor().id}`}
                      type="number"
                      step="1"
                      value={quatToEuler(actor().transform.rotation).x.toFixed(1)}
                      onChange={(e) =>
                        handleRotationChange(actor(), 'x', e.currentTarget.value)
                      }
                    />
                  </div>
                  <div class="transform-input-row">
                    <label for={`rot-y-${actor().id}`}>Y</label>
                    <input
                      id={`rot-y-${actor().id}`}
                      type="number"
                      step="1"
                      value={quatToEuler(actor().transform.rotation).y.toFixed(1)}
                      onChange={(e) =>
                        handleRotationChange(actor(), 'y', e.currentTarget.value)
                      }
                    />
                  </div>
                  <div class="transform-input-row">
                    <label for={`rot-z-${actor().id}`}>Z</label>
                    <input
                      id={`rot-z-${actor().id}`}
                      type="number"
                      step="1"
                      value={quatToEuler(actor().transform.rotation).z.toFixed(1)}
                      onChange={(e) =>
                        handleRotationChange(actor(), 'z', e.currentTarget.value)
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
