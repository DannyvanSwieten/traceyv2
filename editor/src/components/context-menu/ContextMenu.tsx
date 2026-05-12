// Reusable graph context menu. Two surfaces:
//
//   • flat list of `MenuAction`s     — for the right-click-a-node action menu
//                                       (Delete, Wire to Output, Enter subnet …).
//   • nested `MenuCategory[]` tree   — for the right-click-empty-canvas
//                                       "add node" menu. The top has a search
//                                       input; while there's text in it, the
//                                       tree collapses into a single flat
//                                       results list (all entries matching the
//                                       query, with their category name shown
//                                       inline as a hint).
//
// Positioning: caller passes viewport-space coordinates and the menu places
// itself so it stays on-screen, flipping to the left/up when it would
// otherwise spill off the edge. Closes on ESC, on outside-click, and when
// any leaf is invoked.

import {
  Component,
  For,
  Show,
  createMemo,
  createSignal,
  onCleanup,
  onMount,
} from 'solid-js';
import './ContextMenu.css';

export interface MenuLeaf {
  kind: 'leaf';
  label: string;
  // Hint text shown right-aligned (typically a category label when this
  // leaf is collapsed into search results, or a keyboard hint like "⌫").
  hint?: string;
  // Optional disabled flag — greys out and blocks click. Used e.g. for
  // "Enter Subnet" on non-subnet nodes when we still want to keep menu
  // shape stable across selections.
  disabled?: boolean;
  onPick: () => void;
}

export interface MenuCategory {
  kind: 'category';
  label: string;
  entries: MenuEntry[];
}

export type MenuEntry = MenuLeaf | MenuCategory;

export interface ContextMenuProps {
  // Viewport-space pixel coordinates where the menu was triggered. The
  // menu's top-left corner anchors here unless that would push it off-screen.
  x: number;
  y: number;
  // Top-level entries. If any are categories, the user-visible menu shows
  // them as submenus that expand on hover. The optional search filter
  // (toggled by `showSearch`) flattens the whole tree into a single list.
  entries: MenuEntry[];
  // Show the search bar at the top. Defaults to false so the action menu
  // (which is tiny and doesn't need filtering) stays compact.
  showSearch?: boolean;
  // Placeholder for the search input.
  searchPlaceholder?: string;
  onClose: () => void;
}

const MENU_MIN_WIDTH = 220;
const SUBMENU_OVERLAP = -4;  // negative = slight overlap, prevents flicker

