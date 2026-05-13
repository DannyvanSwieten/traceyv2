// Thin wrapper around the shared GraphCanvas — binds the SOP store and
// renders the SOP context menus + handles SOP-specific keyboard
// shortcuts. SOPs flow top-to-bottom (vertical orientation); the shared
// canvas honours that via DEFAULT_VERTICAL. All pan/zoom/marquee/cut/
// frame/multi-drag/wire-snap/click-click logic lives in the shared canvas.
//
// Subnet navigation (currentGraph(), enterSubnet, exitSubnet) is the
// only thing this wrapper layers on top of GraphCanvas — the store
// already exposes path-aware methods so the wiring stays small. The
// breadcrumb above the canvas is rendered by SopGraphEditor, not here.

import { Component, Show, createSignal, onCleanup, onMount } from 'solid-js';
import {
  GraphCanvas,
  type GraphCanvasAdapter,
  type ContextMenuPayload,
  type PortRef,
} from '../graph-canvas/GraphCanvas';
import { DEFAULT_VERTICAL } from '../../lib/graph_geometry';
import {
  SopNode,
  SopConnection,
  inputPortCount,
  outputPortCount,
  inputPortName,
  outputPortName,
  lookupCatalog,
  catalog,
  makeNode,
  allocNodeUid,
} from '../../lib/sop_graph';
import {
  addNode,
  connectToObjectOutput,
  currentGraph,
  enterSubnet,
  exitSubnet,
  moveNode,
  addConnection,
  removeConnection,
  removeNode,
  replaceNode,
  selectedNode,
  selectedNodes,
  setSelectedNode,
  setSelectedNodes,
  toggleSelectedNode,
  toggleNodeBypass,
  isNodeSelected,
  undo as undoSop,
  redo as redoSop,
} from '../../stores/sops';
import { openVopEditor } from '../../stores/vops';
import { ContextMenu, type MenuEntry } from '../context-menu/ContextMenu';
import { registerCommands } from '../../lib/command_palette';
import { computeInsertShift } from '../../lib/graph_insert_layout';

type MenuState =
  | { kind: 'add';    clientX: number; clientY: number; worldX: number; worldY: number;
                      connectFrom?: PortRef }
  | { kind: 'node';   clientX: number; clientY: number; nodeUid: number }
  | { kind: 'insert'; clientX: number; clientY: number; worldX: number; worldY: number;
                      conn: SopConnection }
  | null;

interface SopClipboard {
  nodes: SopNode[];
  connections: SopConnection[];
  originX: number;
  originY: number;
}
let sopClipboard: SopClipboard | null = null;

function nodeLabel(n: SopNode): string {
  // Prefer the node's user-edited `name` param (subnets, object_outputs,
  // glTF-imported branches all carry one) so the title shows what the
  // user actually called it, not the generic catalog kind.
  const named = n.params['name'];
  if (named && named.type === 'string' && typeof named.value === 'string' && named.value) {
    return named.value;
  }
  return lookupCatalog(n.kind)?.label ?? n.kind;
}

