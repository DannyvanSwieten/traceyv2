// Build a SOP subgraph tree from an asset hierarchy peek (glTF or USD).
//
// Shared between the glTF and USD importers — both produce the SAME hierarchy
// shape (api.HierarchyNode) and the SAME procedural structure: one subnet per
// node mirroring the file's transform chain, with one `<kind>_import →
// object_output` chain per mesh entry inside mesh-bearing nodes. Only the
// import SOP kind differs (`gltf_import` vs `usd_import`), so it's a parameter.
//
// This is the engine of the "direct by default, procedural underneath" import
// UX: the user picks a file and it appears as objects; under the hood it's a
// re-cookable subnet tree they can open and extend.

import * as api from './api';
import {
  SopNode,
  SopGraph,
  ParamValueString,
  ParamValueVec3,
  allocNodeUid,
  emptyGraph,
  fetchCatalog,
  makeNode,
} from './sop_graph';

// Spacing for the auto-layout of new nodes. Rough — the user can rearrange.
const NODE_DX = 240;
const NODE_DY = 140;

function stringParam(value: string): ParamValueString {
  return { type: 'string', value };
}

function vec3Param(value: [number, number, number]): ParamValueVec3 {
  return { type: 'vec3', value };
}

// Build the inner SOP graph for one node. Mesh-bearing nodes get one
// `<importKind> → object_output` chain per mesh entry; child nodes recurse
// into nested subnets alongside.
function buildInnerGraph(
  n: api.HierarchyNode,
  filePath: string,
  importKind: string,
): SopGraph {
  const g = emptyGraph();

  let importX = 120;
  const importY = 120;
  for (const meshObjectName of n.mesh_names) {
    const importer = makeNode(importKind, [importX, importY]);
    const output = makeNode('object_output', [importX, importY + NODE_DY]);
    if (importer && output) {
      importer.params.path = stringParam(filePath);
      importer.params.mesh_name = stringParam(meshObjectName);
      // Display name reflects the node (+ a primitive suffix only when the
      // node expands into more than one mesh, keeping single-mesh cases clean).
      const suffix = n.mesh_names.length > 1
        ? `_${meshObjectName.split('_prim_').pop() ?? '0'}`
        : '';
      output.params.name = stringParam(n.name + suffix);
      g.nodes.push(importer, output);
      g.connections.push({
        from_node: importer.uid,
        from_port: 0,
        to_node: output.uid,
        to_port: 0,
      });
      importX += NODE_DX;
    }
  }

  // Child nodes — one subnet each, recursive. Laid out beneath the import
  // chain so the canvas reads top-down.
  let childX = 120;
  const childY = n.mesh_names.length > 0 ? importY + 2 * NODE_DY : importY;
  for (const child of n.children) {
    const childSubnet = buildSubnet(child, filePath, importKind);
    childSubnet.pos = [childX, childY];
    g.nodes.push(childSubnet);
    childX += NODE_DX;
  }

  return g;
}

function buildSubnet(
  n: api.HierarchyNode,
  filePath: string,
  importKind: string,
): SopNode {
  // Construct by hand (not via makeNode) so we can stamp a custom inner graph
  // in place of makeNode's empty-with-output seed.
  const subnet: SopNode = {
    uid: allocNodeUid(),
    kind: 'subnet',
    pos: [0, 0],
    params: {
      name: stringParam(n.name || `node_${allocNodeUid()}`),
      translate: vec3Param(n.translate),
      rotate_euler_deg: vec3Param(n.rotate_euler_deg),
      scale: vec3Param(n.scale),
    },
    subgraph: buildInnerGraph(n, filePath, importKind),
  };
  return subnet;
}

// Turn a peeked hierarchy into one root-level subnet per root node. The
// catalog must be loaded first — makeNode resolves kinds through it and
// returns null when absent (which would silently produce empty subnets).
export function buildSubnetTree(
  roots: api.HierarchyNode[],
  filePath: string,
  importKind: string,
): SopNode[] {
  return roots.map((r, i) => {
    const subnet = buildSubnet(r, filePath, importKind);
    // Lay roots out horizontally so a multi-root scene doesn't pile up.
    subnet.pos = [120 + i * NODE_DX, 120];
    return subnet;
  });
}

// Ensure the SOP catalog is loaded before building (so makeNode resolves the
// import + object_output kinds). Safe to call repeatedly.
export async function ensureCatalog(): Promise<void> {
  await fetchCatalog();
}
