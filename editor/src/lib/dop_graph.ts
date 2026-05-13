// TypeScript mirror of the C++ DopGraph JSON schema
// (see src/dops/serialization.hpp). Same shape as VopGraph — re-uses
// parameter machinery from sop_graph.ts.

import { createSignal } from 'solid-js';
import * as api from './api';
import type { ParamType, ParamValue, Channels } from './sop_graph';
import { paramChannels, isAnimated } from './sop_graph';

export type { ParamType, ParamValue, Channels };
export { paramChannels, isAnimated };

// ── Wire types ────────────────────────────────────────────────────────────

export interface DopNode {
  uid: number;
  kind: string;
  pos: [number, number];
  params: Record<string, ParamValue>;
  // pop_force carries its embedded VopGraph in `extra.vop_graph`. The
  // store doesn't introspect this — it just round-trips. The VOP editor
  // opens against the pop_force uid via the shared host_uid plumbing.
  extra?: Record<string, unknown>;
}

export interface DopConnection {
  from_node: number;
  from_port: number;
  to_node: number;
  to_port: number;
}

export interface DopGraph {
  graph_kind: 'dop';
  version: 1;
  uid: number;
  next_uid?: number;
  nodes: DopNode[];
  connections: DopConnection[];
}

// ── Catalog ────────────────────────────────────────────────────────────────

export interface PortSpec   { name: string; data_type?: string }
export interface ParamRange { min: number; max: number; step: number }
export interface ParamSpec  {
  name: string;
  type: ParamType;
  default: string;
  range?: ParamRange;
  options?: string[];
}

export interface CatalogEntry {
  kind: string;
  label: string;
  category: string;
  inputs: PortSpec[];
  outputs: PortSpec[];
  params: ParamSpec[];
}

const [_catalog, _setCatalog] = createSignal<CatalogEntry[] | null>(null);

export async function fetchCatalog(): Promise<CatalogEntry[]> {
  const cached = _catalog();
  if (cached) return cached;
  const fetched = await api.send<CatalogEntry[]>('list_dop_node_catalog', {});
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
export function inputPortName(kind: string, idx: number): string {
  return lookupCatalog(kind)?.inputs[idx]?.name ?? '';
}
export function outputPortName(kind: string, idx: number): string {
  return lookupCatalog(kind)?.outputs[idx]?.name ?? '';
}

// ── Factories ──────────────────────────────────────────────────────────────

let _nextUid = 1;
export function allocNodeUid(): number { return _nextUid++; }
export function syncNextUid(uid: number) { _nextUid = Math.max(_nextUid, uid + 1); }

export function makeNode(kind: string, pos: [number, number]): DopNode | null {
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

export function emptyGraph(): DopGraph {
  return {
    graph_kind: 'dop',
    version: 1,
    uid: 0,
    next_uid: 1,
    nodes: [],
    connections: [],
  };
}
