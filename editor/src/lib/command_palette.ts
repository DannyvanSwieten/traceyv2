// Global command palette. Anything user-actionable in the app can register
// here — node-creation, save/load, viewport toggles, undo/redo, navigation —
// and the user can invoke it by typing in the Cmd+K modal. Acts as a force
// multiplier on every feature: each new toggle becomes discoverable for
// free without hunting a menu.
//
// Registration is reactive: components register on mount and the registry
// (a Solid signal) updates so an open palette refreshes as new commands
// appear. Most call sites can register at module-load time; ones whose
// availability depends on selection state (e.g. "Frame Selected") use a
// `when` predicate evaluated per render.

import { createSignal } from 'solid-js';

export interface Command {
  id: string;
  label: string;
  group?: string;
  // Free-text keywords for fuzzy matching beyond the label itself.
  keywords?: string;
  // Right-aligned hint, typically the keyboard shortcut.
  hint?: string;
  // Optional gate. False means the command is registered but hidden — the
  // typical use is "only show Frame Selected when something is selected".
  when?: () => boolean;
  run: () => void | Promise<void>;
}

const [registry, setRegistry] = createSignal<Map<string, Command>>(new Map());

// Register a command. Returns an unsubscribe function so callers in
// onMount/onCleanup can unregister on unmount.
export function registerCommand(c: Command): () => void {
  setRegistry((m) => {
    const next = new Map(m);
    next.set(c.id, c);
    return next;
  });
  return () => {
    setRegistry((m) => {
      const next = new Map(m);
      next.delete(c.id);
      return next;
    });
  };
}

// Register many commands at once. Returns one unsubscribe that drops them all.
export function registerCommands(cs: Command[]): () => void {
  setRegistry((m) => {
    const next = new Map(m);
    for (const c of cs) next.set(c.id, c);
    return next;
  });
  return () => {
    setRegistry((m) => {
      const next = new Map(m);
      for (const c of cs) next.delete(c.id);
      return next;
    });
  };
}

export function commands(): Command[] {
  return Array.from(registry().values());
}

// ── Modal visibility ───────────────────────────────────────────────────
const [open, setOpen] = createSignal(false);
export const isCommandPaletteOpen = open;
export function openCommandPalette(): void { setOpen(true); }
export function closeCommandPalette(): void { setOpen(false); }
export function toggleCommandPalette(): void { setOpen((v) => !v); }
