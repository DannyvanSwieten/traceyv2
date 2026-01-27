import { Component, createSignal, onMount, createEffect } from 'solid-js';
import { invoke } from '@tauri-apps/api/core';
import { ViewportHandle } from '../viewport/Viewport';
import './RenderSettings.css';

interface RenderSettingsProps {
  onSettingsChange?: () => void;
  viewportHandle?: ViewportHandle;
}

type RenderMode = 'PathTracer' | 'Rasterizer';
type PresentationMode = 'canvas' | 'native';

export const RenderSettings: Component<RenderSettingsProps> = (props) => {
  const [renderMode, setRenderMode] = createSignal<RenderMode>('PathTracer');
  const [presentationMode, setPresentationMode] = createSignal<PresentationMode>('canvas');
  const [samplesPerFrame, setSamplesPerFrame] = createSignal(4);
  const [maxBounces, setMaxBounces] = createSignal(2);
  const [isLoading, setIsLoading] = createSignal(true);

  onMount(async () => {
    try {
      const mode = await invoke<string>('get_render_mode');
      const samples = await invoke<number>('get_samples_per_frame');
      const bounces = await invoke<number>('get_max_bounces');
      setRenderMode(mode as RenderMode);
      setSamplesPerFrame(samples);
      setMaxBounces(bounces);
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
        </>
      )}
    </div>
  );
};
