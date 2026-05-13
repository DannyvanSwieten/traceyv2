// Thin wrapper around the shared GraphCanvas — binds it to the DOP
// store and renders the canvas's right-click menus (add node, insert on
// wire, node actions). All canvas interactions (pan/zoom/marquee/cut/
// frame/multi-drag/wire-snap) live in the shared canvas itself, so this
// stays small.

import { Component, Show, createSignal } from 'solid-js';
import {
  GraphCanvas,
  type GraphCanvasAdapter,
  type ContextMenuPayload,
  type PortRef,
} from '../graph-canvas/GraphCanvas';
import {
  DopNode,
  DopConnection,
  inputPortCount,
  outputPortCount,
  inputPortName,
  outputPortName,
  lookupCatalog,
  catalog,
  makeNode,
} from '../../lib/dop_graph';
import {
  dopGraph,
  moveNode,
  addNode,
  addConnection,
  removeConnection,
  removeNode,
  selectedNodes,
  setSelectedNode,
  setSelectedNodes,
  toggleSelectedNode,
  isNodeSelected,
} from '../../stores/dops';
import { openVopEditor } from '../../stores/vops';
import { ContextMenu, type MenuEntry } from '../context-menu/ContextMenu';

type MenuState =
  | { kind: 'add';    clientX: number; clientY: number; worldX: number; worldY: number;
                      connectFrom?: PortRef }
  | { kind: 'node';   clientX: number; clientY: number; nodeUid: number }
  | { kind: 'insert'; clientX: number; clientY: number; worldX: number; worldY: number;
                      conn: DopConnection }
  | null;

export const DopGraphCanvas: Component = () => {
  const [menu, setMenu] = createSignal<MenuState>(null);

  const adapter: GraphCanvasAdapter<DopNode> = {
    graph: () => dopGraph(),
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
    // pop_force hosts a VopGraph — open the per-host VOP editor. The
    // VOP IPC handler descends into the DOP graph to find the host.
    onNodeDoubleClick: (n) => {
      if (n.kind === 'pop_force') openVopEditor(n.uid);
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

  function buildAddEntries(worldX: number, worldY: number,
                           connectFrom?: PortRef): MenuEntry[] {
    const byCat = new Map<string, MenuEntry[]>();
    for (const e of catalog()) {
      // Filter out kinds that can't be wired up when seeding from a port.
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
                              conn: DopConnection): MenuEntry[] {
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
          setSelectedNode(node.uid);
        },
      });
      byCat.set(e.category, leafs);
    }
    const out: MenuEntry[] = [];
    for (const [cat, entries] of byCat) out.push({ kind: 'category', label: cat, entries });
    return out;
  }

  function buildNodeEntries(uid: number): MenuEntry[] {
    const out: MenuEntry[] = [];
    out.push({
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
    return out;
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
                searchPlaceholder="Insert DOP node on wire…"
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
              searchPlaceholder={s.connectFrom ? 'Add & connect…' : 'Add DOP node…'}
              entries={buildAddEntries(s.worldX, s.worldY, s.connectFrom)}
              onClose={() => setMenu(null)}
            />
          );
        }}
      </Show>
    </>
  );
};
