// Thin wrapper around the shared GraphCanvas — binds the VOP store and
// renders the VOP context menus + handles VOP-specific keyboard
// shortcuts (S swap inputs, Cmd+C/V/D clipboard, Cmd+A select all).
// All canvas-level interaction (pan/zoom/marquee/cut/frame/multi-drag/
// wire-snap/click-click connect) lives in the shared canvas.

import { Component, Show, createSignal, onCleanup, onMount } from 'solid-js';
import {
  GraphCanvas,
  type GraphCanvasAdapter,
  type ContextMenuPayload,
  type PortRef,
} from '../graph-canvas/GraphCanvas';
import {
  VopNode,
  VopConnection,
  inputPortCount,
  outputPortCount,
  inputPortName,
  outputPortName,
  lookupCatalog,
  catalog,
  makeNode,
  allocNodeUid,
} from '../../lib/vop_graph';
import {
  vopGraph,
  moveNode,
  addNode,
  addConnection,
  removeConnection,
  removeNode,
  replaceNode,
  selectedNodes,
  setSelectedNode,
  setSelectedNodes,
  toggleSelectedNode,
  isNodeSelected,
} from '../../stores/vops';
import { undo as undoSop, redo as redoSop } from '../../stores/sops';
import { ContextMenu, type MenuEntry } from '../context-menu/ContextMenu';
import { registerCommands } from '../../lib/command_palette';
import { computeInsertShift } from '../../lib/graph_insert_layout';

type MenuState =
  | { kind: 'add';    clientX: number; clientY: number; worldX: number; worldY: number;
                      connectFrom?: PortRef }
  | { kind: 'node';   clientX: number; clientY: number; nodeUid: number }
  | { kind: 'insert'; clientX: number; clientY: number; worldX: number; worldY: number;
                      conn: VopConnection }
  | null;

// Module-scoped clipboard so the user can paste even when no VOP node is
// currently selected. Mirrors the original VopGraphCanvas behaviour.
interface VopClipboard {
  nodes: VopNode[];
  connections: VopConnection[];
  originX: number;
  originY: number;
}
let vopClipboard: VopClipboard | null = null;

