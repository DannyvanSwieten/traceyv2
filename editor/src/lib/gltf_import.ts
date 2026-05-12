// Build a SOP subgraph tree from a glTF hierarchy peek.
//
// One subnet per glTF node, mirroring the file's transform chain. Mesh-bearing
// nodes get a `gltf_import` (with the file path + `mesh_name` pointing at the
// SceneObject) wired into an `object_output` inside their subnet. The whole
// tree is constructed bottom-up so it can be inserted with a single
// `addNode(root)` call — `set_sop_graph` then ships it back to the host as
// one nested JSON payload and the cook turns it into a parented actor tree.

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

// Spacing for the auto-layout of new nodes. The values are rough — the user
// can rearrange after import.
const NODE_DX = 240;
const NODE_DY = 140;

function stringParam(value: string): ParamValueString {
  return { type: 'string', value };
}

function vec3Param(value: [number, number, number]): ParamValueVec3 {
  return { type: 'vec3', value };
}

// Build the inner SOP graph for one glTF node. If the node has a mesh, the
// inner graph contains a `gltf_import` → `object_output` chain; child glTF
// nodes recursively become nested subnets sitting alongside.
function buildInnerGraph(
  n: api.GltfHierarchyNode,
  filePath: string,
): SopGraph {
  const g = emptyGraph();

  // Mesh-bearing node: one `gltf_import → object_output` chain per
  // primitive in the referenced mesh. Multi-primitive meshes (common when
  // artists pack sub-materials into one mesh) used to silently lose all
  // but the first primitive — now each becomes its own actor under this
  // subnet, named `<node>_<primName>` so the hierarchy stays readable.
  let importX = 120;
  const importY = 120;
  for (const meshObjectName of n.mesh_names) {
    const importer = makeNode('gltf_import', [importX, importY]);
    const output = makeNode('object_output', [importX, importY + NODE_DY]);
    if (importer && output) {
      importer.params.path = stringParam(filePath);
      importer.params.mesh_name = stringParam(meshObjectName);
      // Make the actor's display name in the scene hierarchy reflect both
      // the node and the primitive (only the primitive suffix is added
      // when there's more than one — keeps single-primitive cases clean).
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

  // Child glTF nodes — one subnet each, recursive. Lay them out in a row
  // beneath the import/output chain so the canvas reads top-down.
  let childX = 120;
  const childY = n.mesh_names.length > 0 ? importY + 2 * NODE_DY : importY;
  for (const child of n.children) {
    const childSubnet = buildSubnet(child, filePath);
    childSubnet.pos = [childX, childY];
    g.nodes.push(childSubnet);
    childX += NODE_DX;
  }

  return g;
}

function buildSubnet(n: api.GltfHierarchyNode, filePath: string): SopNode {
  // Construct the subnet by hand rather than via makeNode so we can stamp
  // a custom inner graph in place of makeNode's empty-with-output seed.
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
    subgraph: buildInnerGraph(n, filePath),
  };
  return subnet;
}

// Public entry point: peek the glTF, then build one root-level subnet per
// top-level glTF node. Returns the list so the caller can decide where to
// drop them (today: addNode for each, into the currently-edited graph).
export async function buildSubnetsFromGltf(filePath: string): Promise<SopNode[]> {
  // The SOP catalog has to be loaded first — `makeNode(kind, …)` resolves the
  // kind through the catalog and returns null when it isn't there yet. If a
  // user imports a glTF *before* opening the SOP dock for the first time
  // (the only place fetchCatalog was previously called from), every
  // `makeNode('gltf_import' | 'object_output', …)` call here would silently
  // return null, the chain wouldn't get pushed into the subnet's inner
  // graph, and the deepest subnet would end up empty — exactly the "bottom
  // level network is empty" symptom on the mandarinorange glTF.
  await fetchCatalog();
  const peek = await api.peekGltf(filePath);
  return peek.roots.map((r, i) => {
    const subnet = buildSubnet(r, filePath);
    // Lay roots out horizontally so a multi-root scene doesn't pile up at
    // the origin.
    subnet.pos = [120 + i * NODE_DX, 120];
    return subnet;
  });
}
