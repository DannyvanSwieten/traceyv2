import { Component, Show } from 'solid-js';
import { sopGraph, setParamAnywhere } from '../../stores/sops';
import { findNodeRecursive } from '../../lib/sop_graph';
import { ColorSwatch } from '../color-swatch/ColorSwatch';
import { NumberInput } from '../number-input/NumberInput';
import * as api from '../../lib/api';
import type { Vec3 } from '../../lib/api';
import './MaterialOverride.css';

// Inline per-object material override, edited from the Object Properties panel.
// The data lives on the actor's source `object_output` SOP node (the cook's
// source of truth); we read it from the live SOP graph and write back through
// setParamAnywhere, which schedules the same recook a SOP-graph edit would.
// This replaces the old pile of material params on the Object Output node
// inspector — material is a property of the object, not of the output node.

type Tuple3 = [number, number, number];
const toVec3 = (t: Tuple3): Vec3 => ({ x: t[0], y: t[1], z: t[2] });
const toTuple = (v: Vec3): Tuple3 => [v.x, v.y, v.z];

const MatSlider: Component<{
  label: string; min: number; max: number; step?: number;
  value: () => number; onChange: (v: number) => void;
}> = (p) => (
  <div class="mat-row">
    <label class="mat-label">{p.label}</label>
    <div class="mat-slider-group">
      <input
        type="range"
        class="mat-slider"
        aria-label={p.label}
        title={p.label}
        min={p.min}
        max={p.max}
        step={p.step ?? 0.01}
        value={p.value()}
        onInput={(e) => p.onChange(parseFloat(e.currentTarget.value) || 0)}
      />
      <NumberInput
        class="mat-readout"
        step={p.step ?? 0.01}
        title={p.label}
        value={p.value}
        onCommit={p.onChange}
      />
    </div>
  </div>
);

const MatColor: Component<{
  label: string; value: () => Vec3; onChange: (v: Vec3) => void;
}> = (p) => (
  <div class="mat-row">
    <label class="mat-label">{p.label}</label>
    <ColorSwatch class="mat-swatch" title={p.label} value={p.value} onCommit={p.onChange} />
  </div>
);

