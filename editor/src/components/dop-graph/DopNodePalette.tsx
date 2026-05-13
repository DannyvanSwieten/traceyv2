// Thin wrapper around the shared NodePalette — supplies the DOP catalog
// and the addNode/select wiring. Replaces the previous standalone
// dropdown; identical UX, less code to maintain.

import { Component } from 'solid-js';
import { NodePalette } from '../graph-canvas/NodePalette';
import { catalog, makeNode } from '../../lib/dop_graph';
import { addNode, setSelectedNode } from '../../stores/dops';

export const DopNodePalette: Component = () => {
  function spawn(kind: string) {
    // Random jitter near the top-left so multiple adds don't stack
    // exactly on top of each other.
    const jitter = (): [number, number] => [
      60 + Math.random() * 80,
      60 + Math.random() * 60,
    ];
    const node = makeNode(kind, jitter());
    if (!node) return;
    addNode(node);
    setSelectedNode(node.uid);
  }
  return (
    <NodePalette
      catalog={catalog}
      onSpawn={spawn}
      title="Add a DOP node"
    />
  );
};
