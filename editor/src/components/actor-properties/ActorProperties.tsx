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

// Material property types from Rust
interface Vec3 {
  x: number;
  y: number;
  z: number;
}

interface Vec4 {
  x: number;
  y: number;
  z: number;
  w: number;
}

type MaterialPropertyValue =
  | { type: 'Float'; value: number }
  | { type: 'Vec3'; value: Vec3 }
  | { type: 'Vec4'; value: Vec4 }
  | { type: 'Int'; value: number }
  | { type: 'Texture'; value: string };

interface MaterialProperty {
  name: string;
  value: MaterialPropertyValue;
}

// Common PBR property definitions for the UI
const COMMON_MATERIAL_PROPERTIES = [
  { name: 'albedo', displayName: 'Albedo', type: 'color' as const },
  { name: 'metallic', displayName: 'Metallic', type: 'float' as const, min: 0, max: 1, step: 0.01 },
  { name: 'roughness', displayName: 'Roughness', type: 'float' as const, min: 0, max: 1, step: 0.01 },
  { name: 'emission', displayName: 'Emission', type: 'color' as const },
  // Clearcoat layer (car paint, lacquered surfaces)
  { name: 'clearcoat', displayName: 'Clearcoat', type: 'float' as const, min: 0, max: 1, step: 0.01 },
  { name: 'clearcoatRoughness', displayName: 'Clearcoat Roughness', type: 'float' as const, min: 0, max: 1, step: 0.01 },
  // Sheen layer (fabric, velvet, cloth)
  { name: 'sheenColor', displayName: 'Sheen Color', type: 'color' as const },
  { name: 'sheenRoughness', displayName: 'Sheen Roughness', type: 'float' as const, min: 0, max: 1, step: 0.01 },
];

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

// Helper to convert Vec3 to hex color
const vec3ToHex = (v: Vec3): string => {
  const r = Math.min(255, Math.max(0, Math.round(v.x * 255)));
  const g = Math.min(255, Math.max(0, Math.round(v.y * 255)));
  const b = Math.min(255, Math.max(0, Math.round(v.z * 255)));
  return `#${r.toString(16).padStart(2, '0')}${g.toString(16).padStart(2, '0')}${b.toString(16).padStart(2, '0')}`;
};

// Helper to convert hex color to Vec3
const hexToVec3 = (hex: string): Vec3 => {
  const result = /^#?([a-f\d]{2})([a-f\d]{2})([a-f\d]{2})$/i.exec(hex);
  if (result) {
    return {
      x: parseInt(result[1], 16) / 255,
      y: parseInt(result[2], 16) / 255,
      z: parseInt(result[3], 16) / 255,
    };
  }
  return { x: 1, y: 1, z: 1 };
};