export const VopGraphCanvas: Component = () => {
  const [menu, setMenu] = createSignal<MenuState>(null);

  const adapter: GraphCanvasAdapter<VopNode> = {
    graph: () => vopGraph(),
    selectedNodes,
    isNodeSelected,
    moveNode,
    removeNode,
    addConnection,
    removeConnection,
    setSelectedNode,
    setSelectedNodes,
    toggleSelectedNode,
    inputPortCount,
    outputPortCount,
    inputPortName,
    outputPortName,
    nodeLabel: (n) => lookupCatalog(n.kind)?.label ?? n.kind,
    nodeCategoryClass: (n) =>
      (lookupCatalog(n.kind)?.category ?? '').toLowerCase().replace(/\s+/g, '-'),
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
  // Swap the two incoming wires on a 2-input node. Houdini ergonomic.
  function canSwapInputs(uid: number): boolean {
    const node = vopGraph().nodes.find((n) => n.uid === uid);
    if (!node) return false;
    if (inputPortCount(node.kind) !== 2) return false;
    const conns = vopGraph().connections;
    const c0 = conns.find((c) => c.to_node === uid && c.to_port === 0);
    const c1 = conns.find((c) => c.to_node === uid && c.to_port === 1);
    return !!c0 && !!c1;
  }
  function swapInputs(uid: number): void {
    const conns = vopGraph().connections;
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
    const g = vopGraph();
    const nodes: VopNode[] = [];
    let minX = Infinity, minY = Infinity;
    for (const n of g.nodes) {
      if (!uids.has(n.uid)) continue;
      nodes.push(structuredClone(n));
      const [x, y] = n.pos ?? [0, 0];
      if (x < minX) minX = x;
      if (y < minY) minY = y;
    }
    if (nodes.length === 0) return;
    const connections = g.connections
      .filter((c) => uids.has(c.from_node) && uids.has(c.to_node))
      .map((c) => structuredClone(c));
    vopClipboard = {
      nodes, connections,
      originX: minX === Infinity ? 0 : minX,
      originY: minY === Infinity ? 0 : minY,
    };
  }
  function pasteAt(targetX: number, targetY: number) {
    if (!vopClipboard || vopClipboard.nodes.length === 0) return;
    const remap = new Map<number, number>();
    const newUids: number[] = [];
    const dx = targetX - vopClipboard.originX;
    const dy = targetY - vopClipboard.originY;
    for (const src of vopClipboard.nodes) {
      const newUid = allocNodeUid();
      remap.set(src.uid, newUid);
      newUids.push(newUid);
      const srcPos = src.pos ?? [0, 0];
      const clone: VopNode = {
        ...structuredClone(src),
        uid: newUid,
        pos: [srcPos[0] + dx, srcPos[1] + dy],
      };
      addNode(clone);
    }
    for (const c of vopClipboard.connections) {
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
    const saved = vopClipboard;
    copySelection();
    if (vopClipboard) {
      pasteAt(vopClipboard.originX + 30, vopClipboard.originY + 30);
    }
    vopClipboard = saved ?? vopClipboard;
  }

  // ── Window-level shortcuts ──────────────────────────────────────────
  // GraphCanvas handles Delete, A (frame all), F (frame selection), and
  // Alt/Y modifier holds. Everything else lives here.
  onMount(() => {
    const isTextEditing = (target: EventTarget | null): boolean => {
      const el = target as HTMLElement | null;
      if (!el) return false;
      const tag = el.tagName;
      if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return true;
      return el.isContentEditable === true;
    };
    const onDown = (e: KeyboardEvent) => {
      if (isTextEditing(e.target)) return;
      if (e.metaKey || e.ctrlKey) {
        const k = e.key.toLowerCase();
        if (k === 'c') { e.preventDefault(); copySelection(); return; }
        if (k === 'v') {
          e.preventDefault();
          // Paste at the centre of the current viewport — wrapper has no
          // access to live mouse coords, so we use the canvas centre as
          // a reasonable approximation.
          pasteAt(0, 0);
          return;
        }
        if (k === 'd') { e.preventDefault(); duplicateSelection(); return; }
        // VOP edits round-trip through the SOP graph (the parent
        // attribute_vop SOP node owns the VopGraph), so the SOP history
        // stack already covers them — delegate Cmd+Z here.
        if (k === 'z') {
          e.preventDefault();
          if (e.shiftKey) redoSop();
          else            undoSop();
          return;
        }
        return;
      }
      if (e.key === 's' || e.key === 'S') {
        const uids = selectedNodes();
        if (uids.length !== 1) return;
        const uid = uids[0];
        if (!canSwapInputs(uid)) return;
        e.preventDefault();
        swapInputs(uid);
      }
    };
    window.addEventListener('keydown', onDown);
    onCleanup(() => window.removeEventListener('keydown', onDown));

    const unregisterNodeCommands = registerCommands(
      catalog().map((e) => ({
        id: `vop.add.${e.kind}`,
        label: `Add VOP: ${e.label}`,
        group: 'VOP',
        keywords: `${e.kind} ${e.category}`,
        run: () => {
          const node = makeNode(e.kind, [120 + Math.random() * 80,
                                          80 + Math.random() * 80]);
          if (!node) return;
          addNode(node);
          setSelectedNode(node.uid);
        },
      })),
    );
    onCleanup(unregisterNodeCommands);
  });

  // ── Menu entry builders ─────────────────────────────────────────────
  function buildAddEntries(worldX: number, worldY: number,
                           connectFrom?: PortRef): MenuEntry[] {
    const byCat = new Map<string, MenuEntry[]>();
    for (const e of catalog()) {
      if (connectFrom) {
        if (connectFrom.kind === 'output' && e.inputs.length === 0) continue;
        if (connectFrom.kind === 'input'  && e.outputs.length === 0) continue;
      }
      const leafs = byCat.get(e.category) ?? [];
      leafs.push({
        kind: 'leaf',
        label: e.label,
        onPick: () => {
          const node = makeNode(e.kind, [worldX, worldY]);
          if (!node) return;
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
      });
      byCat.set(e.category, leafs);
    }
    const out: MenuEntry[] = [];
    for (const [cat, entries] of byCat) out.push({ kind: 'category', label: cat, entries });
    return out;
  }

  function buildInsertEntries(worldX: number, worldY: number,
                              conn: VopConnection): MenuEntry[] {
    const byCat = new Map<string, MenuEntry[]>();
    for (const e of catalog()) {
      if (e.inputs.length === 0 || e.outputs.length === 0) continue;
      const leafs = byCat.get(e.category) ?? [];
      leafs.push({
        kind: 'leaf',
        label: e.label,
        onPick: () => {
          const node = makeNode(e.kind, [worldX, worldY]);
          if (!node) return;
          addNode(node);
          removeConnection(conn);
          addConnection({ from_node: conn.from_node, from_port: conn.from_port,
                          to_node: node.uid,         to_port: 0 });
          addConnection({ from_node: node.uid,       from_port: 0,
                          to_node: conn.to_node,     to_port: conn.to_port });
          // Splay upstream/downstream apart so the inserted node has room.
          const g = vopGraph();
          const moves = computeInsertShift({
            fromUid: conn.from_node,
            toUid:   conn.to_node,
            insertedUid: node.uid,
            insertedExtent: 130, // matches GraphCanvas's default node width
            connections: g.connections,
          });
          for (const m of moves) {
            const n = g.nodes.find((x) => x.uid === m.uid);
            if (!n || !n.pos) continue;
            moveNode(m.uid, n.pos[0] + m.delta, n.pos[1]);
          }
          setSelectedNode(node.uid);
        },
      });
      byCat.set(e.category, leafs);
    }
    const out: MenuEntry[] = [];
    for (const [cat, entries] of byCat) out.push({ kind: 'category', label: cat, entries });
    return out;
  }

  function buildReplaceEntry(uid: number): MenuEntry {
    const node = vopGraph().nodes.find((n) => n.uid === uid);
    const byCat = new Map<string, MenuEntry[]>();
    for (const e of catalog()) {
      if (node && e.kind === node.kind) continue;
      const leafs = byCat.get(e.category) ?? [];
      leafs.push({
        kind: 'leaf',
        label: e.label,
        onPick: () => replaceNode(uid, e.kind),
      });
      byCat.set(e.category, leafs);
    }
    const entries: MenuEntry[] = [];
    for (const [cat, leafs] of byCat) entries.push({ kind: 'category', label: cat, entries: leafs });
    return { kind: 'category', label: 'Replace With', entries };
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
    entries.push(buildReplaceEntry(uid));
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
      <GraphCanvas adapter={adapter} />
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
                searchPlaceholder="Insert VOP node on wire…"
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
              searchPlaceholder={s.connectFrom ? 'Add & connect…' : 'Add VOP node…'}
              entries={buildAddEntries(s.worldX, s.worldY, s.connectFrom)}
              onClose={() => setMenu(null)}
            />
          );
        }}
      </Show>
    </>
  );
};
