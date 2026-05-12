// TypeScript mirror of the C++ ShaderGraph JSON schema (see
// src/graph/graphs/shader_graph/serialization.hpp). The wire format is
// authoritative; this file is just typed convenience for the editor.
//
// One extension over the C++ schema: each node carries an optional
// `position` array for canvas layout. C++ deserializes it as an unknown
// field (ignored) and the engine echoes it back via get_material_graph,
// so editor metadata survives the round-trip.

export type NodeKind =
  | 'Constant'
  | 'Parameter'
  | 'SurfaceAttribute'
  | 'InputAttribute'
  | 'BinaryOp'
  | 'UnaryOp'
  | 'TernaryOp'
  | 'Output';

export interface Vec4Tuple extends Array<number> {
  0: number; 1: number; 2: number; 3: number; length: 4;
}

export interface PositionTuple extends Array<number> {
  0: number; 1: number; length: 2;
}

export interface BaseNode {
  uid: number;
  kind: NodeKind;
  position?: PositionTuple;
  // Per-input vec4 default — used by the compiler when an input has no
  // wire. Key is the input port index as a string (JSON-object key
  // convention from the C++ side); value is a 4-tuple. Missing key →
  // compiler errors as before, so the inspector only ever writes here
  // when the user types into the per-input editor.
  input_defaults?: Record<string, Vec4Tuple>;
}

export interface ConstantNode extends BaseNode {
  kind: 'Constant';
  value: Vec4Tuple;
}

export interface ParameterNode extends BaseNode {
  kind: 'Parameter';
  name: string;
  default: Vec4Tuple;
}

// All op-bearing kinds share the same shape: a single `op` field with an
// enumerator name (e.g. "Add", "WriteAlbedo", "LoadInputAlbedo").
export interface SurfaceAttributeNode extends BaseNode {
  kind: 'SurfaceAttribute';
  op: string;
}
export interface InputAttributeNode extends BaseNode {
  kind: 'InputAttribute';
  op: string;
}
export interface BinaryOpNode extends BaseNode {
  kind: 'BinaryOp';
  op: string;
}
export interface UnaryOpNode extends BaseNode {
  kind: 'UnaryOp';
  op: string;
}
export interface TernaryOpNode extends BaseNode {
  kind: 'TernaryOp';
  op: string;
}
export interface OutputNode extends BaseNode {
  kind: 'Output';
  op: string;
}

export type Node =
  | ConstantNode | ParameterNode
  | SurfaceAttributeNode | InputAttributeNode
  | BinaryOpNode | UnaryOpNode | TernaryOpNode
  | OutputNode;

export interface Connection {
  from_node: number;
  from_port: number;
  to_node: number;
  to_port: number;
}

export interface ShaderGraph {
  version: 1;
  uid: number;
  nodes: Node[];
  connections: Connection[];
}

// ── Port introspection ────────────────────────────────────────────────────
// How many input ports does a node of this kind have? Mirrors the C++ side
// (nodes.hpp). Output ports: 0 for Output, 1 for everything else.

export function inputPortCount(kind: NodeKind): number {
  switch (kind) {
    case 'Constant':         return 0;
    case 'Parameter':        return 0;
    case 'SurfaceAttribute': return 0;
    case 'InputAttribute':   return 0;
    case 'BinaryOp':         return 2;
    case 'UnaryOp':          return 1;
    case 'TernaryOp':        return 3;
    case 'Output':           return 1;
  }
}

export function outputPortCount(kind: NodeKind): number {
  return kind === 'Output' ? 0 : 1;
}

// Input port name per kind/index. Mirrors the names returned by the
// C++ ports() implementations (see src/graph/graphs/shader_graph/nodes.hpp).
// Used by the inspector to label per-input default editors.
export function inputPortName(kind: NodeKind, idx: number): string {
  switch (kind) {
    case 'BinaryOp':  return ['a', 'b'][idx] ?? '';
    case 'UnaryOp':   return ['a'][idx] ?? '';
    case 'TernaryOp': return ['a', 'b', 'c'][idx] ?? '';
    case 'Output':    return ['value'][idx] ?? '';
    default:          return '';
  }
}

// ── Node factories with sensible defaults ────────────────────────────────

let _nextUid = 1000;
function nextUid(): number {
  return _nextUid++;
}
// Public allocator used by paste/duplicate helpers that need to mint a
// fresh uid for a cloned node without going through one of the factory
// helpers (which want to construct a full default node from scratch).
export function allocNodeUid(): number { return nextUid(); }

export function makeConstantNode(value: Vec4Tuple = [1, 1, 1, 1], position?: PositionTuple): ConstantNode {
  return { uid: nextUid(), kind: 'Constant', value, position };
}
export function makeParameterNode(name: string, def: Vec4Tuple = [0.5, 0.5, 0.5, 1], position?: PositionTuple): ParameterNode {
  return { uid: nextUid(), kind: 'Parameter', name, default: def, position };
}
export function makeOpNode<K extends 'SurfaceAttribute' | 'InputAttribute' | 'BinaryOp' | 'UnaryOp' | 'TernaryOp' | 'Output'>(
  kind: K, op: string, position?: PositionTuple
): Node {
  return { uid: nextUid(), kind, op, position } as Node;
}