// Material Editor Component for a single instance
const MaterialEditor: Component<{
  actorId: number;
  instanceIndex: number;
}> = (props) => {
  const [expanded, setExpanded] = createSignal(false);
  const [materialProps, setMaterialProps] = createSignal<Map<string, MaterialProperty>>(new Map());
  const [loading, setLoading] = createSignal(false);
  const [error, setError] = createSignal<string | null>(null);

  // Load material properties when expanded
  createEffect(async () => {
    if (!expanded()) return;

    setLoading(true);
    setError(null);
    const propsMap = new Map<string, MaterialProperty>();

    // Try to load each common property
    for (const propDef of COMMON_MATERIAL_PROPERTIES) {
      try {
        const prop = await invoke<MaterialProperty>('get_material_property', {
          actorId: props.actorId,
          instanceIndex: props.instanceIndex,
          propertyName: propDef.name,
        });
        propsMap.set(propDef.name, prop);
      } catch (e) {
        // Property doesn't exist for this material - that's OK
        console.debug(`Property ${propDef.name} not found:`, e);
      }
    }

    setMaterialProps(propsMap);
    setLoading(false);
  });

  const updateFloatProperty = async (name: string, value: number) => {
    try {
      await invoke('set_material_float', {
        actorId: props.actorId,
        instanceIndex: props.instanceIndex,
        propertyName: name,
        value,
      });
      // Update local state
      const props_map = new Map(materialProps());
      const existing = props_map.get(name);
      if (existing) {
        props_map.set(name, { ...existing, value: { type: 'Float', value } });
        setMaterialProps(props_map);
      }
    } catch (e) {
      console.error(`Failed to set ${name}:`, e);
      setError(`Failed to set ${name}`);
    }
  };

  const updateColorProperty = async (name: string, hexColor: string) => {
    const vec3 = hexToVec3(hexColor);
    try {
      await invoke('set_material_vec3', {
        actorId: props.actorId,
        instanceIndex: props.instanceIndex,
        propertyName: name,
        value: vec3,
      });
      // Update local state
      const props_map = new Map(materialProps());
      const existing = props_map.get(name);
      if (existing) {
        props_map.set(name, { ...existing, value: { type: 'Vec3', value: vec3 } });
        setMaterialProps(props_map);
      }
    } catch (e) {
      console.error(`Failed to set ${name}:`, e);
      setError(`Failed to set ${name}`);
    }
  };

  const getFloatValue = (name: string): number | undefined => {
    const prop = materialProps().get(name);
    if (prop && prop.value.type === 'Float') {
      return prop.value.value;
    }
    return undefined;
  };

  const getVec3Value = (name: string): Vec3 | undefined => {
    const prop = materialProps().get(name);
    if (prop && prop.value.type === 'Vec3') {
      return prop.value.value;
    }
    return undefined;
  };

  return (
    <div class="material-editor">
      <button
        type="button"
        class="material-toggle"
        onClick={() => setExpanded(!expanded())}
      >
        <span class="toggle-icon">{expanded() ? '▼' : '▶'}</span>
        Material Properties
      </button>
      <Show when={expanded()}>
        <div class="material-content">
          <Show when={loading()}>
            <div class="material-loading">Loading...</div>
          </Show>
          <Show when={error()}>
            <div class="material-error">{error()}</div>
          </Show>
          <Show when={!loading() && materialProps().size === 0}>
            <div class="material-empty">No editable material properties</div>
          </Show>
          <Show when={!loading() && materialProps().size > 0}>
            <For each={COMMON_MATERIAL_PROPERTIES}>
              {(propDef) => {
                const floatVal = () => getFloatValue(propDef.name);
                const vec3Val = () => getVec3Value(propDef.name);
                const inputId = `mat-${props.actorId}-${props.instanceIndex}-${propDef.name}`;

                return (
                  <Show when={materialProps().has(propDef.name)}>
                    <div class="material-property">
                      <label class="material-label" for={inputId}>{propDef.displayName}</label>
                      <Show when={propDef.type === 'float' && floatVal() !== undefined}>
                        <div class="material-float-input">
                          <input
                            type="range"
                            id={`${inputId}-slider`}
                            title={propDef.displayName}
                            min={propDef.min ?? 0}
                            max={propDef.max ?? 1}
                            step={propDef.step ?? 0.01}
                            value={floatVal()}
                            onInput={(e) => updateFloatProperty(propDef.name, parseFloat(e.currentTarget.value))}
                          />
                          <input
                            type="number"
                            id={inputId}
                            title={propDef.displayName}
                            min={propDef.min ?? 0}
                            max={propDef.max ?? 1}
                            step={propDef.step ?? 0.01}
                            value={floatVal()?.toFixed(3)}
                            onChange={(e) => {
                              const val = parseFloat(e.currentTarget.value);
                              if (!isNaN(val)) {
                                updateFloatProperty(propDef.name, val);
                              }
                            }}
                          />
                        </div>
                      </Show>
                      <Show when={propDef.type === 'color' && vec3Val() !== undefined}>
                        <div class="material-color-input">
                          <input
                            type="color"
                            id={inputId}
                            title={propDef.displayName}
                            value={vec3ToHex(vec3Val()!)}
                            onChange={(e) => updateColorProperty(propDef.name, e.currentTarget.value)}
                          />
                          <span class="color-value">{vec3ToHex(vec3Val()!)}</span>
                        </div>
                      </Show>
                    </div>
                  </Show>
                );
              }}
            </For>
          </Show>
        </div>
      </Show>
    </div>
  );
};

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
                        <MaterialEditor
                          actorId={actor().id}
                          instanceIndex={index()}
                        />
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
