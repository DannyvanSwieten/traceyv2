import { Component, createSignal, onMount } from 'solid-js';
import { invoke } from '@tauri-apps/api/core';
import './RenderSettings.css';

interface RenderSettingsProps {
  onSettingsChange?: () => void;
}

export const RenderSettings: Component<RenderSettingsProps> = (props) => {
  const [samplesPerFrame, setSamplesPerFrame] = createSignal(16);
  const [maxBounces, setMaxBounces] = createSignal(8);
  const [isLoading, setIsLoading] = createSignal(true);

  onMount(async () => {
    try {
      const samples = await invoke<number>('get_samples_per_frame');
      const bounces = await invoke<number>('get_max_bounces');
      setSamplesPerFrame(samples);
      setMaxBounces(bounces);
    } catch (error) {
      console.error('Failed to load render settings:', error);
    } finally {
      setIsLoading(false);
    }
  });

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

  return (
    <div class="render-settings">
      <h4>Render Settings</h4>
      {isLoading() ? (
        <p class="loading">Loading...</p>
      ) : (
        <>
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
              />
              <span class="setting-value">{maxBounces()}</span>
            </div>
          </div>
        </>
      )}
    </div>
  );
};