export const SopGraphCanvas: Component = () => {
  const [menu, setMenu] = createSignal<MenuState>(null);

  const adapter: GraphCanvasAdapter<SopNode> = {
    graph: () => currentGraph(),
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
    nodeLabel,
    nodeCategoryClass: (n) =>
      (lookupCatalog(n.kind)?.category ?? '').toLowerCase().replace(/\s+/g, '-'),
    isBypassed: (n) => !!n.bypass,
    onNodeDoubleClick: (n) => {
      if (n.kind === 'subnet') enterSubnet(n.uid);
      else if (n.kind === 'attribute_vop') openVopEditor(n.uid);
    },
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

  // ── SOP-specific helpers ────────────────────────────────────────────
  function canSwapInputs(uid: number): boolean {
    const node = currentGraph().nodes.find((n) => n.uid === uid);
    if (!node) return false;
    if (inputPortCount(node.kind) !== 2) return false;
    const conns = currentGraph().connections;
    const c0 = conns.find((c) => c.to_node === uid && c.to_port === 0);
    const c1 = conns.find((c) => c.to_node === uid && c.to_port === 1);
    return !!c0 && !!c1;
  }
  function swapInputs(uid: number): void {
    const conns = currentGraph().connections;
    const c0 = conns.find((c) => c.to_node === uid && c.to_port === 0);
    const c1 = conns.find((c) => c.to_node === uid && c.to_port === 1);
    if (!c0 || !c1) return;
    removeConnection(c0);
    removeConnection(c1);
    addConnection({ ...c0, to_port: 1 });
    addConnection({ ...c1, to_port: 0 });
  }

  // ── Clipboard ───────────────────────────────────────────────────────
  function copySelection() {
    const uids = new Set(selectedNodes());
    if (uids.size === 0) return;
    const g = currentGraph();
    const nodes: SopNode[] = [];
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
    sopClipboard = {
      nodes, connections,
      originX: minX === Infinity ? 0 : minX,
      originY: minY === Infinity ? 0 : minY,
    };
  }
  function pasteAt(targetX: number, targetY: number) {
    if (!sopClipboard || sopClipboard.nodes.length === 0) return;
    const remap = new Map<number, number>();
    const newUids: number[] = [];
    const dx = targetX - sopClipboard.originX;
    const dy = targetY - sopClipboard.originY;
    for (const src of sopClipboard.nodes) {
      const newUid = allocNodeUid();
      remap.set(src.uid, newUid);
      newUids.push(newUid);
      const srcPos = src.pos ?? [0, 0];
      const clone: SopNode = {
        ...structuredClone(src),
        uid: newUid,
        pos: [srcPos[0] + dx, srcPos[1] + dy],
      };
      addNode(clone);
    }
    for (const c of sopClipboard.connections) {
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
    const saved = sopClipboard;
    copySelection();
    if (sopClipboard) {
      pasteAt(sopClipboard.originX + 30, sopClipboard.originY + 30);
    }
    sopClipboard = saved ?? sopClipboard;
  }

  // ── Window-level shortcuts ──────────────────────────────────────────
  // GraphCanvas handles Delete, A (frame all), F (frame selection), and
  // Alt/Y modifier holds. Everything below is SOP-specific.
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
        if (k === 'v') { e.preventDefault(); pasteAt(0, 0); return; }
        if (k === 'd') { e.preventDefault(); duplicateSelection(); return; }
        // Undo / Redo. Cmd+Z = undo, Cmd+Shift+Z = redo. The SOP store's
        // snapshot covers the whole graph tree, including the VOP sub-
        // graphs nested inside attribute_vop nodes — so one stack reverses
        // edits made from any of the three canvases.
        if (k === 'z') {
          e.preventDefault();
          if (e.shiftKey) redoSop();
          else            undoSop();
          return;
        }
        return;
      }
      if (e.key === 'Escape') {
        e.preventDefault();
        exitSubnet();
        return;
      }
      if (e.key === 'o' || e.key === 'O') {
        const uid = selectedNode();
        if (uid === null) return;
        e.preventDefault();
        connectToObjectOutput(uid);
        return;
      }
      if (e.key === 's' || e.key === 'S') {
        const uid = selectedNode();
        if (uid === null) return;
        if (!canSwapInputs(uid)) return;
        e.preventDefault();
        swapInputs(uid);
        return;
      }
      if (e.key === 'b' || e.key === 'B') {
        const sel = selectedNodes();
        if (sel.length === 0) return;
        e.preventDefault();
        for (const u of sel) toggleNodeBypass(u);
        return;
      }
    };
    window.addEventListener('keydown', onDown);
    onCleanup(() => window.removeEventListener('keydown', onDown));

    // Register one Cmd+K command per SOP catalog entry. The catalog is
    // already populated by the time the dock opens; if it isn't, the
    // commands appear when the user next opens the palette since the
    // registry is reactive.
    const unregisterNodeCommands = registerCommands(
      catalog().map((e) => ({
        id: `sop.add.${e.kind}`,
        label: `Add SOP: ${e.label}`,
        group: 'SOP',
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

  // ── Menu entries ────────────────────────────────────────────────────
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
                              conn: SopConnection): MenuEntry[] {
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
          // SOP flow is vertical — shift up/downstream chains apart on Y
          // to make room. computeInsertShift is axis-agnostic; we apply
          // its result to the Y axis here (vs. X for VOP).
          const g = currentGraph();
          const moves = computeInsertShift({
            fromUid: conn.from_node,
            toUid:   conn.to_node,
            insertedUid: node.uid,
            insertedExtent: DEFAULT_VERTICAL.headerHeight + DEFAULT_VERTICAL.bodyPadding + 30,
            connections: g.connections,
          });
          for (const m of moves) {
            const n = g.nodes.find((x) => x.uid === m.uid);
            if (!n || !n.pos) continue;
            moveNode(m.uid, n.pos[0], n.pos[1] + m.delta);
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
    const node = currentGraph().nodes.find((n) => n.uid === uid);
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
    const node = currentGraph().nodes.find((n) => n.uid === uid);
    const entries: MenuEntry[] = [];
    if (node?.kind === 'subnet') {
      entries.push({
        kind: 'leaf',
        label: 'Enter Subnet',
        hint: 'dbl-click',
        onPick: () => enterSubnet(uid),
      });
    }
    if (node?.kind === 'attribute_vop') {
      entries.push({
        kind: 'leaf',
        label: 'Open VOP Editor',
        hint: 'dbl-click',
        onPick: () => openVopEditor(uid),
      });
    }
    entries.push({
      kind: 'leaf',
      label: 'Wire to Object Output',
      hint: 'O',
      onPick: () => connectToObjectOutput(uid),
    });
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
      label: node?.bypass ? 'Un-bypass Node' : 'Bypass Node',
      hint: 'B',
      onPick: () => toggleNodeBypass(uid),
    });
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
      <GraphCanvas adapter={adapter} geometry={DEFAULT_VERTICAL} />
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
                searchPlaceholder="Insert SOP node on wire…"
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
              searchPlaceholder={s.connectFrom ? 'Add & connect…' : 'Add SOP node…'}
              entries={buildAddEntries(s.worldX, s.worldY, s.connectFrom)}
              onClose={() => setMenu(null)}
            />
          );
        }}
      </Show>
    </>
  );
};
