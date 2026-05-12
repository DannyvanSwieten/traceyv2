// Symmetric splay-apart layout used by every graph canvas's right-click
// "insert node on wire" action. The new node is placed at the cursor by
// the canvas; this helper computes how to nudge the *other* nodes so the
// new one fits between the wire's endpoints without overlapping anyone.
//
// Behaviour:
//   • The upstream-connected component (everything reachable backward
//     from the wire's from_node via incoming edges) shifts BACK along
//     the flow axis by half the room needed.
//   • The downstream-connected component (everything reachable forward
//     from the wire's to_node via outgoing edges) shifts FORWARD by the
//     same amount.
//   • Anything not connected to either endpoint stays put — unrelated
//     subgraphs don't move.
//   • The inserted node itself is never moved here (the canvas already
//     placed it at the cursor).
//
// Symmetric (not "push only downstream") because the user said "all
// nodes should move a bit": both sides yielding a little reads as the
// graph "opening up" around the cursor rather than a unilateral push.
//
// Cycles can't occur in Tracey's SOP/VOP/material graphs (DAG-only), so
// a node can't appear in both upstream and downstream sets. The BFS is
// guarded with a visited Set anyway.

export type FlowAxis = 'x' | 'y';

export interface InsertShiftConn {
  from_node: number;
  to_node: number;
}

export interface InsertShiftMove {
  uid: number;
  /** Signed shift to add to the node's flow-axis coordinate. */
  delta: number;
}

export interface InsertShiftParams {
  /** Wire being split. Its from_node is the upstream root, to_node the downstream root. */
  fromUid: number;
  toUid: number;
  /** New node's uid — excluded from shifting (it's already placed at the cursor). */
  insertedUid: number;
  /**
   * Flow-axis extent of the inserted node (width for L→R canvases, height
   * for T→B). Determines how much room to make.
   */
  insertedExtent: number;
  /** Extra spacing added on top of the inserted extent. */
  gap?: number;
  /** Current connections in the graph (after the splice has been applied). */
  connections: InsertShiftConn[];
}

const DEFAULT_GAP = 20;

export function computeInsertShift(p: InsertShiftParams): InsertShiftMove[] {
  const { fromUid, toUid, insertedUid, insertedExtent, connections } = p;
  const gap = p.gap ?? DEFAULT_GAP;

  // BFS upstream of fromUid — follow connections backward (incoming).
  const upstream = bfs(fromUid, (u) =>
    connections.filter((c) => c.to_node === u).map((c) => c.from_node),
  );
  // BFS downstream of toUid — follow connections forward (outgoing).
  const downstream = bfs(toUid, (u) =>
    connections.filter((c) => c.from_node === u).map((c) => c.to_node),
  );

  // The new node is reached from both sides via the freshly-added splice
  // edges; guard against accidentally moving it.
  upstream.delete(insertedUid);
  downstream.delete(insertedUid);

  // Each side gives up half the inserted-extent + gap. Total opening
  // between the original endpoints grows by (insertedExtent + gap).
  const halfShift = (insertedExtent + gap) / 2;

  const moves: InsertShiftMove[] = [];
  for (const uid of upstream)  moves.push({ uid, delta: -halfShift });
  for (const uid of downstream) moves.push({ uid, delta: +halfShift });
  return moves;
}

function bfs(seed: number, neighbours: (u: number) => number[]): Set<number> {
  const visited = new Set<number>([seed]);
  const queue: number[] = [seed];
  while (queue.length) {
    const u = queue.shift()!;
    for (const n of neighbours(u)) {
      if (visited.has(n)) continue;
      visited.add(n);
      queue.push(n);
    }
  }
  return visited;
}
