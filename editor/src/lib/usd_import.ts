// USD → SOP subnet import. Thin wrapper over the shared subnet builder
// (import_subnets.ts) — peek the stage, then build a `usd_import` subnet tree.
// Mirrors glTF import exactly; see import_subnets.ts for the shared logic.

import * as api from './api';
import { SopNode } from './sop_graph';
import { buildSubnetTree, ensureCatalog } from './import_subnets';

// Public entry point: peek the USD stage, then build one root-level subnet per
// mesh prim (flat first slice), each wired with a `usd_import` node. Returns
// the list so the caller can drop them into the currently-edited graph.
export async function buildSubnetsFromUsd(filePath: string): Promise<SopNode[]> {
  await ensureCatalog();
  const peek = await api.peekUsd(filePath);
  return buildSubnetTree(peek.roots, filePath, 'usd_import');
}
