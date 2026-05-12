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

// Subset of ParamValue that can land in an input default slot. Strings
// aren't piped through input ports (no string-typed inputs in the v1 VOP
// set), so they're excluded by the wire encoder; bools could come later
// but no node currently exposes a bool input.
export type InputDefault =
  | { type: 'float'; value: number }
  | { type: 'int';   value: number }
  | { type: 'vec3';  value: [number, number, number] };

export interface VopNode {
  uid: number;
  kind: string;
  pos: [number, number];
  params: Record<string, ParamValue>;
  // Per-port input constants used when the input has no incoming wire.
  // Key is the input port index as a *string* (JSON-object convention on
  // the C++ side); value is a typed scalar/vec3. Missing key → fall back
  // to the node's built-in zero default.
  input_defaults?: Record<string, InputDefault>;
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

// `data_type` is the wire-format string for the port's runtime type
// (float / int / vec3 / vec2 / vec4 / bool / unknown). Drives the
// inspector's input-default widget choice. Missing when the catalog
// emitter couldn't probe — treat as 'unknown' and skip the editor.
export interface PortSpec   { name: string; data_type?: string }
// See lib/sop_graph.ts ParamRange / ParamSpec — same shape, same semantics.
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

// Port name lookups for canvas labels / tooltips. Mirrors the SOP-side
// helpers — empty string when the catalog hasn't loaded or the index is
// out of range so the canvas can omit the <text> cleanly.
export function inputPortName(kind: string, idx: number): string {
  return lookupCatalog(kind)?.inputs[idx]?.name ?? '';
}
export function outputPortName(kind: string, idx: number): string {
  return lookupCatalog(kind)?.outputs[idx]?.name ?? '';
}
// Wire-format data type for the input at `idx`. Used by the inspector
// to decide between a number / vec3 / etc. editor for unconnected
// inputs. Returns 'unknown' if the catalog hasn't loaded or the port
// has no data_type tag (older catalog payload).
export function inputPortDataType(kind: string, idx: number): string {
  return lookupCatalog(kind)?.inputs[idx]?.data_type ?? 'unknown';
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
