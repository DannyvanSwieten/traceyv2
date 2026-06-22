// glTF → SOP subnet import. Thin wrapper over the shared subnet builder
// (import_subnets.ts) — peek the file, then build a `gltf_import` subnet tree.
// See import_subnets.ts for the construction logic shared with USD import.

import * as api from './api';
import { SopNode } from './sop_graph';
import { buildSubnetTree, ensureCatalog } from './import_subnets';

// Public entry point: peek the glTF, then build one root-level subnet per
// top-level glTF node, each wired with `gltf_import` nodes. Returns the list so
// the caller can drop them into the currently-edited graph (today: addNode).
export async function buildSubnetsFromGltf(filePath: string): Promise<SopNode[]> {
  // The catalog must be loaded first — makeNode resolves 'gltf_import' /
  // 'object_output' through it and returns null when absent (silently
  // producing empty subnets). Importing before the SOP dock was ever opened
  // used to hit exactly that.
  await ensureCatalog();
  const peek = await api.peekGltf(filePath);
  return buildSubnetTree(peek.roots, filePath, 'gltf_import');
}
