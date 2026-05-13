// Thin wrapper around the shared NodePalette — supplies the SOP catalog
// + addNode wiring. The visual toolbar element is identical across SOP/
// VOP/DOP now.

import { Component } from 'solid-js';
import { NodePalette } from '../graph-canvas/NodePalette';
import { catalog, makeNode } from '../../lib/sop_graph';
import { addNode, setSelectedNode } from '../../stores/sops';

export const SopNodePalette: Component = () => {
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
      title="Add a SOP node"
    />
  );
};
