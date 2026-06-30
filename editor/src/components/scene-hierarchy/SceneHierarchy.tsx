import {
  Component,
  For,
  Show,
  createSignal,
  createMemo,
  createEffect,
  onMount,
  onCleanup,
} from 'solid-js';
import * as api from '../../lib/api';
import type { Actor as ApiActor, LightKind } from '../../lib/api';
import './SceneHierarchy.css';

export type Actor = ApiActor;

interface TreeNode {
  actor: Actor;
  children: TreeNode[];
}

export type DropMode = 'before' | 'inside' | 'after';

// One row of the FLATTENED, currently-visible tree (the unit the virtualizer
// renders). Flattening + windowing is what lets the hierarchy scale to a
// production scene (Marbles imports thousands of actors): only the ~viewport's
// worth of rows are ever in the DOM, instead of the whole expanded tree.
interface FlatRow {
  node: TreeNode;
  depth: number;
}

// Fixed row height (px). The CSS pins .tree-item-row to exactly this so the
// windowing math (index → translateY) lines up with what's painted; keep the
// two in sync.
const ROW_H = 28;
// Rows rendered above/below the viewport so a fast scroll never shows a gap.
const OVERSCAN = 8;

interface SceneHierarchyProps {
  actors: () => Actor[];
  selectedActorId: () => number | null;
  onActorSelect: (id: number) => void;
  // Optional optimistic-update callback the parent uses to flip the `visible`
  // flag on its local actors signal without waiting for the next round-trip.
  onActorVisibilityChange?: (actorId: number, visible: boolean) => void;
  // Remove the actor — the parent translates this to a SOP node deletion +
  // re-cook. Optional so the hierarchy still mounts in read-only contexts.
  onActorDelete?: (actorId: number) => void;
  // Drag-and-drop reorder/reparent. `mode` is 'before' or 'after' for
  // sibling-order moves and 'inside' for reparent. Parent commits to the
  // SOP graph and re-cooks.
  onActorReorder?: (sourceId: number, targetId: number, mode: DropMode) => void;
  // Double-click rename. The parent writes the new name to the emit SOP
  // node's `name` param and re-cooks. Omit to disable inline renaming.
  onActorRename?: (actorId: number, name: string) => void;
  // Returns true if the target actor's source SOP node is a subnet (or
  // otherwise accepts children). Used to suppress the "drop INSIDE" zone on
  // rows that can't take a child.
  canDropInside?: (targetId: number) => boolean;
  // Optional handler for the "+ Add Light" affordance in the panel header.
  // The parent calls api.createLight(type), refreshes its actors signal,
  // and (typically) selects the newly created light row. Omit to hide the
  // button entirely.
  onLightAdd?: (type: LightKind) => void;
  isLoading: () => boolean;
}

// Light-type metadata for the "+ Add Light" popover. Order matches the
// menu reading: Dome first because it's the most common "I want lighting,
// any lighting" action; Sun next as the second most-reached-for type.
const LIGHT_MENU: ReadonlyArray<{ kind: LightKind; label: string; icon: string }> = [
  { kind: 'dome',  label: 'Dome',  icon: '🌐' },
  { kind: 'sun',   label: 'Sun',   icon: '☀️' },
  { kind: 'point', label: 'Point', icon: '●' },
  { kind: 'area',  label: 'Area',  icon: '▭' },
];

function buildActorTree(actors: Actor[]): TreeNode[] {
  const actorMap = new Map(actors.map((a) => [a.id, a]));
  const childIds = new Set(actors.flatMap((a) => a.children));

  const roots = actors.filter((a) => !childIds.has(a.id));

  function buildNode(actor: Actor): TreeNode {
    return {
      actor,
      children: actor.children
        .map((id) => actorMap.get(id))
        .filter((a): a is Actor => a !== undefined)
        .map(buildNode),
    };
  }

  return roots.map(buildNode);
}

// Drag payload uses a custom mime type so unrelated drag sources (text from
// the editor, files, etc.) can't be mistaken for hierarchy reorders.
const DRAG_MIME = 'application/x-tracey-actor-id';

