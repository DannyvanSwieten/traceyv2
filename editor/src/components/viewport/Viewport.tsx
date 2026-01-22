import { Component, createSignal, onMount } from 'solid-js';
import { invoke } from '@tauri-apps/api/core';
import './Viewport.css';

interface Camera {
  position: { x: number; y: number; z: number };
  rotation: { w: number; x: number; y: number; z: number };
  fov: number;
  near_plane: number;
  far_plane: number;
  aspect_ratio: number;
}

interface RenderResult {
  width: number;
  height: number;
  sample_count: number;
  render_time_ms: number;
}

export const Viewport: Component = () => {
  const [status, setStatus] = createSignal('Initializing...');
  const [renderTime, setRenderTime] = createSignal(0);
  const [sampleCount, setSampleCount] = createSignal(0);
  let canvasRef: HTMLCanvasElement | undefined;

  // Camera positioned to view DamagedHelmet at origin
  const camera: Camera = {
    position: { x: 0, y: 0, z: 3 },
    rotation: { w: 1.0, x: 0, y: 0, z: 0 }, // Default forward = (0,0,-1) looks at origin
    fov: 45.0,
    near_plane: 0.01,
    far_plane: 1000.0,
    aspect_ratio: 16.0 / 9.0,
  };

  const renderFrame = async (clear: boolean) => {
    try {
      // Render the frame
      const result = (await invoke('render_frame', {
        camera,
        clearAccumulation: clear,
      })) as RenderResult;

      setRenderTime(result.render_time_ms);
      setSampleCount(result.sample_count);

      // Get the pixels as a Uint8Array
      const pixels = new Uint8Array(await invoke('get_render_pixels'));

      // Draw to canvas
      if (canvasRef) {
        const ctx = canvasRef.getContext('2d');
        if (ctx) {
          const imageData = ctx.createImageData(result.width, result.height);

          // Copy pixel data (RGBA format)
          imageData.data.set(pixels);

          ctx.putImageData(imageData, 0, 0);
          setStatus('Rendering');
        }
      }
    } catch (e) {
      setStatus(`Error: ${e}`);
      console.error('Render failed:', e);
    }
  };

  const startRendering = async () => {
    try {
      // Load DamagedHelmet
      setStatus('Loading scene...');
      const scenePath = '/Users/dannyvanswieten/Documents/code/traceyv2/examples/scenes/DamagedHelmet.glb';
      await invoke('import_gltf', { path: scenePath });

      // Compile the scene
      setStatus('Compiling scene...');
      await invoke('compile_scene');

      // Get viewport resolution
      const [width, height] = (await invoke('get_viewport_resolution')) as [
        number,
        number
      ];

      // Update canvas size
      if (canvasRef) {
        canvasRef.width = width;
        canvasRef.height = height;
      }

      // Update camera aspect ratio
      camera.aspect_ratio = width / height;

      setStatus('Rendering...');

      // Render just one sample for debugging
      await renderFrame(true);

      setStatus('Done (1 sample)');
    } catch (e) {
      setStatus(`Initialization error: ${e}`);
      console.error('Failed to start rendering:', e);
    }
  };

  onMount(() => {
    startRendering();
  });

  return (
    <div class="viewport-wrapper">
      <div class="viewport-header">
        <span class="viewport-status">{status()}</span>
        <span class="viewport-stats">
          {renderTime().toFixed(2)}ms | {sampleCount()} samples
        </span>
      </div>
      <div class="viewport-canvas-container">
        <canvas
          ref={canvasRef}
          class="viewport-canvas"
          width={1280}
          height={720}
        />
      </div>
    </div>
  );
};
