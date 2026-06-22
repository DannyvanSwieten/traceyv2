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
  Channels,
  Interp,
  Extrap,
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

// Build an animated vec3 param: a per-component keyframe channel sampled from
// the USD time samples. Keys are in seconds (USD timeCode / fps). The constant
// `value` stays as the first-sample fallback. Linear interp, hold extrap —
// matches what the engine bakes for an unedited imported channel. These ride
// into the native graph via set_sop_graph (which deserializes channels), so no
// per-key IPC is needed.
function vec3ParamAnimated(
  staticValue: [number, number, number],
  samples: api.TrsSample[],
  field: 'translate' | 'rotate_euler_deg' | 'scale',
  fps: number,
): ParamValueVec3 {
  const channels: Channels = [0, 1, 2].map((axis) => ({
    keys: samples.map((s) => ({
      t: s.t / fps,
      v: s[field][axis],
      in: 0,
      out: 0,
      i: 'linear' as Interp,
    })),
    pre: 'hold' as Extrap,
    post: 'hold' as Extrap,
  }));
  return { type: 'vec3', value: staticValue, channels };
}

// Build the inner SOP graph for one node. Mesh-bearing nodes get one
// `<importKind> → object_output` chain per mesh entry; child nodes recurse
// into nested subnets alongside.
function buildInnerGraph(
  n: api.HierarchyNode,
  filePath: string,
  importKind: string,
  fps: number,
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
    const childSubnet = buildSubnet(child, filePath, importKind, fps);
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
  fps: number,
): SopNode {
  // Animated prim → bake keyframe channels onto the subnet's TRS params, so
  // the imported actor animates exactly as authored in USD. Static prims keep
  // plain constant params. fps comes from the stage's timeCodesPerSecond.
  const samples = n.trs_samples;
  const animated = !!samples && samples.length > 0 && fps > 0;
  const tParam = animated
    ? vec3ParamAnimated(n.translate, samples!, 'translate', fps)
    : vec3Param(n.translate);
  const rParam = animated
    ? vec3ParamAnimated(n.rotate_euler_deg, samples!, 'rotate_euler_deg', fps)
    : vec3Param(n.rotate_euler_deg);
  const sParam = animated
    ? vec3ParamAnimated(n.scale, samples!, 'scale', fps)
    : vec3Param(n.scale);

  // Construct by hand (not via makeNode) so we can stamp a custom inner graph
  // in place of makeNode's empty-with-output seed.
  const subnet: SopNode = {
    uid: allocNodeUid(),
    kind: 'subnet',
    pos: [0, 0],
    params: {
      name: stringParam(n.name || `node_${allocNodeUid()}`),
      translate: tParam,
      rotate_euler_deg: rParam,
      scale: sParam,
    },
    subgraph: buildInnerGraph(n, filePath, importKind, fps),
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
  fps = 0,
  groupName = '',
): SopNode[] {
  const subnets = roots.map((r, i) => {
    const subnet = buildSubnet(r, filePath, importKind, fps);
    // Lay roots out horizontally so a multi-root scene doesn't pile up.
    subnet.pos = [120 + i * NODE_DX, 120];
    return subnet;
  });

  // Ungrouped: drop the subnets at the graph root as-is.
  if (!groupName) return subnets;

  // Grouped: nest every imported subnet inside ONE outer "group" subnet so the
  // whole asset is a single outliner entry — toggling its visibility hides the
  // entire import (visibility cascades to children at compile time) and
  // deleting the group removes every imported actor in one operation. The
  // group itself is a transform-only marker (identity TRS, no geometry).
  const inner = emptyGraph();
  inner.nodes.push(...subnets);
  const group: SopNode = {
    uid: allocNodeUid(),
    kind: 'subnet',
    pos: [120, 120],
    params: {
      name: stringParam(groupName || 'imported'),
      translate: vec3Param([0, 0, 0]),
      rotate_euler_deg: vec3Param([0, 0, 0]),
      scale: vec3Param([1, 1, 1]),
    },
    subgraph: inner,
  };
  return [group];
}

// Derive a tidy group name from a file path: basename without extension.
export function groupNameFromPath(filePath: string): string {
  const base = filePath.split(/[/\\]/).pop() ?? filePath;
  const dot = base.lastIndexOf('.');
  return dot > 0 ? base.slice(0, dot) : base;
}

// Ensure the SOP catalog is loaded before building (so makeNode resolves the
// import + object_output kinds). Safe to call repeatedly.
export async function ensureCatalog(): Promise<void> {
  await fetchCatalog();
}