export const ContextMenu: Component<ContextMenuProps> = (props) => {
  const [query, setQuery] = createSignal('');
  const [hoveredCat, setHoveredCat] = createSignal<MenuCategory | null>(null);
  let rootRef: HTMLDivElement | undefined;
  let searchRef: HTMLInputElement | undefined;

  // Auto-focus the search input on mount so the user can start typing
  // immediately after right-clicking — same UX as Houdini's TAB menu.
  onMount(() => {
    if (props.showSearch && searchRef) {
      searchRef.focus();
    }
    const onDocPointer = (e: PointerEvent) => {
      if (!rootRef) return;
      if (!rootRef.contains(e.target as Node)) {
        props.onClose();
      }
    };
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        e.preventDefault();
        e.stopPropagation();
        props.onClose();
      }
    };
    // pointerdown (not click) so a right-click outside also dismisses
    // immediately. Capture phase so we beat any other context-menu handlers
    // that might re-open against the same pointerdown.
    document.addEventListener('pointerdown', onDocPointer, true);
    document.addEventListener('keydown', onKey, true);
    onCleanup(() => {
      document.removeEventListener('pointerdown', onDocPointer, true);
      document.removeEventListener('keydown', onKey, true);
    });
  });

  // Flatten the tree into leaves for search. Each leaf carries its
  // category label so the result row can show "Translate · Modifiers".
  const allLeaves = createMemo<{ leaf: MenuLeaf; categoryLabel: string }[]>(() => {
    const out: { leaf: MenuLeaf; categoryLabel: string }[] = [];
    const walk = (entries: MenuEntry[], catLabel: string) => {
      for (const e of entries) {
        if (e.kind === 'leaf') {
          out.push({ leaf: e, categoryLabel: catLabel });
        } else {
          walk(e.entries, e.label);
        }
      }
    };
    walk(props.entries, '');
    return out;
  });

  const searchResults = createMemo<{ leaf: MenuLeaf; categoryLabel: string }[]>(() => {
    const q = query().trim().toLowerCase();
    if (!q) return [];
    return allLeaves().filter(({ leaf, categoryLabel }) => {
      return (
        leaf.label.toLowerCase().includes(q) ||
        categoryLabel.toLowerCase().includes(q)
      );
    });
  });

  // Position so the menu stays on-screen. Computed once on mount —
  // submenus reposition relative to their parent row, not to this anchor.
  const position = createMemo(() => {
    // We render at the requested point first, then a microtask after mount
    // measures the actual element and flips if needed. To keep the
    // computation pure (no DOM read during initial render), use the
    // viewport size as a rough guide.
    const vw = window.innerWidth;
    const vh = window.innerHeight;
    const x = props.x + MENU_MIN_WIDTH > vw ? Math.max(8, vw - MENU_MIN_WIDTH - 8) : props.x;
    const y = props.y;  // top-edge clamp is handled by max-height + scroll
    return { left: x, top: y, maxHeight: vh - y - 8 };
  });

  function handleLeaf(leaf: MenuLeaf) {
    if (leaf.disabled) return;
    leaf.onPick();
    props.onClose();
  }

  return (
    <div
      ref={rootRef}
      class="ctx-menu"
      style={{
        left: `${position().left}px`,
        top: `${position().top}px`,
        'max-height': `${position().maxHeight}px`,
        'min-width': `${MENU_MIN_WIDTH}px`,
      }}
      onContextMenu={(e) => { e.preventDefault(); e.stopPropagation(); }}
    >
      <Show when={props.showSearch}>
        <div class="ctx-menu__search-row">
          <input
            ref={searchRef}
            class="ctx-menu__search"
            type="text"
            value={query()}
            placeholder={props.searchPlaceholder ?? 'Search…'}
            autocomplete="off"
            autocorrect="off"
            spellcheck={false}
            onInput={(e) => setQuery(e.currentTarget.value)}
            onKeyDown={(e) => {
              if (e.key === 'Enter') {
                // Pick the first search result on Enter — gives the
                // "tab menu" feel where typing + Enter creates a node.
                const r = searchResults();
                if (r.length > 0) {
                  e.preventDefault();
                  handleLeaf(r[0].leaf);
                }
              }
            }}
          />
        </div>
      </Show>

      <Show
        when={query().trim().length > 0}
        fallback={
          <div class="ctx-menu__list">
            <For each={props.entries}>
              {(entry) => (
                <Show
                  when={entry.kind === 'category'}
                  fallback={
                    <button
                      class="ctx-menu__row"
                      classList={{ 'ctx-menu__row--disabled': (entry as MenuLeaf).disabled }}
                      onClick={() => handleLeaf(entry as MenuLeaf)}
                      onMouseEnter={() => setHoveredCat(null)}
                    >
                      <span class="ctx-menu__row-label">{(entry as MenuLeaf).label}</span>
                      <Show when={(entry as MenuLeaf).hint}>
                        <span class="ctx-menu__row-hint">{(entry as MenuLeaf).hint}</span>
                      </Show>
                    </button>
                  }
                >
                  <CategoryRow
                    cat={entry as MenuCategory}
                    isOpen={hoveredCat() === entry}
                    onHover={() => setHoveredCat(entry as MenuCategory)}
                    onPickLeaf={(l) => handleLeaf(l)}
                  />
                </Show>
              )}
            </For>
          </div>
        }
      >
        <div class="ctx-menu__list ctx-menu__list--results">
          <Show
            when={searchResults().length > 0}
            fallback={<div class="ctx-menu__empty">No matches</div>}
          >
            <For each={searchResults()}>
              {({ leaf, categoryLabel }) => (
                <button
                  class="ctx-menu__row"
                  classList={{ 'ctx-menu__row--disabled': leaf.disabled }}
                  onClick={() => handleLeaf(leaf)}
                >
                  <span class="ctx-menu__row-label">{leaf.label}</span>
                  <Show when={categoryLabel}>
                    <span class="ctx-menu__row-hint">{categoryLabel}</span>
                  </Show>
                </button>
              )}
            </For>
          </Show>
        </div>
      </Show>
    </div>
  );
};

