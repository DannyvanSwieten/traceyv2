import { Component, For, Show, createSignal } from 'solid-js';
import * as api from '../../lib/api';
import type { Actor as ApiActor } from '../../lib/api';
import './SceneHierarchy.css';

export type Actor = ApiActor;

interface TreeNode {
  actor: Actor;
  children: TreeNode[];
}

export type DropMode = 'before' | 'inside' | 'after';

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
  // Returns true if the target actor's source SOP node is a subnet (or
  // otherwise accepts children). Used to suppress the "drop INSIDE" zone on
  // rows that can't take a child.
  canDropInside?: (targetId: number) => boolean;
  isLoading: () => boolean;
}

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

interface TreeItemProps {
  node: TreeNode;
  depth: number;
  selectedId: () => number | null;
  onSelect: (id: number) => void;
  onVisibilityChange?: (actorId: number, visible: boolean) => void;
  onDelete?: (actorId: number) => void;
  onReorder?: (sourceId: number, targetId: number, mode: DropMode) => void;
  canDropInside?: (targetId: number) => boolean;
}

const TreeItem: Component<TreeItemProps> = (props) => {
  const [expanded, setExpanded] = createSignal(true);
  const [dropZone, setDropZone] = createSignal<DropMode | null>(null);
  const hasChildren = () => props.node.children.length > 0;
  // Default to visible when the server omits the field (older serialisations).
  const isVisible = () => props.node.actor.visible !== false;

  const toggleVisibility = async (e: MouseEvent) => {
    e.stopPropagation();
    const next = !isVisible();
    // Optimistic update so the icon flips immediately.
    props.onVisibilityChange?.(props.node.actor.id, next);
    try {
      await api.setActorVisible(props.node.actor.id, next);
    } catch (err) {
      // Revert if the server rejected the toggle.
      console.warn('setActorVisible failed:', err);
      props.onVisibilityChange?.(props.node.actor.id, !next);
    }
  };

  const handleDelete = (e: MouseEvent) => {
    e.stopPropagation();
    props.onDelete?.(props.node.actor.id);
  };

  // ── Drag-and-drop wiring ──────────────────────────────────────────────
  const onDragStart = (e: DragEvent) => {
    if (!props.onReorder) return;
    e.dataTransfer?.setData(DRAG_MIME, String(props.node.actor.id));
    e.dataTransfer!.effectAllowed = 'move';
    e.stopPropagation();
  };

  // Map the pointer's Y inside the row to one of three drop zones. The
  // middle ("inside") band is only allowed when the target row can host
  // children — otherwise the whole row splits into before/after halves.
  const computeZone = (e: DragEvent): DropMode | null => {
    const row = e.currentTarget as HTMLElement;
    const rect = row.getBoundingClientRect();
    const y = e.clientY - rect.top;
    const h = rect.height || 1;
    const allowInside =
      props.canDropInside?.(props.node.actor.id) !== false;
    if (allowInside) {
      if (y < h * 0.3) return 'before';
      if (y > h * 0.7) return 'after';
      return 'inside';
    }
    return y < h * 0.5 ? 'before' : 'after';
  };

  const onDragOver = (e: DragEvent) => {
    if (!props.onReorder) return;
    // Accept the drop only when our payload type is present. Without this,
    // page-level drag sources (files, etc.) would light up the row.
    if (!e.dataTransfer?.types.includes(DRAG_MIME)) return;
    e.preventDefault();
    e.dataTransfer.dropEffect = 'move';
    setDropZone(computeZone(e));
  };

  const onDragLeave = (e: DragEvent) => {
    // Pointer can briefly cross between child elements inside the row;
    // ignore those phantom leaves by checking relatedTarget containment.
    const row = e.currentTarget as HTMLElement;
    if (e.relatedTarget && row.contains(e.relatedTarget as Node)) return;
    setDropZone(null);
  };

  const onDrop = (e: DragEvent) => {
    if (!props.onReorder) return;
    e.preventDefault();
    const sourceIdStr = e.dataTransfer?.getData(DRAG_MIME);
    setDropZone(null);
    if (!sourceIdStr) return;
    const sourceId = parseInt(sourceIdStr, 10);
    if (Number.isNaN(sourceId) || sourceId === props.node.actor.id) return;
    const zone = computeZone(e) ?? 'after';
    props.onReorder(sourceId, props.node.actor.id, zone);
  };

  return (
    <div class="tree-item">
      <div
        class="tree-item-row"
        classList={{
          'tree-item-row--selected': props.selectedId() === props.node.actor.id,
          'tree-item-row--hidden': !isVisible(),
          'tree-item-row--drop-before': dropZone() === 'before',
          'tree-item-row--drop-inside': dropZone() === 'inside',
          'tree-item-row--drop-after': dropZone() === 'after',
        }}
        ref={(el) => el.style.setProperty('--tree-depth', String(props.depth))}
        draggable={true}
        onDragStart={onDragStart}
        onDragOver={onDragOver}
        onDragLeave={onDragLeave}
        onDrop={onDrop}
        onClick={() => props.onSelect(props.node.actor.id)}
      >
        <Show when={hasChildren()}>
          <span
            class="tree-expand"
            onClick={(e) => {
              e.stopPropagation();
              setExpanded(!expanded());
            }}
          >
            {expanded() ? '▼' : '▶'}
          </span>
        </Show>
        <Show when={!hasChildren()}>
          <span class="tree-expand-placeholder" />
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
        <span class="tree-icon">
          {props.node.actor.light ? '💡' : '🎯'}
        </span>
        <span class="tree-name">{props.node.actor.name}</span>
        <Show when={props.onDelete}>
          <button
            type="button"
            class="tree-delete"
            title="Delete actor"
            onClick={handleDelete}
          >
            🗑
          </button>
        </Show>
      </div>
      <Show when={expanded() && hasChildren()}>
        <div class="tree-children">
          <For each={props.node.children}>
            {(child) => (
              <TreeItem
                node={child}
                depth={props.depth + 1}
                selectedId={props.selectedId}
                onSelect={props.onSelect}
                onVisibilityChange={props.onVisibilityChange}
                onDelete={props.onDelete}
                onReorder={props.onReorder}
                canDropInside={props.canDropInside}
              />
            )}
          </For>
        </div>
      </Show>
    </div>
  );
};

export const SceneHierarchy: Component<SceneHierarchyProps> = (props) => {
  const tree = () => buildActorTree(props.actors());

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

  return (
    <div class="scene-hierarchy" tabIndex={0} onKeyDown={onKeyDown}>
      <Show
        when={!props.isLoading() && props.actors().length > 0}
        fallback={
          <div class="hierarchy-empty">
            {props.isLoading() ? 'Loading...' : 'No scene loaded'}
          </div>
        }
      >
        <div class="hierarchy-tree">
          <For each={tree()}>
            {(node) => (
              <TreeItem
                node={node}
                depth={0}
                selectedId={props.selectedActorId}
                onSelect={props.onActorSelect}
                onVisibilityChange={props.onActorVisibilityChange}
                onDelete={props.onActorDelete}
                onReorder={props.onActorReorder}
                canDropInside={props.canDropInside}
              />
            )}
          </For>
        </div>
      </Show>
    </div>
  );
};
