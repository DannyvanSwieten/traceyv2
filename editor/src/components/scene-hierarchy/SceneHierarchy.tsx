import { Component, For, Show, createSignal } from 'solid-js';
import type { Actor as ApiActor } from '../../lib/api';
import './SceneHierarchy.css';

export type Actor = ApiActor;

interface TreeNode {
  actor: Actor;
  children: TreeNode[];
}

interface SceneHierarchyProps {
  actors: () => Actor[];
  selectedActorId: () => number | null;
  onActorSelect: (id: number) => void;
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

const TreeItem: Component<{
  node: TreeNode;
  depth: number;
  selectedId: () => number | null;
  onSelect: (id: number) => void;
}> = (props) => {
  const [expanded, setExpanded] = createSignal(true);
  const hasChildren = () => props.node.children.length > 0;

  return (
    <div class="tree-item">
      <div
        class="tree-item-row"
        classList={{
          'tree-item-row--selected': props.selectedId() === props.node.actor.id,
        }}
        style={{ 'padding-left': `${props.depth * 16 + 8}px` }}
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

  return (
    <div class="scene-hierarchy">
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
              />
            )}
          </For>
        </div>
      </Show>
    </div>
  );
};