// Keep client uid generator in sync after loading a graph from the engine.
export function reseedUidsFrom(graph: ShaderGraph): void {
  let max = 0;
  for (const n of graph.nodes) {
    if (n.uid > max) max = n.uid;
  }
  if (max + 1 > _nextUid) _nextUid = max + 1;
}

// ── Palette: the node kinds + ops the user can add ────────────────────────

export interface PaletteEntry {
  label: string;
  factory: (pos?: PositionTuple) => Node;
}

export const PALETTE: { group: string; entries: PaletteEntry[] }[] = [
  {
    group: 'Inputs',
    entries: [
      { label: 'Albedo (input)',    factory: (p) => makeOpNode('InputAttribute', 'LoadInputAlbedo',    p) },
      { label: 'Metallic (input)',  factory: (p) => makeOpNode('InputAttribute', 'LoadInputMetallic',  p) },
      { label: 'Roughness (input)', factory: (p) => makeOpNode('InputAttribute', 'LoadInputRoughness', p) },
      { label: 'Emission (input)',  factory: (p) => makeOpNode('InputAttribute', 'LoadInputEmission',  p) },
      { label: 'Normal (input)',    factory: (p) => makeOpNode('InputAttribute', 'LoadInputNormal',    p) },
    ],
  },
  {
    group: 'Surface',
    entries: [
      { label: 'World Position', factory: (p) => makeOpNode('SurfaceAttribute', 'LoadPosition',      p) },
      { label: 'World Normal',   factory: (p) => makeOpNode('SurfaceAttribute', 'LoadNormal',        p) },
      { label: 'View Direction', factory: (p) => makeOpNode('SurfaceAttribute', 'LoadViewDir',       p) },
      { label: 'UV0',            factory: (p) => makeOpNode('SurfaceAttribute', 'LoadUV0',           p) },
      { label: 'Instance ID',    factory: (p) => makeOpNode('SurfaceAttribute', 'LoadInstanceIndex', p) },
    ],
  },
  {
    group: 'Values',
    entries: [
      { label: 'Constant',  factory: (p) => makeConstantNode([1, 1, 1, 1], p) },
      { label: 'Parameter', factory: (p) => makeParameterNode('param', [0.5, 0.5, 0.5, 1], p) },
    ],
  },
  {
    group: 'Math',
    entries: [
      { label: 'Add',         factory: (p) => makeOpNode('BinaryOp',  'Add',         p) },
      { label: 'Subtract',    factory: (p) => makeOpNode('BinaryOp',  'Sub',         p) },
      { label: 'Multiply',    factory: (p) => makeOpNode('BinaryOp',  'Mul',         p) },
      { label: 'Divide',      factory: (p) => makeOpNode('BinaryOp',  'Div',         p) },
      { label: 'Dot3',        factory: (p) => makeOpNode('BinaryOp',  'Dot3',        p) },
      { label: 'Cross',       factory: (p) => makeOpNode('BinaryOp',  'Cross',       p) },
      { label: 'Negate',      factory: (p) => makeOpNode('UnaryOp',   'Neg',         p) },
      { label: 'Saturate',    factory: (p) => makeOpNode('UnaryOp',   'Saturate',    p) },
      { label: 'Normalize3',  factory: (p) => makeOpNode('UnaryOp',   'Normalize3',  p) },
      { label: 'Length3',     factory: (p) => makeOpNode('UnaryOp',   'Length3',     p) },
      { label: 'Splat',       factory: (p) => makeOpNode('UnaryOp',   'Splat',       p) },
      { label: 'Mix',         factory: (p) => makeOpNode('TernaryOp', 'Mix',         p) },
      { label: 'Clamp',       factory: (p) => makeOpNode('TernaryOp', 'Clamp',       p) },
    ],
  },
  {
    group: 'Outputs',
    entries: [
      { label: 'Write Albedo',       factory: (p) => makeOpNode('Output', 'WriteAlbedo',       p) },
      { label: 'Write Metallic',     factory: (p) => makeOpNode('Output', 'WriteMetallic',     p) },
      { label: 'Write Roughness',    factory: (p) => makeOpNode('Output', 'WriteRoughness',    p) },
      { label: 'Write Emission',     factory: (p) => makeOpNode('Output', 'WriteEmission',     p) },
      { label: 'Write Normal',       factory: (p) => makeOpNode('Output', 'WriteNormal',       p) },
      { label: 'Write Alpha',        factory: (p) => makeOpNode('Output', 'WriteAlpha',        p) },
      { label: 'Write IOR',          factory: (p) => makeOpNode('Output', 'WriteIor',          p) },
      { label: 'Write Transmission', factory: (p) => makeOpNode('Output', 'WriteTransmission', p) },
    ],
  },
];
