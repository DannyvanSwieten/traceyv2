import { Component, createSignal, createEffect, onMount, onCleanup, Accessor, Show } from 'solid-js';
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

export interface CameraPosition {
  x: number;
  y: number;
  z: number;
}

export interface ViewportHandle {
  loadScene: (path: string) => Promise<void>;
  render: () => void;
  setRenderMode: (mode: 'canvas' | 'native') => Promise<void>;
  getRenderMode: () => 'canvas' | 'native';
}

interface ViewportProps {
  ref?: (handle: ViewportHandle) => void;
  cameraPosition: Accessor<CameraPosition>;
  onCameraPositionChange: (pos: CameraPosition) => void;
}

// Quaternion math utilities
const quatFromEuler = (yaw: number, pitch: number): Camera['rotation'] => {
  const cy = Math.cos(yaw * 0.5);
  const sy = Math.sin(yaw * 0.5);
  const cp = Math.cos(pitch * 0.5);
  const sp = Math.sin(pitch * 0.5);

  return {
    w: cy * cp,
    x: cy * sp,
    y: sy * cp,
    z: -sy * sp,
  };
};

const getForward = (q: Camera['rotation']): { x: number; y: number; z: number } => {
  // Forward vector from quaternion (looking down -Z by default)
  return {
    x: 2 * (q.x * q.z + q.w * q.y),
    y: 2 * (q.y * q.z - q.w * q.x),
    z: -(1 - 2 * (q.x * q.x + q.y * q.y)),
  };
};

const getRight = (q: Camera['rotation']): { x: number; y: number; z: number } => {
  // Right vector from quaternion
  return {
    x: 1 - 2 * (q.y * q.y + q.z * q.z),
    y: 2 * (q.x * q.y + q.w * q.z),
    z: 2 * (q.x * q.z - q.w * q.y),
  };
};

type RenderMode = 'canvas' | 'native';

