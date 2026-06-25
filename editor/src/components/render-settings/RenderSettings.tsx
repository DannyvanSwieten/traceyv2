import { Component, createSignal, onMount } from 'solid-js';
import * as api from '../../lib/api';
import './RenderSettings.css';

interface RenderSettingsProps {
  onSettingsChange?: () => void;
}

export const RenderSettings: Component<RenderSettingsProps> = (props) => {
  // Path-tracer-only settings live here. Rasterizer overlays (points,
  // edges, ground) moved to a dedicated sub-toolbar under the main
  // toolbar so they're always reachable regardless of which workspace
  // is active — see RasterizerToolbar.tsx.
  const [maxSamples, setMaxSamplesSignal] = createSignal(16);
  const [maxBounces, setMaxBounces] = createSignal(8);
  const [isLoading, setIsLoading] = createSignal(true);

  onMount(async () => {
    try {
      const samples = await api.getMaxSamples();
      const bounces = await api.getMaxBounces();
      setMaxSamplesSignal(samples);
      setMaxBounces(bounces);
    } catch (error) {
      console.error('Failed to load render settings:', error);
    } finally {
      setIsLoading(false);
    }
  });

  const handleMaxSamplesChange = async (value: number) => {
    setMaxSamplesSignal(value);
    try {
      await api.setMaxSamples(value);
      props.onSettingsChange?.();
    } catch (error) {
      console.error('Failed to set max samples:', error);
    }
  };

  const handleBouncesChange = async (value: number) => {
    setMaxBounces(value);
    try {
      await api.setMaxBounces(value);
      props.onSettingsChange?.();
    } catch (error) {
      console.error('Failed to set max bounces:', error);
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
            <label for="max-samples">Max Samples</label>
            <div class="setting-control">
              <input
                id="max-samples"
                type="range"
                min="1"
                max="8192"
                step="1"
                value={maxSamples()}
                onInput={(e) => handleMaxSamplesChange(parseInt(e.currentTarget.value))}
              />
              <span class="setting-value">{maxSamples()}</span>
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
              />
              <span class="setting-value">{maxBounces()}</span>
            </div>
          </div>
        </>
      )}
    </div>
  );
};
