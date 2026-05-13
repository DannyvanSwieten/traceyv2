// Cmd+K command-palette modal. Fuzzy-filters every registered Command
// against the user's query and runs the highlighted one on Enter. Subsequence
// matching is intentionally generous — typing "wralb" should still find
// "Write Albedo" — but exact-label-prefix matches outrank everything so the
// common case stays predictable.

import {
  Component,
  For,
  Show,
  createEffect,
  createMemo,
  createSignal,
  onMount,
  onCleanup,
} from 'solid-js';
import * as api from '../../lib/api';
import {
  commands,
  isCommandPaletteOpen,
  closeCommandPalette,
  type Command,
} from '../../lib/command_palette';
import './CommandPalette.css';

function scoreCommand(c: Command, q: string): number {
  if (!q) return 1;
  const label = c.label.toLowerCase();
  const haystack = (label + ' ' + (c.group ?? '') + ' ' + (c.keywords ?? '')).toLowerCase();
  // Exact substring → top tier; label-prefix wins above generic substring.
  if (label.startsWith(q)) return 100;
  if (haystack.includes(q)) return 50;
  // Subsequence over the label only — keeps the noise down vs. matching
  // through group/keywords letter-by-letter.
  let i = 0;
  for (const ch of label) {
    if (q[i] === ch) i++;
    if (i === q.length) return 10;
  }
  return 0;
}

export const CommandPalette: Component = () => {
  const [query, setQuery] = createSignal('');
  const [highlight, setHighlight] = createSignal(0);
  let inputRef: HTMLInputElement | undefined;

  const filtered = createMemo(() => {
    const q = query().toLowerCase().trim();
    const visible = commands().filter((c) => !c.when || c.when());
    if (!q) {
      // No query → show the first 50 in registration order so the user can
      // browse with arrow keys. Group label is a useful organising hint.
      return visible.slice(0, 50);
    }
    return visible
      .map((c) => ({ c, s: scoreCommand(c, q) }))
      .filter((x) => x.s > 0)
      .sort((a, b) => b.s - a.s)
      .slice(0, 50)
      .map((x) => x.c);
  });

  function pick(c: Command): void {
    closeCommandPalette();
    Promise.resolve(c.run()).catch((err) => console.error('command failed:', c.id, err));
  }

  // The native Metal-layer viewport sits above the WebView in z-order, so
  // any DOM modal (including this one) is obscured by it. Hide the
  // viewport while the palette is open, restore on close.
  createEffect(() => {
    const open = isCommandPaletteOpen();
    api.setViewportVisible(!open).catch(() => {});
  });

  onMount(() => {
    queueMicrotask(() => inputRef?.focus());
    const onKey = (e: KeyboardEvent) => {
      if (!isCommandPaletteOpen()) return;
      if (e.key === 'Escape') {
        e.preventDefault();
        closeCommandPalette();
        return;
      }
      if (e.key === 'ArrowDown') {
        e.preventDefault();
        const n = filtered().length;
        setHighlight((h) => (n === 0 ? 0 : (h + 1) % n));
        return;
      }
      if (e.key === 'ArrowUp') {
        e.preventDefault();
        const n = filtered().length;
        setHighlight((h) => (n === 0 ? 0 : (h - 1 + n) % n));
        return;
      }
      if (e.key === 'Enter') {
        e.preventDefault();
        const c = filtered()[highlight()];
        if (c) pick(c);
        return;
      }
    };
    window.addEventListener('keydown', onKey, { capture: true });
    onCleanup(() => window.removeEventListener('keydown', onKey, { capture: true } as EventListenerOptions));
  });

  return (
    <Show when={isCommandPaletteOpen()}>
      <div class="command-palette-backdrop" onPointerDown={closeCommandPalette}>
        <div class="command-palette" onPointerDown={(e) => e.stopPropagation()}>
          <input
            ref={inputRef}
            class="command-palette-input"
            type="text"
            placeholder="Type a command…"
            value={query()}
            autocomplete="off"
            autocorrect="off"
            spellcheck={false}
            onInput={(e) => {
              setQuery(e.currentTarget.value);
              setHighlight(0);
            }}
          />
          <ul class="command-palette-list">
            <For
              each={filtered()}
              fallback={<li class="command-palette-empty">No matches</li>}
            >
              {(c, i) => (
                <li
                  class="command-palette-row"
                  classList={{ 'command-palette-row--active': i() === highlight() }}
                  onMouseEnter={() => setHighlight(i())}
                  onClick={() => pick(c)}
                >
                  <span class="command-palette-row-label">{c.label}</span>
                  <Show when={c.group}>
                    <span class="command-palette-row-group">{c.group}</span>
                  </Show>
                  <Show when={c.hint}>
                    <span class="command-palette-row-hint">{c.hint}</span>
                  </Show>
                </li>
              )}
            </For>
          </ul>
        </div>
      </div>
    </Show>
  );
};
