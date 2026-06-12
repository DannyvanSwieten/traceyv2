// Flatten the SOP graph into a list of animated channels for the dopesheet.
//
// One entry per (node, param, component) pair that has at least one keyframe.
// Vec3 params produce up to three entries (x/y/z); scalar params produce one.
// The label is "<actor>.<param>" or "<actor>.<param>.<axis>" so the dopesheet
// channel list reads like "cube.translate.x" without forcing the user to
// learn SOP node uids.
//
// This module is a derived view over stores/sops; nothing here mutates state.

import { Channels, Extrap, Keyframe, ParamValue, SopGraph, SopNode } from './sop_graph';

const AXIS = ['x', 'y', 'z'];

export interface AnimatedChannel {
  nodeUid: number;
  paramName: string;
  component: number;     // 0 for scalar; 0/1/2 for vec3
  label: string;
  // The current key list, sorted by time. Same shape as the wire data so
  // components can render markers without a second indirection.
  keys: Keyframe[];
  // Component count of the owning param (3 for vec3, else 1) — the curve
  // editor uses it for the X/Y/Z colour convention.
  slots: number;
  paramType: 'float' | 'int' | 'bool' | 'vec3';
  // Extrapolation modes, surfaced so the curve editor can render the
  // out-of-range curve and show the current mode in its menus.
  pre: Extrap;
  post: Extrap;
}

// Pull the user-facing actor label off an object_output or subnet node by
// reading its `name` param. Falls back to the node kind so other animatable
// nodes still have a sensible row label.
function nodeLabel(n: SopNode): string {
  if (n.kind === 'object_output' || n.kind === 'subnet') {
    const p = n.params['name'];
    if (p && p.type === 'string' && typeof p.value === 'string' && p.value) {
      return p.value;
    }
    return `${n.kind}#${n.uid}`;
  }
  return `${n.kind}#${n.uid}`;
}

function channelComponentLabel(paramName: string, component: number, slots: number): string {
  if (slots <= 1) return paramName;
  const axis = AXIS[component] ?? `c${component}`;
  return `${paramName}.${axis}`;
}

function channelsOf(p: ParamValue): Channels | undefined {
  return p.type !== 'string' ? p.channels : undefined;
}

function paramSlotCount(p: ParamValue): number {
  return p.type === 'vec3' ? 3 : 1;
}

// Recursive walker — traverses the SOP graph plus every nested subnet
// subgraph. Path prefix accumulates ancestor labels (e.g. ["subnet_a"]) so
// channel rows read like "subnet_a/cube.translate.x" without the user having
// to think about subnet uids.
function collectChannels(g: SopGraph, prefix: string[], out: AnimatedChannel[]): void {
  for (const n of g.nodes) {
    const label = [...prefix, nodeLabel(n)].join('/');
    for (const [paramName, p] of Object.entries(n.params)) {
      const chs = channelsOf(p);
      if (!chs) continue;
      const slots = paramSlotCount(p);
      for (let c = 0; c < chs.length; ++c) {
        const ch = chs[c];
        if (!ch || ch.keys.length === 0) continue;
        out.push({
          nodeUid: n.uid,
          paramName,
          component: c,
          label: `${label}.${channelComponentLabel(paramName, c, slots)}`,
          keys: ch.keys.slice().sort((a, b) => a.t - b.t),
          slots,
          paramType: p.type as 'float' | 'int' | 'bool' | 'vec3',
          pre: ch.pre,
          post: ch.post,
        });
      }
    }
    if (n.subgraph) {
      collectChannels(n.subgraph, [...prefix, nodeLabel(n)], out);
    }
  }
}

export function listAnimatedChannels(graph: SopGraph): AnimatedChannel[] {
  const out: AnimatedChannel[] = [];
  collectChannels(graph, [], out);
  out.sort((a, b) => a.label.localeCompare(b.label));
  return out;
}
