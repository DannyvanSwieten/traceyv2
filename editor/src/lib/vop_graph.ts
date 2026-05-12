// TypeScript mirror of the C++ VopGraph JSON schema
// (see src/vops/serialization.hpp). Schema is identical to SopGraph's modulo
// `graph_kind: 'vop'` and the VOP-specific node kind set; we re-export the
// shared parameter types from sop_graph.ts so editor components can import
// either one interchangeably.

import { createSignal } from 'solid-js';
import * as api from './api';
import type {
  ParamType,
  ParamValue,
  Channels,
} from './sop_graph';
import { paramChannels, isAnimated } from './sop_graph';

// Shared parameter machinery — identical wire format to SOP params.
export type { ParamType, ParamValue, Channels };
export { paramChannels, isAnimated };

// ── Wire types ────────────────────────────────────────────────────────────

export interface VopNode {
  uid: number;
  kind: string;
  pos: [number, number];
  params: Record<string, ParamValue>;
  // VOPs do not nest in v1 (no `subgraph` field). When VOP-of-VOP composition
  // lands later, mirror the SOP `subgraph` pattern.
}

export interface VopConnection {
  from_node: number;
  from_port: number;
  to_node: number;
  to_port: number;
}

export interface VopGraph {
  graph_kind: 'vop';
  version: 1;
  uid: number;
  next_uid?: number;
  nodes: VopNode[];
  connections: VopConnection[];
}

// ── Catalog (fetched from the C++ side) ────────────────────────────────────

export interface PortSpec   { name: string }
export interface ParamSpec  { name: string; type: ParamType; default: string }

export interface CatalogEntry {
  kind: string;
  label: string;
  category: string;
  inputs: PortSpec[];
  outputs: PortSpec[];
  params: ParamSpec[];
}

// Solid signal so palette/canvas components rerender when fetchCatalog()
// resolves. A plain `let` here was the cause of "first open shows an empty
// palette, second open shows nodes" — Solid couldn't track the assignment.
const [_catalog, _setCatalog] = createSignal<CatalogEntry[] | null>(null);

export async function fetchCatalog(): Promise<CatalogEntry[]> {
  const cached = _catalog();
  if (cached) return cached;
  const fetched = await api.send<CatalogEntry[]>('list_vop_node_catalog', {});
  _setCatalog(fetched);
  return fetched;
}

export function catalog(): CatalogEntry[] {
  return _catalog() ?? [];
}

export function lookupCatalog(kind: string): CatalogEntry | undefined {
  return catalog().find((c) => c.kind === kind);
}

export function inputPortCount(kind: string): number {
  return lookupCatalog(kind)?.inputs.length ?? 0;
}
export function outputPortCount(kind: string): number {
  return lookupCatalog(kind)?.outputs.length ?? 1;
}

// ── Factories ──────────────────────────────────────────────────────────────

let _nextUid = 1;
export function allocNodeUid(): number { return _nextUid++; }
export function syncNextUid(uid: number) { _nextUid = Math.max(_nextUid, uid + 1); }

export function makeNode(kind: string, pos: [number, number]): VopNode | null {
  const entry = lookupCatalog(kind);
  if (!entry) return null;
  const params: Record<string, ParamValue> = {};
  for (const p of entry.params) {
    params[p.name] = parseDefault(p);
  }
  return { uid: allocNodeUid(), kind, pos, params };
}

function parseDefault(p: ParamSpec): ParamValue {
  const s = (p.default ?? '').trim();
  switch (p.type) {
    case 'float': return { type: 'float', value: parseFloat(s) || 0 };
    case 'int':   return { type: 'int',   value: parseInt(s, 10) || 0 };
    case 'bool':  return { type: 'bool',  value: s === 'true' };
    case 'vec3': {
      const m = s.match(/\[\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*\]/);
      if (m) return { type: 'vec3', value: [+m[1], +m[2], +m[3]] };
      return { type: 'vec3', value: [0, 0, 0] };
    }
    case 'string': {
      const v = s.startsWith('"') && s.endsWith('"') ? s.slice(1, -1) : s;
      return { type: 'string', value: v };
    }
  }
}

export function emptyGraph(): VopGraph {
  return {
    graph_kind: 'vop',
    version: 1,
    uid: 0,
    next_uid: 1,
    nodes: [],
    connections: [],
  };
}
