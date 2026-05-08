import { Component, createSignal, onMount, onCleanup, Accessor } from 'solid-js';
import * as api from '../../lib/api';
import './Viewport.css';

// Re-exported for compatibility with App.tsx until camera control fully moves
// to native. The native side currently owns camera state; this stays as a
// stub so the prop interface doesn't break.
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

// The viewport is rendered by a native CAMetalLayer overlaid on top of the
// WKWebView (z-order above the page). This component is just a transparent
// placeholder whose layout rect we report to the host every time it changes —
// the host repositions/sizes its Metal view to match.
export const Viewport: Component<ViewportProps> = (props) => {
  const [status, setStatus] = createSignal('Initializing...');
  let placeholderRef: HTMLDivElement | undefined;

  // Push the placeholder's current bounding box to the native side so the
  // metal-view overlay tracks it. Coordinates are in document logical pixels;
  // the host converts to its window-local Cocoa coords.
  const reportRect = () => {
    if (!placeholderRef) return;
    const r = placeholderRef.getBoundingClientRect();
    const x = Math.round(r.left);
    const y = Math.round(r.top);
    const w = Math.max(0, Math.round(r.width));
    const h = Math.max(0, Math.round(r.height));
    api.setViewportRect(x, y, w, h).catch((e) =>
      console.error('setViewportRect failed:', e)
    );
  };

  const loadScene = async (scenePath: string) => {
    try {
      setStatus('Loading scene...');
      await api.importGltf(scenePath);
      setStatus('Compiling scene...');
      await api.compileScene();
      setStatus('Ready');
    } catch (e) {
      setStatus(`Error: ${e}`);
      console.error('Failed to load scene:', e);
    }
  };

  let resizeObserver: ResizeObserver | undefined;
  let scrollListener: (() => void) | undefined;

  onMount(() => {
    props.ref?.({ loadScene, render: () => {} });
    setStatus('Select a scene to begin');
    reportRect();

    if (placeholderRef) {
      resizeObserver = new ResizeObserver(() => reportRect());
      resizeObserver.observe(placeholderRef);
    }

    scrollListener = () => reportRect();
    window.addEventListener('resize', scrollListener);
    window.addEventListener('scroll', scrollListener, true);
  });

  onCleanup(() => {
    resizeObserver?.disconnect();
    if (scrollListener) {
      window.removeEventListener('resize', scrollListener);
      window.removeEventListener('scroll', scrollListener, true);
    }
    api.setViewportVisible(false).catch(() => {});
  });

  return (
    <div class="viewport-wrapper">
      <div class="viewport-header">
        <span class="viewport-status">{status()}</span>
        <span class="viewport-camera">
          pos: ({props.cameraPosition().x.toFixed(2)},{' '}
          {props.cameraPosition().y.toFixed(2)}, {props.cameraPosition().z.toFixed(2)})
        </span>
      </div>
      <div ref={placeholderRef} class="viewport-canvas-container" />
    </div>
  );
};
