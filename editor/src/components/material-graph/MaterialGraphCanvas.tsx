// Thin wrapper around the shared GraphCanvas — binds the material store
// and renders the material context menus + material-specific keyboard
// shortcuts (S swap inputs, Cmd+C/V/D clipboard, Cmd+Z material undo).
// All canvas-level interaction (pan/zoom/marquee/cut/frame/multi-drag/
// wire-snap/click-click connect) lives in the shared canvas.

import { Component, Show, createSignal, onCleanup, onMount } from 'solid-js';
import {
  GraphCanvas,
  type GraphCanvasAdapter,
  type GraphCanvasHandle,
  type ContextMenuPayload,
  type PortRef,
} from '../graph-canvas/GraphCanvas';
import { type GraphGeometryConfig } from '../../lib/graph_geometry';
import {
  Node,
  Connection,
  inputPortCount,
  outputPortCount,
  inputPortName,
  outputPortName,
  PALETTE,
  allocNodeUid,
} from '../../lib/material_graph';
import {
  materialGraph,
  addNode,
  moveNode,
  addConnection,
  removeConnection,
  removeNode,
  selectedNode,
  selectedNodes,
  setSelectedNode,
  setSelectedNodes,
  toggleSelectedNode,
  isNodeSelected,
  undoMaterial,
  redoMaterial,
} from '../../stores/materials';
import { ContextMenu, type MenuEntry } from '../context-menu/ContextMenu';
import { registerCommands } from '../../lib/command_palette';
import { computeInsertShift } from '../../lib/graph_insert_layout';

// Material nodes are wider than the default horizontal config because
// the MaterialInput / MaterialOutput terminals stack a dozen named ports.
const MATERIAL_GEOMETRY: GraphGeometryConfig = {
  orientation: 'horizontal',
  nodeWidth: 180,
  headerHeight: 28,
  portRowHeight: 22,
  bodyPadding: 8,
  portRadius: 6,
};

// The shared canvas reads `node.pos`; the material wire format carries
// `position` (the C++ schema echoes it back as an unknown field). Bridge
// with a WeakMap so unchanged store nodes keep their mapped identity —
// the store updates immutably, so only edited nodes get a fresh object
// and the canvas's <For> re-renders just those.
type MatCanvasNode = Node & { pos: [number, number] };
const nodeViewCache = new WeakMap<Node, MatCanvasNode>();
function toCanvasNode(n: Node): MatCanvasNode {
  let m = nodeViewCache.get(n);
  if (!m) {
    m = { ...n, pos: [n.position?.[0] ?? 0, n.position?.[1] ?? 0] } as MatCanvasNode;
    nodeViewCache.set(n, m);
  }
  return m;
}

// Short label for a node header. Kinds with `op` show the op; constants and
// parameters show their value/name; the unified Material I/O terminals show
// a fixed label since they're singletons.
function nodeLabel(n: Node): string {
  switch (n.kind) {
    case 'Constant':
      return `Const (${n.value.map((v) => v.toFixed(2)).join(', ')})`;
    case 'Parameter':
      return `Param: ${n.name}`;
    case 'MaterialInput':
      return 'Material Input';
    case 'MaterialOutput':
      return 'Material Output';
    default:
      return n.op;
  }
}

type MenuState =
  | { kind: 'add';    clientX: number; clientY: number; worldX: number; worldY: number;
                      connectFrom?: PortRef }
  | { kind: 'node';   clientX: number; clientY: number; nodeUid: number }
  | { kind: 'insert'; clientX: number; clientY: number; worldX: number; worldY: number;
                      conn: Connection }
  | null;

// Module-scoped clipboard so the user can paste even when no material node
// is currently selected. Mirrors the VOP/SOP wrappers.
interface MatClipboard {
  nodes: Node[];
  connections: Connection[];
  originX: number;
  originY: number;
}
let matClipboard: MatClipboard | null = null;

