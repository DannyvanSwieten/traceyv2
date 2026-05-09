// TypeScript mirror of the C++ SopGraph JSON schema
// (see src/sops/serialization.hpp). The C++ side is authoritative; this
// file is typed convenience + a runtime-fetched catalog the palette/
// inspector consume.

import * as api from './api';

// ── Wire types ────────────────────────────────────────────────────────────

export type ParamType = 'float' | 'int' | 'bool' | 'vec3' | 'string';

// Param value shape on the wire. `value` is typed by the variant tag in
// `type` so a future codegen pass can emit C++ literals from this JSON
// directly.
export type ParamValueFloat  = { type: 'float';  value: number };
export type ParamValueInt    = { type: 'int';    value: number };
export type ParamValueBool   = { type: 'bool';   value: boolean };
export type ParamValueVec3   = { type: 'vec3';   value: [number, number, number] };
export type ParamValueString = { type: 'string'; value: string };
export type ParamValue =
  | ParamValueFloat
  | ParamValueInt
  | ParamValueBool
  | ParamValueVec3
  | ParamValueString;

export interface SopNode {
  uid: number;
  kind: string;
  pos: [number, number];  // graph editor canvas position
  params: Record<string, ParamValue>;
}

export interface SopConnection {
  from_node: number;
  from_port: number;
  to_node: number;
  to_port: number;
}

export interface SopGraph {
  graph_kind: 'sop';
  version: 1;
  uid: number;
  next_uid?: number;
  nodes: SopNode[];
  connections: SopConnection[];
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

let _catalog: CatalogEntry[] | null = null;

export async function fetchCatalog(): Promise<CatalogEntry[]> {
  if (_catalog) return _catalog;
  _catalog = await api.send<CatalogEntry[]>('list_sop_node_catalog', {});
  return _catalog;
}

export function catalog(): CatalogEntry[] {
  return _catalog ?? [];
}

export function lookupCatalog(kind: string): CatalogEntry | undefined {
  return catalog().find((c) => c.kind === kind);
}

// Port-count introspection for the canvas. Falls back to (0, 1) if the catalog
// hasn't loaded yet — generators are the most common shape, so the placeholder
// renders sensibly during the first frame after mount.
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

// Build a fresh node with parameters pre-populated from the catalog defaults.
// Returns null if the kind isn't registered (palette typically prevents that).
export function makeNode(kind: string, pos: [number, number]): SopNode | null {
  const entry = lookupCatalog(kind);
  if (!entry) return null;
  const params: Record<string, ParamValue> = {};
  for (const p of entry.params) {
    params[p.name] = parseDefault(p);
  }
  return { uid: allocNodeUid(), kind, pos, params };
}

// Parse the catalog's `default` string into a typed ParamValue.
// Defaults ship as quoted-or-bracketed C-style literals to keep the
// catalog C++ side simple ("\"actor\"", "[0, 0, 0]", "1.0", "true").
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
      // Strip surrounding quotes if present.
      const v = s.startsWith('"') && s.endsWith('"') ? s.slice(1, -1) : s;
      return { type: 'string', value: v };
    }
  }
}

export function emptyGraph(): SopGraph {
  return {
    graph_kind: 'sop',
    version: 1,
    uid: 0,
    next_uid: 1,
    nodes: [],
    connections: [],
  };
}
