import { Component, For, Show, createSignal } from 'solid-js';
import './SceneHierarchy.css';

export interface Actor {
  id: number;
  name: string;
  transform: {
    position: { x: number; y: number; z: number };
    rotation: { x: number; y: number; z: number; w: number };
    scale: { x: number; y: number; z: number };
  };
  children: number[];
  parent?: number | null;
}

interface TreeNode {
  actor: Actor;
  children: TreeNode[];
}

interface SceneHierarchyProps {
  actors: () => Actor[];
  selectedActorId: () => number | null;
  onActorSelect: (id: number) => void;
  isLoading: () => boolean;
  onAssetDropped?: (assetPath: string) => void;
  onActorRemove?: (id: number) => void;
  onActorReorder?: (actorId: number, parentId: number | null, newIndex: number) => void;
  onSetParent?: (actorId: number, newParentId: number | null) => void;
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

const DropZone: Component<{
  parentId: number | null;
  index: number;
  depth: number;
  onReorder?: (actorId: number, parentId: number | null, newIndex: number) => void;
}> = (props) => {
  const [isOver, setIsOver] = createSignal(false);

  function handleDragOver(e: DragEvent) {
    // Check if we're dragging an actor (getData only works in drop event)
    const types = e.dataTransfer?.types || [];
    if (types.includes('application/x-actor')) {
      e.preventDefault();
      e.stopPropagation();
      e.dataTransfer!.dropEffect = 'move';
      setIsOver(true);
    }
  }

  function handleDrop(e: DragEvent) {
    const actorData = e.dataTransfer!.getData('application/x-actor');
    if (actorData && props.onReorder) {
      e.preventDefault();
      e.stopPropagation();
      const actorId = parseInt(actorData);
      props.onReorder(actorId, props.parentId, props.index);
    }
    setIsOver(false);
  }

  return (
    <div
      class="drop-zone"
      classList={{ 'drop-zone--active': isOver() }}
      style={{ 'margin-left': `${props.depth * 16 + 8}px` }}
      onDragOver={handleDragOver}
      onDragLeave={() => setIsOver(false)}
      onDrop={handleDrop}
    />
  );
};

const TreeItem: Component<{
  node: TreeNode;
  depth: number;
  parentId: number | null;
  index: number;
  selectedId: () => number | null;
  onSelect: (id: number) => void;
  onRemove?: (id: number) => void;
  onReorder?: (actorId: number, parentId: number | null, newIndex: number) => void;
  onSetParent?: (actorId: number, newParentId: number | null) => void;
}> = (props) => {
  const [expanded, setExpanded] = createSignal(true);
  const [isParentDropTarget, setIsParentDropTarget] = createSignal(false);
  const hasChildren = () => props.node.children.length > 0;

  function handleDragStart(e: DragEvent) {
    e.dataTransfer!.setData('application/x-actor', props.node.actor.id.toString());
    e.dataTransfer!.effectAllowed = 'move';
  }

  function handleRowDragOver(e: DragEvent) {
    const types = e.dataTransfer?.types || [];
    if (types.includes('application/x-actor')) {
      e.preventDefault();
      e.stopPropagation();
      e.dataTransfer!.dropEffect = 'move';
      setIsParentDropTarget(true);
    }
  }

  function handleRowDrop(e: DragEvent) {
    const actorData = e.dataTransfer!.getData('application/x-actor');
    if (actorData) {
      e.preventDefault();
      e.stopPropagation();
      const draggedActorId = parseInt(actorData);

      // Don't allow dropping on itself or making it its own parent
      if (draggedActorId !== props.node.actor.id && props.onSetParent) {
        props.onSetParent(draggedActorId, props.node.actor.id);
      }
    }
    setIsParentDropTarget(false);
  }

  return (
    <>
      <DropZone
        parentId={props.parentId}
        index={props.index}
        depth={props.depth}
        onReorder={props.onReorder}
      />
      <div class="tree-item">
        <div
          class="tree-item-row"
          classList={{
            'tree-item-row--selected': props.selectedId() === props.node.actor.id,
            'tree-item-row--drop-target': isParentDropTarget(),
          }}
          style={{ 'padding-left': `${props.depth * 16 + 8}px` }}
          draggable={true}
          onDragStart={handleDragStart}
          onDragOver={handleRowDragOver}
          onDragLeave={() => setIsParentDropTarget(false)}
          onDrop={handleRowDrop}
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
          <span class="tree-icon">🎯</span>
          <span class="tree-name">{props.node.actor.name}</span>
          <Show when={props.node.actor.parent !== undefined && props.node.actor.parent !== null && props.onSetParent}>
            <button
              type="button"
              class="tree-unparent"
              onClick={(e) => {
                e.stopPropagation();
                props.onSetParent?.(props.node.actor.id, null);
              }}
              title="Remove from parent"
            >
              ↑
            </button>
          </Show>
          <Show when={props.onRemove}>
            <button
              type="button"
              class="tree-remove"
              onClick={(e) => {
                e.stopPropagation();
                props.onRemove?.(props.node.actor.id);
              }}
              title="Remove from scene"
            >
              ×
            </button>
          </Show>
        </div>
        <Show when={expanded() && hasChildren()}>
          <div class="tree-children">
            <For each={props.node.children}>
              {(child, idx) => (
                <TreeItem
                  node={child}
                  depth={props.depth + 1}
                  parentId={props.node.actor.id}
                  index={idx()}
                  selectedId={props.selectedId}
                  onSelect={props.onSelect}
                  onRemove={props.onRemove}
                  onReorder={props.onReorder}
                  onSetParent={props.onSetParent}
                />
              )}
            </For>
          </div>
        </Show>
      </div>
    </>
  );
};

export const SceneHierarchy: Component<SceneHierarchyProps> = (props) => {
  const tree = () => buildActorTree(props.actors());
  const [isDraggingOver, setIsDraggingOver] = createSignal(false);

  return (
    <div
      class="scene-hierarchy"
      onDragOver={(e) => {
        e.preventDefault();
        e.dataTransfer!.dropEffect = 'copy';
        setIsDraggingOver(true);
      }}
      onDragLeave={() => {
        setIsDraggingOver(false);
      }}
      onDrop={(e) => {
        e.preventDefault();
        setIsDraggingOver(false);
        const data = e.dataTransfer!.getData('application/x-asset');
        if (data && props.onAssetDropped) {
          const asset = JSON.parse(data);
          props.onAssetDropped(asset.path);
        }
      }}
    >
      <Show
        when={!props.isLoading() && props.actors().length > 0}
        fallback={
          <div
            class="hierarchy-empty"
            classList={{ 'drag-over': isDraggingOver() }}
            onDragOver={(e) => {
              e.preventDefault(); // Required to allow dropping
              e.dataTransfer!.dropEffect = 'copy';
            }}
          >
            {props.isLoading() ? 'Loading...' : 'Drag assets here to add to scene'}
          </div>
        }
      >
        <div class="hierarchy-tree">
          <For each={tree()}>
            {(node, idx) => (
              <TreeItem
                node={node}
                depth={0}
                parentId={null}
                index={idx()}
                selectedId={props.selectedActorId}
                onSelect={props.onActorSelect}
                onRemove={props.onActorRemove}
                onReorder={props.onActorReorder}
                onSetParent={props.onSetParent}
              />
            )}
          </For>
          <DropZone
            parentId={null}
            index={tree().length}
            depth={0}
            onReorder={props.onActorReorder}
          />
        </div>
      </Show>
    </div>
  );
};