export const MaterialGraphCanvas: Component = () => {
  const [menu, setMenu] = createSignal<MenuState>(null);
  let handle: GraphCanvasHandle | undefined;
  // Last pointer position in client coords so Cmd+V can paste under the
  // cursor (converted to world space through the canvas handle).
  let lastClient: [number, number] = [0, 0];

  const adapter: GraphCanvasAdapter<MatCanvasNode> = {
    graph: () => {
      const g = materialGraph();
      return { nodes: g.nodes.map(toCanvasNode), connections: g.connections };
    },
    selectedNodes,
    isNodeSelected,
    moveNode,
    removeNode,
    addConnection,
    removeConnection,
    setSelectedNode,
    setSelectedNodes,
    toggleSelectedNode,
    inputPortCount: (kind) => inputPortCount(kind as Node['kind']),
    outputPortCount: (kind) => outputPortCount(kind as Node['kind']),
    inputPortName: (kind, idx) => inputPortName(kind as Node['kind'], idx),
    outputPortName: (kind, idx) => outputPortName(kind as Node['kind'], idx),
    nodeLabel,
    nodeCategoryClass: (n) => n.kind,
    onContextMenu: (p: ContextMenuPayload) => {
      if (p.target.kind === 'node') {
        setMenu({ kind: 'node', clientX: p.clientX, clientY: p.clientY,
                  nodeUid: p.target.uid });
      } else if (p.target.kind === 'wire') {
        setMenu({ kind: 'insert', clientX: p.clientX, clientY: p.clientY,
                  worldX: p.worldX, worldY: p.worldY, conn: p.target.conn });
      } else {
        setMenu({ kind: 'add', clientX: p.clientX, clientY: p.clientY,
                  worldX: p.worldX, worldY: p.worldY, connectFrom: p.connectFrom });
      }
    },
  };

  // ── Swap inputs (S key) ─────────────────────────────────────────────
  function canSwapInputs(uid: number): boolean {
    const node = materialGraph().nodes.find((n) => n.uid === uid);
    if (!node) return false;
    if (inputPortCount(node.kind) !== 2) return false;
    const conns = materialGraph().connections;
    const c0 = conns.find((c) => c.to_node === uid && c.to_port === 0);
    const c1 = conns.find((c) => c.to_node === uid && c.to_port === 1);
    return !!c0 && !!c1;
  }
  function swapInputs(uid: number): void {
    const conns = materialGraph().connections;
    const c0 = conns.find((c) => c.to_node === uid && c.to_port === 0);
    const c1 = conns.find((c) => c.to_node === uid && c.to_port === 1);
    if (!c0 || !c1) return;
    removeConnection(c0);
    removeConnection(c1);
    addConnection({ ...c0, to_port: 1 });
    addConnection({ ...c1, to_port: 0 });
  }

  // ── Copy / paste / duplicate ────────────────────────────────────────
  function copySelection() {
    const uids = new Set(selectedNodes());
    if (uids.size === 0) return;
    const g = materialGraph();
    const nodes: Node[] = [];
    let minX = Infinity, minY = Infinity;
    for (const n of g.nodes) {
      if (!uids.has(n.uid)) continue;
      nodes.push(structuredClone(n));
      const [x, y] = n.position ?? [0, 0];
      if (x < minX) minX = x;
      if (y < minY) minY = y;
    }
    if (nodes.length === 0) return;
    const connections = g.connections
      .filter((c) => uids.has(c.from_node) && uids.has(c.to_node))
      .map((c) => structuredClone(c));
    matClipboard = {
      nodes, connections,
      originX: minX === Infinity ? 0 : minX,
      originY: minY === Infinity ? 0 : minY,
    };
  }
  function pasteAt(targetX: number, targetY: number) {
    if (!matClipboard || matClipboard.nodes.length === 0) return;
    const remap = new Map<number, number>();
    const newUids: number[] = [];
    const dx = targetX - matClipboard.originX;
    const dy = targetY - matClipboard.originY;
    for (const src of matClipboard.nodes) {
      const newUid = allocNodeUid();
      remap.set(src.uid, newUid);
      newUids.push(newUid);
      const srcPos = src.position ?? [0, 0];
      const clone: Node = {
        ...structuredClone(src),
        uid: newUid,
        position: [srcPos[0] + dx, srcPos[1] + dy],
      } as Node;
      addNode(clone);
    }
    for (const c of matClipboard.connections) {
      const fn = remap.get(c.from_node);
      const tn = remap.get(c.to_node);
      if (fn === undefined || tn === undefined) continue;
      addConnection({ from_node: fn, from_port: c.from_port, to_node: tn, to_port: c.to_port });
    }
    setSelectedNodes(newUids);
  }
  function duplicateSelection() {
    const uids = selectedNodes();
    if (uids.length === 0) return;
    const saved = matClipboard;
    copySelection();
    if (matClipboard) {
      pasteAt(matClipboard.originX + 30, matClipboard.originY + 30);
    }
    matClipboard = saved ?? matClipboard;
  }

  // ── Window-level shortcuts ──────────────────────────────────────────
  // GraphCanvas handles Delete, Cmd+A, A (frame all), F (frame selection),
  // and Alt/Y modifier holds. Everything else lives here.
  onMount(() => {
    const isTextEditing = (target: EventTarget | null): boolean => {
      const el = target as HTMLElement | null;
      if (!el) return false;
      const tag = el.tagName;
      if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return true;
      return el.isContentEditable === true;
    };
    const onPointerMove = (e: PointerEvent) => {
      lastClient = [e.clientX, e.clientY];
    };
    const onDown = (e: KeyboardEvent) => {
      if (isTextEditing(e.target)) return;
      if (e.metaKey || e.ctrlKey) {
        const k = e.key.toLowerCase();
        if (k === 'c') { e.preventDefault(); copySelection(); return; }
        if (k === 'v') {
          e.preventDefault();
          const [wx, wy] = handle
            ? handle.worldFromClient(lastClient[0], lastClient[1])
            : [0, 0];
          pasteAt(wx, wy);
          return;
        }
        if (k === 'd') { e.preventDefault(); duplicateSelection(); return; }
        // Materials have their own snapshot stack (separate from the SOP
        // one) since the graph isn't part of the scene tree.
        if (k === 'z') {
          e.preventDefault();
          if (e.shiftKey) redoMaterial();
          else            undoMaterial();
          return;
        }
        return;
      }
      if (e.key === 's' || e.key === 'S') {
        const uid = selectedNode();
        if (uid === null || !canSwapInputs(uid)) return;
        e.preventDefault();
        swapInputs(uid);
      }
    };
    window.addEventListener('pointermove', onPointerMove);
    window.addEventListener('keydown', onDown);
    onCleanup(() => {
      window.removeEventListener('pointermove', onPointerMove);
      window.removeEventListener('keydown', onDown);
    });

    // Flatten the static PALETTE into one command per entry. Group prefix
    // mirrors the palette grouping (Math, Texturing, …) so the user can
    // type "math add" and land on Add directly.
    const matCommands = PALETTE.flatMap((g) =>
      g.entries.map((entry) => ({
        id: `material.add.${g.group}.${entry.label}`,
        label: `Add Material: ${entry.label}`,
        group: 'Material',
        keywords: g.group,
        run: () => {
          const node = entry.factory([120 + Math.random() * 80,
                                       80 + Math.random() * 80]);
          addNode(node);
          setSelectedNode(node.uid);
        },
      })),
    );
    const unregisterNodeCommands = registerCommands(matCommands);
    onCleanup(unregisterNodeCommands);
  });

  // ── Menu entry builders ─────────────────────────────────────────────
  function buildAddEntries(worldX: number, worldY: number,
                           connectFrom?: PortRef): MenuEntry[] {
    // PALETTE is grouped at the source. In add+connect mode, probe each
    // entry's port shape via a throwaway factory call to filter out
    // nodes that can't satisfy the drag direction.
    return PALETTE.map((g) => ({
      kind: 'category' as const,
      label: g.group,
      entries: g.entries
        .map((p): MenuEntry | null => {
          if (connectFrom) {
            const probe = p.factory([0, 0]);
            const ins = inputPortCount(probe.kind);
            const outs = outputPortCount(probe.kind);
            if (connectFrom.kind === 'output' && ins === 0) return null;
            if (connectFrom.kind === 'input'  && outs === 0) return null;
          }
          return {
            kind: 'leaf' as const,
            label: p.label,
            onPick: () => {
              const node = p.factory([worldX, worldY]);
              addNode(node);
              if (connectFrom) {
                if (connectFrom.kind === 'output') {
                  addConnection({
                    from_node: connectFrom.nodeUid, from_port: connectFrom.portIdx,
                    to_node: node.uid,              to_port: 0,
                  });
                } else {
                  addConnection({
                    from_node: node.uid,            from_port: 0,
                    to_node: connectFrom.nodeUid,   to_port: connectFrom.portIdx,
                  });
                }
              }
              setSelectedNode(node.uid);
            },
          };
        })
        .filter((e): e is MenuEntry => e !== null),
    })).filter((g) => g.entries.length > 0);
  }

  // Insert-on-wire: filter PALETTE to nodes with ≥1 input and ≥1 output,
  // splice the wire on pick.
  function buildInsertEntries(worldX: number, worldY: number,
                              conn: Connection): MenuEntry[] {
    return PALETTE.map((g) => ({
      kind: 'category' as const,
      label: g.group,
      entries: g.entries
        .map((p): MenuEntry | null => {
          const probe = p.factory([0, 0]);
          const ins = inputPortCount(probe.kind);
          const outs = outputPortCount(probe.kind);
          if (ins === 0 || outs === 0) return null;
          return {
            kind: 'leaf' as const,
            label: p.label,
            onPick: () => {
              const node = p.factory([worldX, worldY]);
              addNode(node);
              removeConnection(conn);
              addConnection({ from_node: conn.from_node, from_port: conn.from_port,
                              to_node: node.uid,         to_port: 0 });
              addConnection({ from_node: node.uid,       from_port: 0,
                              to_node: conn.to_node,     to_port: conn.to_port });
              // Splay upstream/downstream apart so the inserted node has room.
              const g = materialGraph();
              const moves = computeInsertShift({
                fromUid: conn.from_node,
                toUid:   conn.to_node,
                insertedUid: node.uid,
                insertedExtent: MATERIAL_GEOMETRY.nodeWidth,
                connections: g.connections,
              });
              for (const m of moves) {
                const n = g.nodes.find((x) => x.uid === m.uid);
                if (!n || !n.position) continue;
                moveNode(m.uid, n.position[0] + m.delta, n.position[1]);
              }
              setSelectedNode(node.uid);
            },
          };
        })
        .filter((e): e is MenuEntry => e !== null),
    })).filter((g) => g.entries.length > 0);
  }

  function buildNodeEntries(uid: number): MenuEntry[] {
    const entries: MenuEntry[] = [];
    if (canSwapInputs(uid)) {
      entries.push({
        kind: 'leaf',
        label: 'Swap Inputs',
        hint: 'S',
        onPick: () => swapInputs(uid),
      });
    }
    entries.push({
      kind: 'leaf',
      label: selectedNodes().length > 1
        ? `Delete ${selectedNodes().length} Nodes`
        : 'Delete Node',
      hint: '⌫',
      onPick: () => {
        const uids = [...selectedNodes()];
        if (uids.length === 0) uids.push(uid);
        for (const u of uids) removeNode(u);
        setSelectedNode(null);
      },
    });
    return entries;
  }

  return (
    <>
      <GraphCanvas
        adapter={adapter}
        geometry={MATERIAL_GEOMETRY}
        handle={(h) => { handle = h; }}
      />
      <Show when={menu()} keyed>
        {(s) => {
          if (s.kind === 'node') {
            return (
              <ContextMenu
                x={s.clientX}
                y={s.clientY}
                entries={buildNodeEntries(s.nodeUid)}
                onClose={() => setMenu(null)}
              />
            );
          }
          if (s.kind === 'insert') {
            return (
              <ContextMenu
                x={s.clientX}
                y={s.clientY}
                showSearch
                searchPlaceholder="Insert node on wire…"
                entries={buildInsertEntries(s.worldX, s.worldY, s.conn)}
                onClose={() => setMenu(null)}
              />
            );
          }
          return (
            <ContextMenu
              x={s.clientX}
              y={s.clientY}
              showSearch
              searchPlaceholder={s.connectFrom ? 'Add & connect…' : 'Add material node…'}
              entries={buildAddEntries(s.worldX, s.worldY, s.connectFrom)}
              onClose={() => setMenu(null)}
            />
          );
        }}
      </Show>
    </>
  );
};
