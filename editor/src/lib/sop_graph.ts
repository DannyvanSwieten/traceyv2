// TypeScript mirror of the C++ SopGraph JSON schema
// (see src/sops/serialization.hpp). The C++ side is authoritative; this
// file is typed convenience + a runtime-fetched catalog the palette/
// inspector consume.

import { createSignal } from 'solid-js';
import * as api from './api';

// ── Wire types ────────────────────────────────────────────────────────────

export type ParamType = 'float' | 'int' | 'bool' | 'vec3' | 'string';

// Param value shape on the wire. `value` is typed by the variant tag in
// `type` so a future codegen pass can emit C++ literals from this JSON
// directly.
//
// Animated parameters get an optional `channels` array (per-component for
// vec3, length 1 for scalar params). Editing the constant `value` doesn't
// touch `channels`, so round-tripping a graph through this store preserves
// keyframes set via the timeline IPC.
export type Interp = 'step' | 'linear' | 'bezier';
export type Extrap = 'hold' | 'cycle' | 'linear';
export interface Keyframe {
  t:   number;  // seconds
  v:   number;
  in:  number;  // tangent (value/sec)
  out: number;
  i:   Interp;
}
export interface Channel {
  keys: Keyframe[];
  pre:  Extrap;
  post: Extrap;
}
// `channels[i]` may be null for components that aren't animated.
export type Channels = (Channel | null)[];

interface AnimatableParamFields { channels?: Channels }
export type ParamValueFloat  = { type: 'float';  value: number } & AnimatableParamFields;
export type ParamValueInt    = { type: 'int';    value: number } & AnimatableParamFields;
export type ParamValueBool   = { type: 'bool';   value: boolean } & AnimatableParamFields;
export type ParamValueVec3   = { type: 'vec3';   value: [number, number, number] } & AnimatableParamFields;
export type ParamValueString = { type: 'string'; value: string };
export type ParamValue =
  | ParamValueFloat
  | ParamValueInt
  | ParamValueBool
  | ParamValueVec3
  | ParamValueString;

// Returns the channels block on a param if it's animatable, else undefined.
export function paramChannels(p: ParamValue): Channels | undefined {
  return p.type !== 'string' ? p.channels : undefined;
}

// True iff at least one component channel has at least one keyframe.
export function isAnimated(p: ParamValue): boolean {
  const ch = paramChannels(p);
  if (!ch) return false;
  return ch.some((c) => c && c.keys.length > 0);
}

export interface SopNode {
  uid: number;
  kind: string;
  pos: [number, number];  // graph editor canvas position
  params: Record<string, ParamValue>;
  // Houdini-style /obj subnet nodes carry a nested SopGraph here. The
  // backend's deserializer recognises the field on any node kind that
  // overrides setInnerGraph (today only "subnet"). The frontend treats
  // subgraph-bearing nodes as containers — double-click enters them, the
  // current-path nav resolves through them, etc.
  subgraph?: SopGraph;
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

// Solid signal so palette/canvas components rerender when fetchCatalog()
// resolves. A plain `let` here was the cause of "first open shows an empty
// palette, second open shows nodes" — Solid couldn't track the assignment.
const [_catalog, _setCatalog] = createSignal<CatalogEntry[] | null>(null);

export async function fetchCatalog(): Promise<CatalogEntry[]> {
  const cached = _catalog();
  if (cached) return cached;
  const fetched = await api.send<CatalogEntry[]>('list_sop_node_catalog', {});
  _setCatalog(fetched);
  return fetched;
}

export function catalog(): CatalogEntry[] {
  return _catalog() ?? [];
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

// Walk a graph (recursively into subgraphs) and seed the uid allocator past
// every uid present anywhere in the tree. Called after loading a graph from
// the backend so locally-allocated uids never collide with nested ones.
export function syncNextUidRecursive(g: SopGraph): void {
  for (const n of g.nodes) {
    syncNextUid(n.uid);
    if (n.subgraph) syncNextUidRecursive(n.subgraph);
  }
}

// Find a node by uid anywhere in the (possibly nested) graph. Returns null
// when no node matches. Uids are globally unique across nesting (the
// backend's SopGraph::nextUid forwards to the root allocator), so a single
// uid identifies a node unambiguously.
export function findNodeRecursive(g: SopGraph, uid: number): SopNode | null {
  for (const n of g.nodes) {
    if (n.uid === uid) return n;
    if (n.subgraph) {
      const hit = findNodeRecursive(n.subgraph, uid);
      if (hit) return hit;
    }
  }
  return null;
}

// Build a fresh node with parameters pre-populated from the catalog defaults.
// Returns null if the kind isn't registered (palette typically prevents that).
export function makeNode(kind: string, pos: [number, number]): SopNode | null {
  const entry = lookupCatalog(kind);
  if (!entry) return null;
  const params: Record<string, ParamValue> = {};
  for (const p of entry.params) {
    params[p.name] = parseDefault(p);
  }
  const node: SopNode = { uid: allocNodeUid(), kind, pos, params };
  // Subnets ship with a default object_output inside so the user can dive
  // straight in and see an actor in the scene without an extra "add output"
  // step. Other node kinds get no subgraph; the backend's setInnerGraph
  // virtual is a no-op for them.
  if (kind === 'subnet') {
    node.subgraph = emptyGraphWithOutput();
  }
  return node;
}

// Build a fresh graph seeded with a single `object_output` node so every
// network ships ready to render. Falls back to a truly empty graph if the
// catalog hasn't loaded yet (callers that need the output should run after
// fetchCatalog()).
export function emptyGraphWithOutput(): SopGraph {
  const g = emptyGraph();
  const out = makeNode('object_output', [120, 120]);
  if (out) g.nodes.push(out);
  return g;
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
