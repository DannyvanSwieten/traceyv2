import { Component, createSignal, onMount } from 'solid-js';
import * as api from '../../lib/api';
import './RenderSettings.css';

interface RenderSettingsProps {
  onSettingsChange?: () => void;
}

export const RenderSettings: Component<RenderSettingsProps> = (props) => {
  const [maxSamples, setMaxSamplesSignal] = createSignal(1024);
  const [maxBounces, setMaxBounces] = createSignal(8);
  const [showPoints, setShowPoints] = createSignal(false);
  const [showEdges, setShowEdges] = createSignal(false);
  const [showGround, setShowGround] = createSignal(false);
  const [isLoading, setIsLoading] = createSignal(true);

  onMount(async () => {
    try {
      const samples = await api.getMaxSamples();
      const bounces = await api.getMaxBounces();
      const points = await api.getShowPoints();
      const edges = await api.getShowEdges().catch(() => false);
      const ground = await api.getShowGround().catch(() => false);
      setMaxSamplesSignal(samples);
      setMaxBounces(bounces);
      setShowPoints(points);
      setShowEdges(edges);
      setShowGround(ground);
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

  const handleShowPointsChange = async (value: boolean) => {
    setShowPoints(value);
    try {
      await api.setShowPoints(value);
      props.onSettingsChange?.();
    } catch (error) {
      console.error('Failed to set show points:', error);
    }
  };

  const handleShowEdgesChange = async (value: boolean) => {
    setShowEdges(value);
    try {
      await api.setShowEdges(value);
      props.onSettingsChange?.();
    } catch (error) {
      console.error('Failed to set show edges:', error);
    }
  };

  const handleShowGroundChange = async (value: boolean) => {
    setShowGround(value);
    try {
      await api.setShowGround(value);
      props.onSettingsChange?.();
    } catch (error) {
      console.error('Failed to set show ground:', error);
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
                min="16"
                max="8192"
                step="16"
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
          <div class="setting-row">
            <label for="show-points">Show Points</label>
            <div class="setting-control">
              <input
                id="show-points"
                type="checkbox"
                checked={showPoints()}
                onChange={(e) => handleShowPointsChange(e.currentTarget.checked)}
              />
            </div>
          </div>
          <div class="setting-row">
            <label for="show-edges">Show Edges</label>
            <div class="setting-control">
              <input
                id="show-edges"
                type="checkbox"
                checked={showEdges()}
                onChange={(e) => handleShowEdgesChange(e.currentTarget.checked)}
              />
            </div>
          </div>
          <div class="setting-row">
            <label for="show-ground">Show Ground</label>
            <div class="setting-control">
              <input
                id="show-ground"
                type="checkbox"
                checked={showGround()}
                onChange={(e) => handleShowGroundChange(e.currentTarget.checked)}
              />
            </div>
          </div>
        </>
      )}
    </div>
  );
};
