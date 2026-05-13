// Hold-Q pie menu. Pressing Q opens a radial menu at the cursor; releasing
// Q selects whichever wedge the cursor is currently over (or no action if
// inside the dead-zone). Inspired by Maya's hotbox and Blender's pie menus.
//
// Wedges are user-configurable per-canvas-context — the App registers the
// viewport set; other panels could register their own and toggle the
// "active context" if/when we add modal panel focus.

import { createSignal } from 'solid-js';

export interface PieWedge {
  label: string;
  hint?: string;
  run: () => void;
}

export interface PieMenuState {
  cx: number;
  cy: number;
  wedges: PieWedge[];
  activeWedge: number | null; // -1 / null = dead-zone
}

const [state, setState] = createSignal<PieMenuState | null>(null);
export const pieMenuState = state;
export const isPieMenuOpen = () => state() !== null;

// Wedge registry — App registers the viewport set on mount.
let registeredWedges: PieWedge[] = [];
export function setPieMenuWedges(wedges: PieWedge[]): void {
  registeredWedges = wedges;
}

export function openPieMenu(cx: number, cy: number): void {
  if (registeredWedges.length === 0) return;
  setState({ cx, cy, wedges: registeredWedges, activeWedge: null });
}

const DEAD_ZONE_PX = 30;

// Recompute the active wedge from the current cursor position. Returns the
// wedge index, or null when inside the dead-zone.
export function updatePieMenuCursor(x: number, y: number): void {
  const s = state();
  if (!s) return;
  const dx = x - s.cx;
  const dy = y - s.cy;
  const dist = Math.sqrt(dx * dx + dy * dy);
  if (dist < DEAD_ZONE_PX) {
    if (s.activeWedge !== null) setState({ ...s, activeWedge: null });
    return;
  }
  // Angle in [0, 2π) measured from straight up (12 o'clock), going clockwise.
  // atan2(dy, dx) is from +x axis CCW; convert via (π/2 + atan2) mod 2π.
  let angle = Math.atan2(dy, dx) + Math.PI * 0.5;
  if (angle < 0) angle += Math.PI * 2;
  const slice = (Math.PI * 2) / s.wedges.length;
  const idx = Math.floor((angle + slice * 0.5) / slice) % s.wedges.length;
  if (s.activeWedge !== idx) setState({ ...s, activeWedge: idx });
}

// Close + run the active wedge. No-op if cursor is in the dead-zone.
export function commitPieMenu(): void {
  const s = state();
  if (!s) return;
  setState(null);
  if (s.activeWedge !== null) {
    try { s.wedges[s.activeWedge].run(); }
    catch (e) { console.error('pie menu wedge failed:', e); }
  }
}

export function dismissPieMenu(): void {
  setState(null);
}