// Filter the tree to nodes whose name matches `query` (case-insensitive
// substring) — keeping ancestors of matches so the hierarchy context stays
// visible while filtering.
function filterTree(nodes: TreeNode[], query: string): TreeNode[] {
  const q = query.toLowerCase();
  const out: TreeNode[] = [];
  for (const n of nodes) {
    const kids = filterTree(n.children, query);
    if (n.actor.name.toLowerCase().includes(q) || kids.length > 0) {
      out.push({ actor: n.actor, children: kids });
    }
  }
  return out;
}

export const SceneHierarchy: Component<SceneHierarchyProps> = (props) => {
  const [search, setSearch] = createSignal('');
  const [lightMenuOpen, setLightMenuOpen] = createSignal(false);

  // ── Tree state, lifted out of the per-row components ──────────────────────
  // Virtualization recycles row components as you scroll, so transient row
  // state can't live inside them. Expanded-set is keyed by actor id: absence
  // means COLLAPSED. The tree defaults to collapsed (only roots visible) so a
  // production-scale import (Marbles = thousands of actors) opens to a handful
  // of rows you drill into, instead of dumping the whole nested tree open.
  const [expandedIds, setExpandedIds] = createSignal<Set<number>>(new Set());
  const [renamingId, setRenamingId] = createSignal<number | null>(null);
  const [drag, setDrag] = createSignal<{ id: number; zone: DropMode } | null>(null);

  const toggleExpanded = (id: number) =>
    setExpandedIds((prev) => {
      const next = new Set(prev);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });

  const searchActive = () => search().trim().length > 0;

  const tree = createMemo(() => {
    const full = buildActorTree(props.actors());
    const q = search().trim();
    return q ? filterTree(full, q) : full;
  });

  // Flatten the visible tree (respecting collapse state) into the linear row
  // list the virtualizer windows over. While a filter is active everything is
  // treated as expanded so matches deep in the tree stay visible.
  const flatRows = createMemo<FlatRow[]>(() => {
    const out: FlatRow[] = [];
    const expandedSet = expandedIds();
    const expandAll = searchActive();
    const walk = (nodes: TreeNode[], depth: number) => {
      for (const node of nodes) {
        out.push({ node, depth });
        if (node.children.length > 0 && (expandAll || expandedSet.has(node.actor.id))) {
          walk(node.children, depth + 1);
        }
      }
    };
    walk(tree(), 0);
    return out;
  });

  // ── Virtual window ────────────────────────────────────────────────────────
  let scrollEl: HTMLDivElement | undefined;
  let sizerEl: HTMLDivElement | undefined;
  let windowEl: HTMLDivElement | undefined;
  const [scrollTop, setScrollTop] = createSignal(0);
  const [viewportH, setViewportH] = createSignal(600);

  const range = createMemo(() => {
    const total = flatRows().length;
    const first = Math.max(0, Math.floor(scrollTop() / ROW_H) - OVERSCAN);
    const visibleCount = Math.ceil(viewportH() / ROW_H) + OVERSCAN * 2;
    const last = Math.min(total, first + visibleCount);
    return { first, last };
  });
  const visibleRows = createMemo(() => flatRows().slice(range().first, range().last));

  // Drive the scroll geometry through CSS custom properties (the lint config
  // blocks dynamic style={...} attributes — same reason --tree-depth is set via
  // a ref). --sizer-h reserves the full scroll range; --vy offsets the rendered
  // window to its true position.
  createEffect(() => {
    if (sizerEl) sizerEl.style.setProperty('--sizer-h', `${flatRows().length * ROW_H}px`);
  });
  createEffect(() => {
    if (windowEl) windowEl.style.setProperty('--vy', `${range().first * ROW_H}px`);
  });

  onMount(() => {
    if (!scrollEl) return;
    setViewportH(scrollEl.clientHeight || 600);
    const ro = new ResizeObserver(() => setViewportH(scrollEl!.clientHeight || 600));
    ro.observe(scrollEl);
    onCleanup(() => ro.disconnect());
  });

  // Delete/Backspace on the panel deletes the selected actor — same gesture
  // the SOP canvas uses for the node version.
  const onKeyDown = (e: KeyboardEvent) => {
    if (e.key !== 'Delete' && e.key !== 'Backspace') return;
    const id = props.selectedActorId();
    if (id === null) return;
    // Don't fight with native text-input deletion when an inspector input is
    // focused — only delete when the hierarchy itself owns focus.
    const target = e.target as HTMLElement | null;
    if (target && (target.tagName === 'INPUT' || target.tagName === 'TEXTAREA')) {
      return;
    }
    e.preventDefault();
    props.onActorDelete?.(id);
  };

  // ── Per-row rendering ───────────────────────────────────────────────────
  // Defined inside the component so it closes over the lifted state + props
  // instead of threading a dozen callbacks through. One flattened row; no
  // recursion (children are their own rows in the flat list).
  const Row: Component<{ row: FlatRow }> = (rp) => {
    const actor = () => rp.row.node.actor;
    const hasChildren = () => rp.row.node.children.length > 0;
    const expanded = () => searchActive() || expandedIds().has(actor().id);
    const isVisible = () => actor().visible !== false;
    const renaming = () => renamingId() === actor().id;
    const dropZone = () => (drag()?.id === actor().id ? drag()!.zone : null);

    const toggleVisibility = async (e: MouseEvent) => {
      e.stopPropagation();
      const next = !isVisible();
      props.onActorVisibilityChange?.(actor().id, next);
      try {
        await api.setActorVisible(actor().id, next);
      } catch (err) {
        console.warn('setActorVisible failed:', err);
        props.onActorVisibilityChange?.(actor().id, !next);
      }
    };

    const commitRename = (raw: string) => {
      setRenamingId(null);
      const next = raw.trim();
      if (!next || next === actor().name) return;
      props.onActorRename?.(actor().id, next);
    };

    const computeZone = (e: DragEvent): DropMode | null => {
      const row = e.currentTarget as HTMLElement;
      const rect = row.getBoundingClientRect();
      const y = e.clientY - rect.top;
      const h = rect.height || 1;
      const allowInside = props.canDropInside?.(actor().id) !== false;
      if (allowInside) {
        if (y < h * 0.3) return 'before';
        if (y > h * 0.7) return 'after';
        return 'inside';
      }
      return y < h * 0.5 ? 'before' : 'after';
    };

    const onDragStart = (e: DragEvent) => {
      if (!props.onActorReorder) return;
      e.dataTransfer?.setData(DRAG_MIME, String(actor().id));
      e.dataTransfer!.effectAllowed = 'move';
      e.stopPropagation();
    };
    const onDragOver = (e: DragEvent) => {
      if (!props.onActorReorder) return;
      if (!e.dataTransfer?.types.includes(DRAG_MIME)) return;
      e.preventDefault();
      e.dataTransfer.dropEffect = 'move';
      setDrag({ id: actor().id, zone: computeZone(e) ?? 'after' });
    };
    const onDragLeave = (e: DragEvent) => {
      const row = e.currentTarget as HTMLElement;
      if (e.relatedTarget && row.contains(e.relatedTarget as Node)) return;
      setDrag((d) => (d?.id === actor().id ? null : d));
    };
    const onDrop = (e: DragEvent) => {
      if (!props.onActorReorder) return;
      e.preventDefault();
      const sourceIdStr = e.dataTransfer?.getData(DRAG_MIME);
      setDrag(null);
      if (!sourceIdStr) return;
      const sourceId = parseInt(sourceIdStr, 10);
      if (Number.isNaN(sourceId) || sourceId === actor().id) return;
      props.onActorReorder(sourceId, actor().id, computeZone(e) ?? 'after');
    };

    return (
      <div
        class="tree-item-row"
        classList={{
          'tree-item-row--selected': props.selectedActorId() === actor().id,
          'tree-item-row--hidden': !isVisible(),
          'tree-item-row--drop-before': dropZone() === 'before',
          'tree-item-row--drop-inside': dropZone() === 'inside',
          'tree-item-row--drop-after': dropZone() === 'after',
        }}
        ref={(el) => el.style.setProperty('--tree-depth', String(rp.row.depth))}
        draggable={true}
        onDragStart={onDragStart}
        onDragOver={onDragOver}
        onDragLeave={onDragLeave}
        onDrop={onDrop}
        onClick={() => props.onActorSelect(actor().id)}
      >
        <Show
          when={hasChildren()}
          fallback={<span class="tree-expand-placeholder" />}
        >
          <span
            class="tree-expand"
            onClick={(e) => {
              e.stopPropagation();
              toggleExpanded(actor().id);
            }}
          >
            {expanded() ? '▼' : '▶'}
          </span>
        </Show>
        <button
          type="button"
          class="tree-visibility"
          classList={{ 'tree-visibility--hidden': !isVisible() }}
          title={isVisible() ? 'Hide actor' : 'Show actor'}
          onClick={toggleVisibility}
        >
          {isVisible() ? '👁' : '⊘'}
        </button>
        <span class="tree-icon">{actor().light ? '💡' : '🎯'}</span>
        <Show
          when={renaming()}
          fallback={
            <span
              class="tree-name"
              title={props.onActorRename ? 'Double-click to rename' : undefined}
              onDblClick={(e) => {
                if (!props.onActorRename) return;
                e.stopPropagation();
                setRenamingId(actor().id);
              }}
            >
              {actor().name}
            </span>
          }
        >
          <input
            class="tree-rename-input"
            type="text"
            title="Rename actor"
            aria-label="Rename actor"
            value={actor().name}
            ref={(el) => {
              requestAnimationFrame(() => {
                el.focus();
                el.select();
              });
            }}
            onClick={(e) => e.stopPropagation()}
            onBlur={(e) => commitRename(e.currentTarget.value)}
            onKeyDown={(e) => {
              e.stopPropagation();
              if (e.key === 'Enter') commitRename(e.currentTarget.value);
              else if (e.key === 'Escape') setRenamingId(null);
            }}
          />
        </Show>
        <Show when={props.onActorDelete}>
          <button
            type="button"
            class="tree-delete"
            title="Delete actor"
            onClick={(e) => {
              e.stopPropagation();
              props.onActorDelete?.(actor().id);
            }}
          >
            🗑
          </button>
        </Show>
      </div>
    );
  };

  return (
    <div class="scene-hierarchy" tabIndex={0} onKeyDown={onKeyDown}>
      <div class="hierarchy-toolbar">
        <input
          class="hierarchy-search"
          type="search"
          placeholder="Filter…"
          title="Filter actors by name"
          value={search()}
          onInput={(e) => setSearch(e.currentTarget.value)}
          onKeyDown={(e) => {
            e.stopPropagation();
            if (e.key === 'Escape') {
              setSearch('');
              e.currentTarget.blur();
            }
          }}
        />
        <Show when={props.onLightAdd}>
          <div class="hierarchy-add-light">
            <button
              type="button"
              class="hierarchy-add-light-btn"
              title="Add a scene-level light (Dome / Sun / Point / Area)"
              onClick={() => setLightMenuOpen((v) => !v)}
            >
              + 💡 Add Light
            </button>
            <Show when={lightMenuOpen()}>
              <div
                class="hierarchy-add-light-menu"
                onMouseLeave={() => setLightMenuOpen(false)}
              >
                <For each={LIGHT_MENU}>
                  {(item) => (
                    <button
                      type="button"
                      class="hierarchy-add-light-menu-item"
                      onClick={() => {
                        setLightMenuOpen(false);
                        props.onLightAdd?.(item.kind);
                      }}
                    >
                      <span class="hierarchy-add-light-menu-icon">{item.icon}</span>
                      <span>{item.label}</span>
                    </button>
                  )}
                </For>
              </div>
            </Show>
          </div>
        </Show>
      </div>
      <Show
        when={!props.isLoading() && props.actors().length > 0}
        fallback={
          <div class="hierarchy-empty">
            {props.isLoading() ? 'Loading...' : 'No scene loaded'}
          </div>
        }
      >
        <Show
          when={flatRows().length > 0}
          fallback={<div class="hierarchy-empty">No actors match “{search()}”</div>}
        >
          <div
            class="hierarchy-tree"
            ref={scrollEl}
            onScroll={(e) => setScrollTop(e.currentTarget.scrollTop)}
          >
            <div class="hierarchy-sizer" ref={sizerEl}>
              <div class="hierarchy-window" ref={windowEl}>
                <For each={visibleRows()}>{(row) => <Row row={row} />}</For>
              </div>
            </div>
          </div>
        </Show>
      </Show>
    </div>
  );
};