// One row in the top-level menu when it represents a category. Renders
// the child entries as a viewport-fixed sibling popup while the row is
// hovered. Fixed positioning is deliberate — the parent menu has
// `overflow: auto` (so a long catalog still scrolls), which would
// otherwise clip an absolutely-positioned submenu. The fixed submenu
// escapes the ancestor's overflow chain and we compute its viewport
// coordinates from the parent row's getBoundingClientRect at hover time.
interface CategoryRowProps {
  cat: MenuCategory;
  isOpen: boolean;
  onHover: () => void;
  onPickLeaf: (leaf: MenuLeaf) => void;
}

const CategoryRow: Component<CategoryRowProps> = (props) => {
  let rowRef: HTMLDivElement | undefined;
  // Submenu position is recomputed every hover (and on the first open of
  // this row) so a scrolled parent menu places submenus correctly.
  const [submenuPos, setSubmenuPos] = createSignal<{ left: number; top: number } | null>(null);

  function recomputeSubmenuPos() {
    if (!rowRef) return;
    const rect = rowRef.getBoundingClientRect();
    const rightSpace = window.innerWidth - rect.right;
    // Flip to the left side of the parent row when there isn't room for
    // the submenu on the right.
    const flipLeft = rightSpace < MENU_MIN_WIDTH + 16;
    const left = flipLeft
      ? Math.max(8, rect.left - MENU_MIN_WIDTH + Math.abs(SUBMENU_OVERLAP))
      : rect.right + SUBMENU_OVERLAP;
    setSubmenuPos({ left, top: rect.top });
  }

  return (
    <div
      ref={rowRef}
      class="ctx-menu__cat-row-wrap"
      onMouseEnter={() => { props.onHover(); recomputeSubmenuPos(); }}
    >
      <button
        class="ctx-menu__row ctx-menu__row--category"
        classList={{ 'ctx-menu__row--active': props.isOpen }}
      >
        <span class="ctx-menu__row-label">{props.cat.label}</span>
        <span class="ctx-menu__row-chevron">▸</span>
      </button>
      <Show when={props.isOpen && submenuPos()}>
        <div
          class="ctx-menu__submenu"
          style={{
            left: `${submenuPos()!.left}px`,
            top: `${submenuPos()!.top}px`,
            'min-width': `${MENU_MIN_WIDTH}px`,
            // Cap height to remaining viewport below the parent row so the
            // submenu itself scrolls when a category has many entries
            // (Math/Noise can be long).
            'max-height': `${Math.max(120, window.innerHeight - submenuPos()!.top - 8)}px`,
          }}
        >
          <For each={props.cat.entries}>
            {(child) => (
              <Show
                when={child.kind === 'leaf'}
                fallback={
                  // Nested categories: render as a label-only row. We don't
                  // support recursive submenus yet (catalogs are 1 level
                  // deep); show the entries inline as a fallback if a
                  // catalog ever grows a sub-category.
                  <div class="ctx-menu__cat-section">{(child as MenuCategory).label}</div>
                }
              >
                <button
                  class="ctx-menu__row"
                  classList={{ 'ctx-menu__row--disabled': (child as MenuLeaf).disabled }}
                  onClick={() => props.onPickLeaf(child as MenuLeaf)}
                >
                  <span class="ctx-menu__row-label">{(child as MenuLeaf).label}</span>
                  <Show when={(child as MenuLeaf).hint}>
                    <span class="ctx-menu__row-hint">{(child as MenuLeaf).hint}</span>
                  </Show>
                </button>
              </Show>
            )}
          </For>
        </div>
      </Show>
    </div>
  );
};
