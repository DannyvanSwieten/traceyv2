import { Component, createSignal, onMount } from 'solid-js';
import * as api from '../../lib/api';
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
      const samples = await api.getSamplesPerFrame();
      const bounces = await api.getMaxBounces();
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
      await api.setSamplesPerFrame(value);
      props.onSettingsChange?.();
    } catch (error) {
      console.error('Failed to set samples per frame:', error);
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
