import { Component, createSignal, createEffect, onMount, onCleanup, Accessor } from 'solid-js';
import * as api from '../../lib/api';
import './Viewport.css';

interface Camera {
  position: { x: number; y: number; z: number };
  rotation: { w: number; x: number; y: number; z: number };
  fov: number;
  near_plane: number;
  far_plane: number;
  aspect_ratio: number;
}

export interface CameraPosition {
  x: number;
  y: number;
  z: number;
}

export interface ViewportHandle {
  loadScene: (path: string) => Promise<void>;
  render: () => void;
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

export const Viewport: Component<ViewportProps> = (props) => {
  const [status, setStatus] = createSignal('Initializing...');
  const [renderTime, setRenderTime] = createSignal(0);
  const [sampleCount, setSampleCount] = createSignal(0);
  const [sceneLoaded, setSceneLoaded] = createSignal(false);
  let canvasRef: HTMLCanvasElement | undefined;
  let containerRef: HTMLDivElement | undefined;

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
      const result = await api.renderFrame(camera, clear);

      setRenderTime(result.render_time_ms);
      setSampleCount(result.sample_count);

      const pixels = await api.getRenderPixels();

      if (canvasRef) {
        const ctx = canvasRef.getContext('2d');
        if (ctx) {
          const imageData = ctx.createImageData(result.width, result.height);
          imageData.data.set(pixels);
          ctx.putImageData(imageData, 0, 0);
          setStatus('Rendering');
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

    yaw -= deltaX * MOUSE_SENSITIVITY;
    pitch -= deltaY * MOUSE_SENSITIVITY;

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
      await api.importGltf(scenePath);

      setStatus('Compiling scene...');
      await api.compileScene();

      const [width, height] = await api.getViewportResolution();

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

  onMount(() => {
    props.ref?.({ loadScene, render: triggerRender });
    setStatus('Select a scene to begin');

    // Add global mouse up listener to handle drag release outside canvas
    document.addEventListener('mouseup', handleMouseUp);
    document.addEventListener('mousemove', handleMouseMove);
  });

  onCleanup(() => {
    document.removeEventListener('mouseup', handleMouseUp);
    document.removeEventListener('mousemove', handleMouseMove);
  });

  return (
    <div class="viewport-wrapper">
      <div class="viewport-header">
        <span class="viewport-status">{status()}</span>
        <span class="viewport-camera">
          pos: ({props.cameraPosition().x.toFixed(2)}, {props.cameraPosition().y.toFixed(2)}, {props.cameraPosition().z.toFixed(2)})
        </span>
        <span class="viewport-stats">
          {renderTime().toFixed(2)}ms | {sampleCount()} samples
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
        <canvas ref={canvasRef} class="viewport-canvas" width={1280} height={720} />
      </div>
    </div>
  );
};