export const MaterialOverride: Component<{ nodeUid: number | null | undefined }> = (props) => {
  const node = () =>
    props.nodeUid == null ? undefined : findNodeRecursive(sopGraph(), props.nodeUid);

  const f = (name: string, def: number): number => {
    const p = node()?.params[name];
    return p && p.type === 'float' ? p.value : def;
  };
  const v3 = (name: string, def: Tuple3): Tuple3 => {
    const p = node()?.params[name];
    return p && p.type === 'vec3' ? (p.value as Tuple3) : def;
  };
  const b = (name: string, def: boolean): boolean => {
    const p = node()?.params[name];
    return p && p.type === 'bool' ? p.value : def;
  };
  const setF = (name: string, value: number) => {
    if (props.nodeUid != null) setParamAnywhere(props.nodeUid, name, { type: 'float', value });
  };
  const setV3 = (name: string, value: Tuple3) => {
    if (props.nodeUid != null) setParamAnywhere(props.nodeUid, name, { type: 'vec3', value });
  };
  const setB = (name: string, value: boolean) => {
    if (props.nodeUid != null) setParamAnywhere(props.nodeUid, name, { type: 'bool', value });
  };

  const overrideOn = () => b('override_material', false);

  // Import a MaterialX standard_surface onto this object's override params.
  const importMaterialX = async () => {
    if (props.nodeUid == null) return;
    try {
      const path = await api.openFileDialog('Import MaterialX', [
        { description: 'MaterialX', extensions: ['mtlx'] },
      ]);
      if (!path) return;
      const mats = await api.readMaterialXMaterials(path);
      if (!mats.length) return;
      const p = mats[0].params;
      setB('override_material', true);
      setV3('base_color', p.base_color);
      setF('metallic', p.metallic);
      setF('roughness', p.roughness);
      setF('transmission', p.transmission);
      setF('ior', p.ior);
      setV3('emission', p.emission);
      setF('emission_strength', p.emission_strength);
      setF('opacity', p.opacity);
      setF('clearcoat', p.clearcoat);
      setF('clearcoat_roughness', p.clearcoat_roughness);
      setF('sheen', p.sheen);
      setF('subsurface', p.subsurface);
      setV3('subsurface_color', p.subsurface_color);
      setF('anisotropy', p.anisotropy);
    } catch (e) {
      console.warn('MaterialX import failed:', e);
    }
  };

  return (
    <Show when={node()?.kind === 'object_output'}>
      <div class="property-section">
        <h4>Material Override</h4>
        <label class="mat-toggle">
          <input
            type="checkbox"
            checked={overrideOn()}
            onChange={(e) => setB('override_material', e.currentTarget.checked)}
          />
          <span>Override material</span>
        </label>
        <button type="button" class="mat-import" onClick={importMaterialX}>
          Import MaterialX…
        </button>

        <Show
          when={overrideOn()}
          fallback={
            <div class="mat-hint">
              Enable to set this object's material directly, or assign a library
              material below.
            </div>
          }
        >
          <div class="mat-group">Base</div>
          <MatColor label="Base Color" value={() => toVec3(v3('base_color', [0.8, 0.8, 0.8]))}
                    onChange={(c) => setV3('base_color', toTuple(c))} />
          <MatSlider label="Metallic" min={0} max={1} value={() => f('metallic', 0)}
                     onChange={(v) => setF('metallic', v)} />
          <MatSlider label="Roughness" min={0} max={1} value={() => f('roughness', 0.5)}
                     onChange={(v) => setF('roughness', v)} />

          <div class="mat-group">Specular &amp; Transmission</div>
          <MatSlider label="Transmission" min={0} max={1} value={() => f('transmission', 0)}
                     onChange={(v) => setF('transmission', v)} />
          <MatSlider label="IOR" min={1} max={3} value={() => f('ior', 1.5)}
                     onChange={(v) => setF('ior', v)} />
          <MatSlider label="Opacity" min={0} max={1} value={() => f('opacity', 1)}
                     onChange={(v) => setF('opacity', v)} />

          <div class="mat-group">Emission</div>
          <MatColor label="Emission" value={() => toVec3(v3('emission', [0, 0, 0]))}
                    onChange={(c) => setV3('emission', toTuple(c))} />
          <div class="mat-row">
            <label class="mat-label">Strength</label>
            <NumberInput class="mat-readout-wide" step={0.1} title="Emission strength"
                         value={() => f('emission_strength', 1)} onCommit={(v) => setF('emission_strength', v)} />
          </div>

          <div class="mat-group">Coat</div>
          <MatSlider label="Clearcoat" min={0} max={1} value={() => f('clearcoat', 0)}
                     onChange={(v) => setF('clearcoat', v)} />
          <MatSlider label="Coat Rough" min={0} max={1} value={() => f('clearcoat_roughness', 0)}
                     onChange={(v) => setF('clearcoat_roughness', v)} />

          <div class="mat-group">Sheen &amp; Subsurface</div>
          <MatSlider label="Sheen" min={0} max={1} value={() => f('sheen', 0)}
                     onChange={(v) => setF('sheen', v)} />
          <MatSlider label="Subsurface" min={0} max={1} value={() => f('subsurface', 0)}
                     onChange={(v) => setF('subsurface', v)} />
          <MatColor label="SSS Color" value={() => toVec3(v3('subsurface_color', [1, 1, 1]))}
                    onChange={(c) => setV3('subsurface_color', toTuple(c))} />
          <MatSlider label="Anisotropy" min={-1} max={1} value={() => f('anisotropy', 0)}
                     onChange={(v) => setF('anisotropy', v)} />
        </Show>
      </div>
    </Show>
  );
};
