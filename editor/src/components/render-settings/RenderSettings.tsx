import { Component, createSignal, onMount, createEffect } from 'solid-js';
import { invoke } from '@tauri-apps/api/core';
import { open } from '@tauri-apps/plugin-dialog';
import { ViewportHandle } from '../viewport/Viewport';
import './RenderSettings.css';

interface RenderSettingsProps {
  onSettingsChange?: () => void;
  viewportHandle?: ViewportHandle;
}

type RenderMode = 'PathTracer' | 'Rasterizer';
type PresentationMode = 'canvas' | 'native';

// Resolution scale options
const SCALE_OPTIONS = [
  { value: 0.25, label: '25%' },
  { value: 0.5, label: '50%' },
  { value: 0.75, label: '75%' },
  { value: 1.0, label: '100%' },
];

// Max resolution presets
const MAX_RES_OPTIONS = [
  { value: [1280, 720], label: '720p' },
  { value: [1920, 1080], label: '1080p' },
  { value: [2560, 1440], label: '1440p' },
  { value: [0, 0], label: 'No Limit' },
];

export const RenderSettings: Component<RenderSettingsProps> = (props) => {
  const [renderMode, setRenderMode] = createSignal<RenderMode>('PathTracer');
  const [presentationMode, setPresentationMode] = createSignal<PresentationMode>('canvas');
  const [samplesPerFrame, setSamplesPerFrame] = createSignal(4);
  const [maxBounces, setMaxBounces] = createSignal(2);
  const [maxSamples, setMaxSamples] = createSignal(256);
  const [resolutionScale, setResolutionScale] = createSignal(0.5);
  const [maxResolution, setMaxResolution] = createSignal<[number, number]>([1920, 1080]);
  const [effectiveResolution, setEffectiveResolution] = createSignal<[number, number]>([1280, 720]);
  const [isLoading, setIsLoading] = createSignal(true);

  // Environment map state
  const [envMapPath, setEnvMapPath] = createSignal<string | null>(null);
  const [envMapIntensity, setEnvMapIntensity] = createSignal(1.0);
  const [envMapRotation, setEnvMapRotation] = createSignal(0.0);

  onMount(async () => {
    try {
      const mode = await invoke<string>('get_render_mode');
      const samples = await invoke<number>('get_samples_per_frame');
      const bounces = await invoke<number>('get_max_bounces');
      const maxSamplesVal = await invoke<number>('get_max_samples');
      const scale = await invoke<number>('get_resolution_scale');
      const [maxW, maxH] = await invoke<[number, number]>('get_max_resolution');
      const [effW, effH] = await invoke<[number, number]>('get_viewport_resolution');
      setRenderMode(mode as RenderMode);
      setSamplesPerFrame(samples);
      setMaxBounces(bounces);
      setMaxSamples(maxSamplesVal);
      setResolutionScale(scale);
      setMaxResolution([maxW, maxH]);
      setEffectiveResolution([effW, effH]);
    } catch (error) {
      console.error('Failed to load render settings:', error);
    } finally {
      setIsLoading(false);
    }
  });

  const handleRenderModeChange = async (mode: RenderMode) => {
    console.log(`Switching render mode to: ${mode}`);
    setRenderMode(mode);
    try {
      await invoke('set_render_mode', { mode });
      console.log(`Render mode switched successfully to: ${mode}`);
      props.onSettingsChange?.();
    } catch (error) {
      console.error('Failed to set render mode:', error);
    }
  };

  const handleSamplesChange = async (value: number) => {
    setSamplesPerFrame(value);
    try {
      await invoke('set_samples_per_frame', { samples: value });
      props.onSettingsChange?.();
    } catch (error) {
      console.error('Failed to set samples per frame:', error);
    }
  };

  const handleBouncesChange = async (value: number) => {
    setMaxBounces(value);
    try {
      await invoke('set_max_bounces', { bounces: value });
      props.onSettingsChange?.();
    } catch (error) {
      console.error('Failed to set max bounces:', error);
    }
  };

  const handleMaxSamplesChange = async (value: number) => {
    const clampedValue = Math.max(1, value);
    setMaxSamples(clampedValue);
    try {
      await invoke('set_max_samples', { samples: clampedValue });
      props.onSettingsChange?.();
    } catch (error) {
      console.error('Failed to set max samples:', error);
    }
  };

  const handleResolutionScaleChange = async (scale: number) => {
    setResolutionScale(scale);
    try {
      await invoke('set_resolution_scale', { scale });
      // Get updated effective resolution
      const [effW, effH] = await invoke<[number, number]>('get_viewport_resolution');
      setEffectiveResolution([effW, effH]);
      props.onSettingsChange?.();
    } catch (error) {
      console.error('Failed to set resolution scale:', error);
    }
  };

  const handleMaxResolutionChange = async (width: number, height: number) => {
    setMaxResolution([width, height]);
    try {
      await invoke('set_max_resolution', { width, height });
      // Get updated effective resolution
      const [effW, effH] = await invoke<[number, number]>('get_viewport_resolution');
      setEffectiveResolution([effW, effH]);
    } catch (error) {
      console.error('Failed to set max resolution:', error);
    }
  };

  const handlePresentationModeChange = async (mode: PresentationMode) => {
    console.log('[RenderSettings] handlePresentationModeChange called with:', mode);
    console.log('[RenderSettings] viewportHandle exists:', !!props.viewportHandle);
    setPresentationMode(mode);
    try {
      if (props.viewportHandle) {
        console.log('[RenderSettings] Calling viewportHandle.setRenderMode');
        await props.viewportHandle.setRenderMode(mode);
        props.onSettingsChange?.();
      } else {
        console.warn('[RenderSettings] No viewportHandle available!');
      }
    } catch (error) {
      console.error('Failed to set presentation mode:', error);
    }
  };

  // Sync presentation mode from viewport handle
  createEffect(() => {
    if (props.viewportHandle) {
      const mode = props.viewportHandle.getRenderMode();
      setPresentationMode(mode);
    }
  });

  // Environment map handlers
  const handleEnvMapBrowse = async () => {
    try {
      const selected = await open({
        multiple: false,
        filters: [{
          name: 'HDR Images',
          extensions: ['hdr', 'exr']
        }]
      });
      if (selected && typeof selected === 'string') {
        setEnvMapPath(selected);
        await invoke('set_environment_map', {
          path: selected,
          intensity: envMapIntensity(),
          rotation: envMapRotation()
        });
        props.onSettingsChange?.();
      }
    } catch (error) {
      console.error('Failed to select environment map:', error);
    }
  };

  const handleEnvMapClear = async () => {
    setEnvMapPath(null);
    try {
      await invoke('set_environment_map', {
        path: null,
        intensity: 1.0,
        rotation: 0.0
      });
      setEnvMapIntensity(1.0);
      setEnvMapRotation(0.0);
      props.onSettingsChange?.();
    } catch (error) {
      console.error('Failed to clear environment map:', error);
    }
  };

  const handleEnvMapIntensityChange = async (value: number) => {
    setEnvMapIntensity(value);
    if (envMapPath()) {
      try {
        await invoke('set_environment_map', {
          path: envMapPath(),
          intensity: value,
          rotation: envMapRotation()
        });
        props.onSettingsChange?.();
      } catch (error) {
        console.error('Failed to update environment intensity:', error);
      }
    }
  };

  const handleEnvMapRotationChange = async (value: number) => {
    setEnvMapRotation(value);
    if (envMapPath()) {
      try {
        await invoke('set_environment_map', {
          path: envMapPath(),
          intensity: envMapIntensity(),
          rotation: value
        });
        props.onSettingsChange?.();
      } catch (error) {
        console.error('Failed to update environment rotation:', error);
      }
    }
  };

  return (
    <div class="render-settings">
      <h4>Render Settings</h4>
      {isLoading() ? (
        <p class="loading">Loading...</p>
      ) : (
        <>
          <div class="setting-row">
            <label for="render-mode">Render Mode</label>
            <div class="setting-control">
              <div class="render-mode-toggle">
                <button
                  type="button"
                  class={`mode-button ${renderMode() === 'Rasterizer' ? 'active' : ''}`}
                  onClick={() => handleRenderModeChange('Rasterizer')}
                  title="Real-time preview using rasterization"
                >
                  Realtime Preview
                </button>
                <button
                  type="button"
                  class={`mode-button ${renderMode() === 'PathTracer' ? 'active' : ''}`}
                  onClick={() => handleRenderModeChange('PathTracer')}
                  title="High-quality path tracing"
                >
                  Path Traced
                </button>
              </div>
            </div>
          </div>
          <div class="setting-row">
            <label for="presentation-mode">Presentation Mode</label>
            <div class="setting-control">
              <div class="render-mode-toggle">
                <button
                  type="button"
                  class={`mode-button ${presentationMode() === 'canvas' ? 'active' : ''}`}
                  onClick={() => handlePresentationModeChange('canvas')}
                  title="Canvas 2D rendering (compatible)"
                >
                  Canvas
                </button>
                <button
                  type="button"
                  class={`mode-button ${presentationMode() === 'native' ? 'active' : ''}`}
                  onClick={() => handlePresentationModeChange('native')}
                  title="Direct GPU presentation (faster)"
                >
                  Native
                </button>
              </div>
            </div>
          </div>
          <div class="setting-row">
            <label for="samples">Samples per Frame</label>
            <div class="setting-control">
              <input
                id="samples"
                type="range"
                min="1"
                max="64"
                value={samplesPerFrame()}
                onInput={(e) => handleSamplesChange(parseInt(e.currentTarget.value))}
                disabled={renderMode() === 'Rasterizer'}
              />
              <span class="setting-value">{samplesPerFrame()}</span>
            </div>
          </div>
          <div class="setting-row">
            <label for="bounces">Max Bounces</label>
            <div class="setting-control">
              <input
                id="bounces"
                type="range"
                min="1"
                max="16"
                value={maxBounces()}
                onInput={(e) => handleBouncesChange(parseInt(e.currentTarget.value))}
                disabled={renderMode() === 'Rasterizer'}
              />
              <span class="setting-value">{maxBounces()}</span>
            </div>
          </div>
          <div class="setting-row">
            <label for="max-samples">Max Samples</label>
            <div class="setting-control">
              <input
                id="max-samples"
                type="number"
                min="1"
                value={maxSamples()}
                onInput={(e) => handleMaxSamplesChange(parseInt(e.currentTarget.value) || 1)}
                disabled={renderMode() === 'Rasterizer'}
              />
            </div>
          </div>

          <h4>Resolution</h4>

          <div class="setting-row">
            <label for="resolution-scale">Render Scale</label>
            <div class="setting-control">
              <select
                id="resolution-scale"
                value={resolutionScale()}
                onChange={(e) => handleResolutionScaleChange(parseFloat(e.currentTarget.value))}
              >
                {SCALE_OPTIONS.map((opt) => (
                  <option value={opt.value}>{opt.label}</option>
                ))}
              </select>
            </div>
          </div>

          <div class="setting-row">
            <label for="max-resolution">Max Resolution</label>
            <div class="setting-control">
              <select
                id="max-resolution"
                value={`${maxResolution()[0]},${maxResolution()[1]}`}
                onChange={(e) => {
                  const [w, h] = e.currentTarget.value.split(',').map(Number);
                  handleMaxResolutionChange(w, h);
                }}
              >
                {MAX_RES_OPTIONS.map((opt) => (
                  <option value={`${opt.value[0]},${opt.value[1]}`}>{opt.label}</option>
                ))}
              </select>
            </div>
          </div>

          <div class="setting-row">
            <label>Effective Resolution</label>
            <div class="setting-control">
              <span class="setting-value resolution-display">
                {effectiveResolution()[0]} x {effectiveResolution()[1]}
              </span>
            </div>
          </div>

          <h4>Environment</h4>

          <div class="setting-row">
            <label>HDR Environment Map</label>
            <div class="setting-control env-map-control">
              <button
                type="button"
                class="env-map-button"
                onClick={handleEnvMapBrowse}
                title="Browse for HDR environment map"
              >
                {envMapPath() ? envMapPath()!.split('/').pop() || envMapPath()!.split('\\').pop() : 'Browse...'}
              </button>
              {envMapPath() && (
                <button
                  type="button"
                  class="env-map-clear"
                  onClick={handleEnvMapClear}
                  title="Clear environment map"
                >
                  ×
                </button>
              )}
            </div>
          </div>

          <div class="setting-row">
            <label for="env-intensity">Intensity</label>
            <div class="setting-control">
              <input
                id="env-intensity"
                type="range"
                min="0"
                max="10"
                step="0.1"
                value={envMapIntensity()}
                onInput={(e) => handleEnvMapIntensityChange(parseFloat(e.currentTarget.value))}
                disabled={!envMapPath()}
              />
              <span class="setting-value">{envMapIntensity().toFixed(1)}</span>
            </div>
          </div>

          <div class="setting-row">
            <label for="env-rotation">Rotation</label>
            <div class="setting-control">
              <input
                id="env-rotation"
                type="range"
                min="0"
                max="6.28318"
                step="0.01"
                value={envMapRotation()}
                onInput={(e) => handleEnvMapRotationChange(parseFloat(e.currentTarget.value))}
                disabled={!envMapPath()}
              />
              <span class="setting-value">{(envMapRotation() * 180 / Math.PI).toFixed(0)}°</span>
            </div>
          </div>
        </>
      )}
    </div>
  );
};