export const Viewport: Component<ViewportProps> = (props) => {
  const [status, setStatus] = createSignal('Initializing...');
  const [renderTime, setRenderTime] = createSignal(0);
  const [sampleCount, setSampleCount] = createSignal(0);
  const [sceneLoaded, setSceneLoaded] = createSignal(false);
  const [renderMode, setRenderMode] = createSignal<RenderMode>('canvas');
  const [nativeViewportReady, setNativeViewportReady] = createSignal(false);
  let canvasRef: HTMLCanvasElement | undefined;
  let containerRef: HTMLDivElement | undefined;
  let nativePlaceholderRef: HTMLDivElement | undefined;

  // Camera state
  const camera: Camera = {
    position: { x: 0, y: 0, z: 3 },
    rotation: { w: 1.0, x: 0, y: 0, z: 0 },
    fov: 45.0,
    near_plane: 0.01,
    far_plane: 1000.0,
    aspect_ratio: 16.0 / 9.0,
  };

  // Camera control state
  let yaw = 0;
  let pitch = 0;
  let isDragging = false;
  let lastMouseX = 0;
  let lastMouseY = 0;
  const keysPressed = new Set<string>();
  let isRendering = false;
  let needsRender = false;
  let externalUpdate = false;

  const MOUSE_SENSITIVITY = 0.003;
  const MOVE_SPEED = 0.1;
  const SCROLL_SPEED = 0.01;

  // Sync camera position from props
  createEffect(() => {
    const pos = props.cameraPosition();
    if (
      camera.position.x !== pos.x ||
      camera.position.y !== pos.y ||
      camera.position.z !== pos.z
    ) {
      camera.position = { ...pos };
      if (sceneLoaded() && !externalUpdate) {
        externalUpdate = true;
        renderFrame(true);
        externalUpdate = false;
      }
    }
  });

  const updateCameraRotation = () => {
    // Clamp pitch to avoid gimbal lock
    pitch = Math.max(-Math.PI / 2 + 0.01, Math.min(Math.PI / 2 - 0.01, pitch));
    camera.rotation = quatFromEuler(yaw, pitch);
  };

  const updateCameraPosition = () => {
    props.onCameraPositionChange({ ...camera.position });
  };

  const renderFrame = async (clear: boolean) => {
    if (isRendering) {
      needsRender = true;
      return;
    }

    isRendering = true;
    try {
      if (renderMode() === 'native' && nativeViewportReady()) {
        // Native mode: render directly to native window
        const result = (await invoke('render_frame', {
          camera,
          clearAccumulation: clear,
        })) as RenderResult;

        setRenderTime(result.render_time_ms);
        setSampleCount(result.sample_count);

        // Present to native viewport
        try {
          await invoke('present_pathtracer');
          setStatus('Rendering (Native)');
        } catch (e) {
          console.error('Native presentation failed:', e);
          setStatus(`Present Error: ${e}`);
        }
      } else {
        // Canvas mode: traditional CPU readback
        const result = (await invoke('render_frame', {
          camera,
          clearAccumulation: clear,
        })) as RenderResult;

        setRenderTime(result.render_time_ms);
        setSampleCount(result.sample_count);

        const pixels = new Uint8Array(await invoke('get_render_pixels'));

        if (canvasRef) {
          const ctx = canvasRef.getContext('2d');
          if (ctx) {
            const imageData = ctx.createImageData(result.width, result.height);
            imageData.data.set(pixels);
            ctx.putImageData(imageData, 0, 0);
            setStatus('Rendering (Canvas)');
          }
        }
      }
    } catch (e) {
      setStatus(`Error: ${e}`);
      console.error('Render failed:', e);
    } finally {
      isRendering = false;
      if (needsRender) {
        needsRender = false;
        renderFrame(true);
      }
    }
  };

  const handleMouseDown = (e: MouseEvent) => {
    if (e.button === 0 || e.button === 2) {
      isDragging = true;
      lastMouseX = e.clientX;
      lastMouseY = e.clientY;
      containerRef?.focus();
    }
  };

  const handleMouseUp = () => {
    isDragging = false;
  };

  const handleMouseMove = (e: MouseEvent) => {
    if (!isDragging || !sceneLoaded()) return;

    const deltaX = e.clientX - lastMouseX;
    const deltaY = e.clientY - lastMouseY;
    lastMouseX = e.clientX;
    lastMouseY = e.clientY;

    yaw += deltaX * MOUSE_SENSITIVITY;
    pitch += deltaY * MOUSE_SENSITIVITY;

    updateCameraRotation();
    updateCameraPosition();
    renderFrame(true);
  };

  const handleWheel = (e: WheelEvent) => {
    if (!sceneLoaded()) return;
    e.preventDefault();

    const forward = getForward(camera.rotation);
    const distance = e.deltaY > 0 ? -SCROLL_SPEED : SCROLL_SPEED;

    camera.position.x += forward.x * distance;
    camera.position.y += forward.y * distance;
    camera.position.z += forward.z * distance;

    updateCameraPosition();
    renderFrame(true);
  };

  const handleKeyDown = (e: KeyboardEvent) => {
    if (!sceneLoaded()) return;
    keysPressed.add(e.key.toLowerCase());
    processMovement();
  };

  const handleKeyUp = (e: KeyboardEvent) => {
    keysPressed.delete(e.key.toLowerCase());
  };

  const processMovement = () => {
    if (keysPressed.size === 0) return;

    const forward = getForward(camera.rotation);
    const right = getRight(camera.rotation);
    let moved = false;

    if (keysPressed.has('w')) {
      camera.position.x += forward.x * MOVE_SPEED;
      camera.position.y += forward.y * MOVE_SPEED;
      camera.position.z += forward.z * MOVE_SPEED;
      moved = true;
    }
    if (keysPressed.has('s')) {
      camera.position.x -= forward.x * MOVE_SPEED;
      camera.position.y -= forward.y * MOVE_SPEED;
      camera.position.z -= forward.z * MOVE_SPEED;
      moved = true;
    }
    if (keysPressed.has('a')) {
      camera.position.x -= right.x * MOVE_SPEED;
      camera.position.y -= right.y * MOVE_SPEED;
      camera.position.z -= right.z * MOVE_SPEED;
      moved = true;
    }
    if (keysPressed.has('d')) {
      camera.position.x += right.x * MOVE_SPEED;
      camera.position.y += right.y * MOVE_SPEED;
      camera.position.z += right.z * MOVE_SPEED;
      moved = true;
    }
    if (keysPressed.has('q') || keysPressed.has(' ')) {
      camera.position.y += MOVE_SPEED;
      moved = true;
    }
    if (keysPressed.has('e') || keysPressed.has('shift')) {
      camera.position.y -= MOVE_SPEED;
      moved = true;
    }

    if (moved) {
      updateCameraPosition();
      renderFrame(true);
    }
  };

  const handleContextMenu = (e: MouseEvent) => {
    e.preventDefault();
  };

  const loadScene = async (scenePath: string) => {
    try {
      setSceneLoaded(false);
      setStatus('Loading scene...');
      await invoke('import_gltf', { path: scenePath });

      setStatus('Compiling scene...');
      await invoke('compile_scene');

      const [width, height] = (await invoke('get_viewport_resolution')) as [number, number];

      if (canvasRef) {
        canvasRef.width = width;
        canvasRef.height = height;
      }

      camera.aspect_ratio = width / height;

      // Reset camera position for new scene
      camera.position = { x: 0, y: 0, z: 3 };
      yaw = 0;
      pitch = 0;
      updateCameraRotation();
      updateCameraPosition();

      setStatus('Rendering...');
      await renderFrame(true);

      setSceneLoaded(true);
      setStatus('Ready - WASD to move, drag to look');
    } catch (e) {
      setStatus(`Error: ${e}`);
      console.error('Failed to load scene:', e);
    }
  };

  const triggerRender = () => {
    if (sceneLoaded()) {
      renderFrame(true);
    }
  };

  // Sync native viewport bounds with container
  const syncNativeViewportBounds = async () => {
    if (renderMode() !== 'native' || !nativePlaceholderRef) return;

    const rect = nativePlaceholderRef.getBoundingClientRect();
    try {
      await invoke('sync_native_viewport', {
        bounds: {
          x: Math.floor(rect.left),
          y: Math.floor(rect.top),
          width: Math.floor(rect.width),
          height: Math.floor(rect.height),
        },
      });
    } catch (e) {
      console.error('Failed to sync native viewport bounds:', e);
    }
  };

  // Create native viewport
  const createNativeViewport = async () => {
    console.log('[Viewport] createNativeViewport called, ref exists:', !!nativePlaceholderRef);
    if (!nativePlaceholderRef) return;

    try {
      const rect = nativePlaceholderRef.getBoundingClientRect();
      console.log('[Viewport] Creating native viewport with bounds:', rect);
      await invoke('create_native_viewport', {
        bounds: {
          x: Math.floor(rect.left),
          y: Math.floor(rect.top),
          width: Math.floor(rect.width),
          height: Math.floor(rect.height),
        },
      });
      setNativeViewportReady(true);
      setStatus('Native viewport ready');
      console.log('[Viewport] Native viewport created successfully');
    } catch (e) {
      console.error('Failed to create native viewport:', e);
      setStatus(`Native viewport error: ${e}`);
      // Fall back to canvas mode
      setRenderMode('canvas');
    }
  };

  // Destroy native viewport
  const destroyNativeViewport = async () => {
    try {
      await invoke('destroy_native_viewport');
      setNativeViewportReady(false);
    } catch (e) {
      console.error('Failed to destroy native viewport:', e);
    }
  };

  // Switch render mode
  const switchRenderMode = async (mode: RenderMode) => {
    console.log('[Viewport] switchRenderMode called with:', mode, 'current:', renderMode());
    if (mode === renderMode()) return;

    if (mode === 'native') {
      console.log('[Viewport] Switching to native mode');
      setRenderMode('native');
      await createNativeViewport();
      if (sceneLoaded()) {
        renderFrame(true);
      }
    } else {
      console.log('[Viewport] Switching to canvas mode');
      await destroyNativeViewport();
      setRenderMode('canvas');
      if (sceneLoaded()) {
        renderFrame(true);
      }
    }
  };

  // Setup ResizeObserver when placeholder becomes available
  let resizeObserver: ResizeObserver | null = null;

  // Effect to handle render mode changes and setup observers
  createEffect(() => {
    const mode = renderMode();
    console.log('[Viewport] createEffect triggered, mode:', mode, 'ready:', nativeViewportReady());

    if (mode === 'native') {
      // Wait a tick for the DOM to update
      setTimeout(() => {
        console.log('[Viewport] setTimeout fired, ref exists:', !!nativePlaceholderRef, 'ready:', nativeViewportReady());
        if (nativePlaceholderRef && !nativeViewportReady()) {
          createNativeViewport();

          // Setup ResizeObserver
          if (!resizeObserver) {
            resizeObserver = new ResizeObserver(() => {
              syncNativeViewportBounds();
            });
            resizeObserver.observe(nativePlaceholderRef);
          }
        }
      }, 0);
    } else {
      // Clean up observer when switching away from native mode
      if (resizeObserver) {
        resizeObserver.disconnect();
        resizeObserver = null;
      }
    }
  });

  onMount(() => {
    props.ref?.({
      loadScene,
      render: triggerRender,
      setRenderMode: switchRenderMode,
      getRenderMode: () => renderMode(),
    });
    setStatus('Select a scene to begin');

    // Add global mouse up listener to handle drag release outside canvas
    document.addEventListener('mouseup', handleMouseUp);
    document.addEventListener('mousemove', handleMouseMove);
  });

  onCleanup(() => {
    document.removeEventListener('mouseup', handleMouseUp);
    document.removeEventListener('mousemove', handleMouseMove);

    // Cleanup native viewport if active
    if (renderMode() === 'native' && nativeViewportReady()) {
      destroyNativeViewport();
    }
  });

  return (
    <div class="viewport-wrapper">
      <div class="viewport-header">
        <span class="viewport-status">{status()}</span>
        <span class="viewport-camera">
          pos: ({props.cameraPosition().x.toFixed(2)}, {props.cameraPosition().y.toFixed(2)}, {props.cameraPosition().z.toFixed(2)})
        </span>
        <span class="viewport-stats">
          {renderTime().toFixed(2)}ms | {sampleCount()} samples | Mode: {renderMode()}
        </span>
      </div>
      <div
        ref={containerRef}
        class="viewport-canvas-container"
        tabIndex={0}
        onMouseDown={handleMouseDown}
        onWheel={handleWheel}
        onKeyDown={handleKeyDown}
        onKeyUp={handleKeyUp}
        onContextMenu={handleContextMenu}
      >
        <Show
          when={renderMode() === 'canvas'}
          fallback={
            <div ref={nativePlaceholderRef} class="native-viewport-placeholder">
              {/* Native window will render beneath this transparent div */}
            </div>
          }
        >
          <canvas ref={canvasRef} class="viewport-canvas" width={1280} height={720} />
        </Show>
      </div>
    </div>
  );
};
