// Tiny corner-overlay status indicator shown while a Blender-style modal
// transform (G/R/S) is active. Just a chip — the actual transform updates
// happen via the viewport pointer-event chain wired up in App.tsx.

import { Component, Show } from 'solid-js';
import { grabState } from '../../lib/viewport_grab';
import './GrabOverlay.css';

export const GrabOverlay: Component = () => {
  const verb = () => {
    const g = grabState();
    if (!g) return '';
    switch (g.kind) {
      case 'translate': return 'Translate';
      case 'rotate':    return 'Rotate';
      case 'scale':     return 'Scale';
    }
  };
  const axisLabel = () => {
    const g = grabState();
    if (!g || g.axis === null) return 'free';
    return g.axis.toUpperCase();
  };
  return (
    <Show when={grabState()}>
      <div class="grab-overlay">
        <span class="grab-overlay-verb">{verb()}</span>
        <span class="grab-overlay-axis">{axisLabel()}</span>
        <span class="grab-overlay-hint">drag · X/Y/Z · ⏎ commit · esc cancel</span>
      </div>
    </Show>
  );
};
