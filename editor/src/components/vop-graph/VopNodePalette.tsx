// Thin wrapper around the shared NodePalette — supplies the VOP catalog
// + addNode wiring.

import { Component } from 'solid-js';
import { NodePalette } from '../graph-canvas/NodePalette';
import { catalog, makeNode } from '../../lib/vop_graph';
import { addNode, setSelectedNode } from '../../stores/vops';

export const VopNodePalette: Component = () => {
  function spawn(kind: string) {
    const jitter = (): [number, number] => [
      40 + Math.random() * 80,
      40 + Math.random() * 80,
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
      title="Add a VOP node"
    />
  );
};
