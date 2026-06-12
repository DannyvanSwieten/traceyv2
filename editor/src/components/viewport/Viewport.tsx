import { Component, Show, createSignal, onMount, onCleanup, Accessor } from 'solid-js';
import * as api from '../../lib/api';
import { renderStats } from '../../stores/render_stats';
import './Viewport.css';

// Compact count formatting for the header stats readout: 1234 → "1.2k",
// 3500000 → "3.5M". Keeps the strip width stable as scenes grow.
function fmtCount(n: number): string {
  if (n >= 1e6) return `${(n / 1e6).toFixed(1)}M`;
  if (n >= 1e3) return `${(n / 1e3).toFixed(1)}k`;
  return String(n);
}

// Re-exported for compatibility with App.tsx until camera control fully moves
// to native. The native side currently owns camera state; this stays as a
// stub so the prop interface doesn't break.
export interface CameraPosition {
  x: number;
  y: number;
  z: number;
}

export interface ViewportHandle {
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
  //
  // Resolution updates are debounced because a window-edge drag fires
  // ResizeObserver many times in quick succession and each setViewportResolution
  // recreates the path tracer (image alloc + pipeline rebuild + program reupload).
  let resolutionTimer: ReturnType<typeof setTimeout> | null = null;
  let lastReportedPixelW = 0;
  let lastReportedPixelH = 0;
  const RESOLUTION_DEBOUNCE_MS = 250;

  const scheduleResolutionUpdate = (pixelW: number, pixelH: number) => {
    if (pixelW === lastReportedPixelW && pixelH === lastReportedPixelH) return;
    if (resolutionTimer !== null) clearTimeout(resolutionTimer);
    resolutionTimer = setTimeout(() => {
      resolutionTimer = null;
      lastReportedPixelW = pixelW;
      lastReportedPixelH = pixelH;
      api.setViewportResolution(pixelW, pixelH).catch((e) =>
        console.error('setViewportResolution failed:', e)
      );
    }, RESOLUTION_DEBOUNCE_MS);
  };

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
    // Match the path tracer's render resolution to the viewport's actual
    // device-pixel dimensions, otherwise the swapchain blit stretches.
    const dpr = window.devicePixelRatio || 1;
    if (w > 0 && h > 0) {
      scheduleResolutionUpdate(Math.round(w * dpr), Math.round(h * dpr));
    }
  };

  let resizeObserver: ResizeObserver | undefined;
  let scrollListener: (() => void) | undefined;

  onMount(() => {
    props.ref?.({ render: () => {} });
    setStatus('Ready');
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
        {/* Live render stats from the native render_stats broadcast (~4 Hz).
            The DOM can't overlay the canvas itself — the native Metal layer
            sits above the WebView — so the header strip is the HUD. */}
        <Show when={renderStats()}>
          {(s) => (
            <span class="viewport-stats" title="FPS · triangles · instances · path-tracer samples">
              {s().fps.toFixed(0)} fps · {fmtCount(s().triangles)} tris ·{' '}
              {fmtCount(s().instances)} inst
              <Show when={s().max_samples > 0}>
                {' '}· {s().samples}/{s().max_samples} spp
              </Show>
            </span>
          )}
        </Show>
        <span class="viewport-camera">
          pos: ({props.cameraPosition().x.toFixed(2)},{' '}
          {props.cameraPosition().y.toFixed(2)}, {props.cameraPosition().z.toFixed(2)})
        </span>
      </div>
      <div ref={placeholderRef} class="viewport-canvas-container" />
    </div>
  );
};
