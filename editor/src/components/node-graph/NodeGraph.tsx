import { Component, createSignal, onMount, onCleanup, For, Show } from 'solid-js';
import { invoke } from '@tauri-apps/api/core';
import './NodeGraph.css';

// Vec3 value from backend: { "type": "Vec3", "value": { "x": ..., "y": ..., "z": ... } }
export interface Vec3Value {
  x: number;
  y: number;
  z: number;
}

export interface ParameterValue {
  type: string;
  // For Float/Int/Bool, value is the primitive; for Vec3, value is { x, y, z }
  value?: number | string | boolean | Vec3Value;
}

// Helper to get Vec3 from parameter value
const getVec3Value = (param: ParameterValue | null): Vec3Value | null => {
  if (param?.type === 'Vec3' && typeof param.value === 'object' && param.value !== null) {
    return param.value as Vec3Value;
  }
  return null;
};

export interface ParameterInfo {
  name: string;
  value: ParameterValue | null;
}

export interface NodeDetails {
  id: number;
  name: string;
  node_type: string;
  parameters: ParameterInfo[];
}

export interface NodeData {
  id: number;
  type: string;
  name: string;
  x: number;
  y: number;
  parameters?: ParameterInfo[];
}

export interface Connection {
  from_node: number;
  to_node: number;
  from_port?: string;
  to_port?: string;
}

export interface PortInfo {
  name: string;
  data_type: string;
  port_type: string;
}

export interface GraphContextInfo {
  level: string;
  actor_node_uid: number | null;
}

interface BreadcrumbItem {
  label: string;
  actorNodeUid: number | null;
}

export interface NodeGraphHandle {
  refresh: () => Promise<void>;
}

interface NodeGraphProps {
  onEvaluate?: () => Promise<void>;
  onActorNodeCreated?: () => Promise<void>;
  onActorNodeSelected?: (actorNodeUid: number) => void;
  onReady?: (handle: NodeGraphHandle) => void;
}

// Debounce utility
function debounce<T extends (...args: any[]) => any>(fn: T, delay: number): T {
  let timeoutId: number | undefined;
  return ((...args: Parameters<T>) => {
    if (timeoutId !== undefined) {
      clearTimeout(timeoutId);
    }
    timeoutId = window.setTimeout(() => fn(...args), delay);
  }) as T;
}

export const NodeGraph: Component<NodeGraphProps> = (props) => {
  const [nodes, setNodes] = createSignal<NodeData[]>([]);
  const [connections, setConnections] = createSignal<Connection[]>([]);
  const [selectedNodes, setSelectedNodes] = createSignal<Set<number>>(new Set());
  const [draggingNode, setDraggingNode] = createSignal<number | null>(null);
  const [dragOffset, setDragOffset] = createSignal({ x: 0, y: 0 });
  const [viewOffset, setViewOffset] = createSignal({ x: 0, y: 0 });
  const [isPanning, setIsPanning] = createSignal(false);
  const [panStart, setPanStart] = createSignal({ x: 0, y: 0 });
  const [zoom, setZoom] = createSignal(1);
  const [showNodeMenu, setShowNodeMenu] = createSignal(false);
  const [nodeMenuPos, setNodeMenuPos] = createSignal({ x: 0, y: 0 });
  const [connecting, setConnecting] = createSignal<{ nodeId: number; isOutput: boolean; portName?: string } | null>(null);
  const [outputLocked, setOutputLocked] = createSignal(false);
  const [nodeErrors, setNodeErrors] = createSignal<Map<number, string>>(new Map());
  const [nodePorts, setNodePorts] = createSignal<Map<number, PortInfo[]>>(new Map());
  // Box selection state
  const [boxSelectStart, setBoxSelectStart] = createSignal<{ x: number; y: number } | null>(null);
  const [boxSelectEnd, setBoxSelectEnd] = createSignal<{ x: number; y: number } | null>(null);
  // Clipboard state
  const [clipboard, setClipboard] = createSignal<{ nodes: NodeData[]; connections: Connection[] } | null>(null);

  // Phase 2: Graph navigation state
  const [graphContext, setGraphContext] = createSignal<GraphContextInfo>({ level: 'scene', actor_node_uid: null });
  const [breadcrumb, setBreadcrumb] = createSignal<BreadcrumbItem[]>([{ label: 'Scene', actorNodeUid: null }]);

  let canvasRef: HTMLDivElement | undefined;

  // Helper to get port color based on data type
  const getPortColor = (dataType: string): string => {
    switch (dataType.toLowerCase()) {
      case 'geometry': return '#4a9eff';  // Blue for geometry
      case 'float': return '#90ee90';     // Light green for float
      case 'vec2': return '#ffb347';      // Orange for vec2
      case 'vec3': return '#ff6b6b';      // Red for vec3
      case 'vec4': return '#c77dff';      // Purple for vec4
      case 'int': return '#98d8c8';       // Teal for int
      case 'bool': return '#f7dc6f';      // Yellow for bool
      default: return '#888';             // Gray for unknown
    }
  };

  // Fetch port information for a node
  const fetchNodePorts = async (nodeId: number): Promise<PortInfo[]> => {
    try {
      const ports = await invoke<PortInfo[]>('get_node_ports', { nodeUid: nodeId });
      return ports;
    } catch (error) {
      console.error(`Failed to fetch ports for node ${nodeId}:`, error);
      return [];
    }
  };

  // Context-aware node types based on current graph level
  const getAvailableNodeTypes = () => {
    const ctx = graphContext();
    if (ctx.level === 'scene') {
      // Scene level: only Actor nodes
      return [{ type: 'actor', label: 'Actor Node' }];
    } else {
      // Geometry network: SOP nodes only
      return [
        { type: 'cube', label: 'Cube' },
        { type: 'sphere', label: 'Sphere' },
        { type: 'torus', label: 'Torus' },
        { type: 'plane', label: 'Plane' },
        { type: 'cylinder', label: 'Cylinder' },
        { type: 'cone', label: 'Cone' },
        { type: 'transform', label: 'Transform' },
        { type: 'merge', label: 'Merge' },
      ];
    }
  };

  const refreshGraphContext = async () => {
    try {
      const ctx = await invoke<GraphContextInfo>('get_graph_context');
      setGraphContext(ctx);
    } catch (error) {
      console.error('Failed to get graph context:', error);
    }
  };

  const refreshGraph = async () => {
    try {
      await refreshGraphContext();

      const nodeIds = await invoke<number[]>('get_all_nodes');
      const conns = await invoke<Connection[]>('get_all_connections');

      // Fetch details and ports for each node
      const nodeData: NodeData[] = await Promise.all(
        nodeIds.map(async (id, index) => {
          // Try to find existing node to preserve position
          const existing = nodes().find(n => n.id === id);

          try {
            const details = await invoke<NodeDetails>('get_node_details', { nodeUid: id });
            // Also fetch port information
            const ports = await fetchNodePorts(id);

            // Store ports in the map
            setNodePorts(prev => {
              const newMap = new Map(prev);
              newMap.set(id, ports);
              return newMap;
            });

            return {
              id,
              type: details.node_type,
              name: details.name,
              parameters: details.parameters,
              x: existing?.x ?? 50 + (index % 3) * 250,
              y: existing?.y ?? 50 + Math.floor(index / 3) * 200,
            };
          } catch (error) {
            console.error(`Failed to get details for node ${id}:`, error);
            return {
              id,
              type: 'unknown',
              name: `Node ${id}`,
              x: existing?.x ?? 50 + (index % 3) * 250,
              y: existing?.y ?? 50 + Math.floor(index / 3) * 200,
            };
          }
        })
      );

      setNodes(nodeData);
      setConnections(conns);
    } catch (error) {
      console.error('Failed to refresh graph:', error);
    }
  };

  const createNode = async (type: string) => {
    try {
      const pos = nodeMenuPos();
      const worldPos = screenToWorld(pos.x, pos.y);

      const nodeId = await invoke<number>('create_node', {
        nodeType: type,
        name: type,
      });

      console.log(`Created node ${nodeId} of type ${type} at position (${worldPos.x}, ${worldPos.y})`);

      // Add node to local state with correct position BEFORE refresh
      // This ensures the position is preserved when refreshGraph() is called
      setNodes([...nodes(), {
        id: nodeId,
        type: type,
        name: type,
        x: worldPos.x,
        y: worldPos.y,
      }]);

      // Check if we created an ActorNode at scene level
      const ctx = graphContext();
      const isSceneActorNode = type === 'actor' && ctx.level === 'scene';

      // Refresh from backend to ensure consistency
      // The position will be preserved via existing?.x in refreshGraph
      await refreshGraph();

      // Notify parent if we created an ActorNode (to update scene hierarchy)
      if (isSceneActorNode && props.onActorNodeCreated) {
        await props.onActorNodeCreated();
      }

      // Automatically select the newly created node
      await selectNodeAndSetOutput(nodeId);

      setShowNodeMenu(false);
    } catch (error) {
      console.error('Failed to create node:', error);
    }
  };

  const deleteNode = async (nodeId: number) => {
    try {
      await invoke('remove_node', { nodeUid: nodeId });
      setNodes(nodes().filter(n => n.id !== nodeId));
      setConnections(connections().filter(c => c.from_node !== nodeId && c.to_node !== nodeId));
      if (selectedNodes().has(nodeId)) {
        const newSelection = new Set(selectedNodes());
        newSelection.delete(nodeId);
        setSelectedNodes(newSelection);
      }

      // Automatically evaluate the graph after removing a node
      await evaluateGraph();
    } catch (error) {
      console.error('Failed to delete node:', error);
    }
  };

  const connectNodes = async (fromNode: number, fromPort: string, toNode: number, toPort: string) => {
    try {
      await invoke('connect_nodes', {
        fromNode,
        fromPort,
        toNode,
        toPort,
      });

      setConnections([...connections(), { from_node: fromNode, to_node: toNode, from_port: fromPort, to_port: toPort }]);

      // Automatically evaluate the graph after connecting nodes
      await evaluateGraph();
    } catch (error) {
      console.error('Failed to connect nodes:', error);
    }
  };

  const disconnectNodes = async (fromNode: number, toNode: number) => {
    try {
      await invoke('disconnect_nodes', {
        fromNode,
        toNode,
      });

      // Remove connection from local state
      setConnections(connections().filter(c =>
        !(c.from_node === fromNode && c.to_node === toNode)
      ));

      // Automatically evaluate the graph after disconnecting nodes
      await evaluateGraph();
    } catch (error) {
      console.error('Failed to disconnect nodes:', error);
    }
  };

  const setAsOutput = async (nodeId?: number) => {
    // Get first selected node if no specific node provided
    const firstSelected = selectedNodes().size > 0 ? Array.from(selectedNodes())[0] : null;
    const targetNode = nodeId ?? firstSelected;
    if (targetNode === null) {
      console.warn('Cannot set output: no node selected');
      return;
    }

    try {
      await invoke('set_graph_output', {
        outputName: 'geometry',
        nodeUid: targetNode,
      });
      console.log(`Set node ${targetNode} as graph output`);

      // Automatically evaluate the graph after setting output
      await evaluateGraph();
    } catch (error) {
      console.error('Failed to set graph output:', error);
    }
  };

  const selectNodeAndSetOutput = async (nodeId: number, addToSelection: boolean = false) => {
    if (addToSelection) {
      // Toggle selection with shift+click
      const newSelection = new Set(selectedNodes());
      if (newSelection.has(nodeId)) {
        newSelection.delete(nodeId);
      } else {
        newSelection.add(nodeId);
      }
      setSelectedNodes(newSelection);
    } else {
      // Single selection (replace)
      setSelectedNodes(new Set([nodeId]));
    }

    // If it's an actor node, notify parent to sync hierarchy selection
    const node = nodes().find(n => n.id === nodeId);
    if (node && node.type === 'actor' && props.onActorNodeSelected) {
      props.onActorNodeSelected(nodeId);
    }

    // Set as graph output (for visualization) but DON'T evaluate.
    // This updates which node is the "output" without triggering scene compilation
    // which would reset material state. Evaluation only happens on explicit actions
    // like connecting nodes, changing parameters, or pressing evaluate.
    if (!outputLocked()) {
      try {
        await invoke('set_graph_output', {
          outputName: 'geometry',
          nodeUid: nodeId,
        });
        console.log(`Set node ${nodeId} as graph output (no evaluation)`);
      } catch (error) {
        // Silently ignore errors - the node might not have a geometry output
        console.debug(`Could not set node ${nodeId} as output:`, error);
      }
    }
  };

  const toggleOutputLock = () => {
    setOutputLocked(!outputLocked());
    console.log(`Output ${outputLocked() ? 'locked' : 'unlocked'}`);
  };

  const evaluateGraph = async () => {
    try {
      console.log('Evaluating node graph...');
      await invoke('evaluate_graph', { time: 0.0, frame: 0 });
      console.log('Graph evaluated, compiling scene...');

      // Clear errors on successful evaluation
      setNodeErrors(new Map());

      // Call the parent's onEvaluate which compiles and renders
      await props.onEvaluate?.();

      console.log('Graph evaluation complete and rendered');
    } catch (error) {
      console.error('Failed to evaluate graph:', error);

      // Store error - try to extract node ID from error message if possible
      const errorMsg = String(error);
      const errors = new Map<number, string>();

      // Try to parse node-specific errors (format: "Node X failed: ...")
      const nodeErrorMatch = errorMsg.match(/Node (\d+)|node (\d+)/i);
      if (nodeErrorMatch) {
        const nodeId = parseInt(nodeErrorMatch[1] || nodeErrorMatch[2]);
        errors.set(nodeId, errorMsg);
      } else {
        // General error - mark currently selected nodes if any
        const selection = selectedNodes();
        if (selection.size > 0) {
          const firstSelected = Array.from(selection)[0];
          errors.set(firstSelected, errorMsg);
        }
      }

      setNodeErrors(errors);
    }
  };

  // Debounced version for parameter changes (300ms delay)
  const debouncedEvaluateGraph = debounce(evaluateGraph, 300);

  const updateParameter = async (nodeId: number, paramName: string, value: any) => {
    try {
      await invoke('set_node_parameter', {
        nodeUid: nodeId,
        paramName,
        value,
      });

      // Update local state - value is already { type: 'Float', value: number }
      setNodes(nodes().map(n => {
        if (n.id === nodeId && n.parameters) {
          return {
            ...n,
            parameters: n.parameters.map(p =>
              p.name === paramName
                ? { ...p, value }
                : p
            ),
          };
        }
        return n;
      }));

      console.log(`Updated ${paramName} on node ${nodeId} to ${value}`);

      // Use debounced evaluation for parameter changes to prevent spam
      debouncedEvaluateGraph();
    } catch (error) {
      console.error('Failed to update parameter:', error);
    }
  };

  // Convert screen coordinates to world coordinates (for placing new nodes, etc.)
  const screenToWorld = (screenX: number, screenY: number) => {
    if (!canvasRef) return { x: screenX, y: screenY };
    const rect = canvasRef.getBoundingClientRect();
    const offset = viewOffset();
    const z = zoom();
    // Subtract canvas position, then apply inverse transform
    return {
      x: (screenX - rect.left - offset.x) / z,
      y: (screenY - rect.top - offset.y) / z,
    };
  };

  // Get the CSS transform string for the canvas (single transform for all nodes)
  const getCanvasTransform = () => {
    const offset = viewOffset();
    const z = zoom();
    return `translate(${offset.x}px, ${offset.y}px) scale(${z})`;
  };

  const navigateToBreadcrumb = async (index: number) => {
    try {
      const item = breadcrumb()[index];

      if (item.actorNodeUid === null) {
        // Navigate to scene level
        await invoke('navigate_to_scene_graph');
      } else {
        // Navigate to specific actor node
        await invoke('navigate_to_actor_node', { actorNodeUid: item.actorNodeUid });
      }

      // Update breadcrumb (remove items after clicked index)
      setBreadcrumb(breadcrumb().slice(0, index + 1));

      // Refresh graph
      await refreshGraph();
    } catch (error) {
      console.error('Failed to navigate via breadcrumb:', error);
      alert(`Navigation failed: ${error}`);
    }
  };

  const handleCanvasMouseDown = (e: MouseEvent) => {
    if (e.button === 1 || (e.button === 0 && e.altKey)) {
      // Middle mouse or Alt+Left mouse for panning
      setIsPanning(true);
      setPanStart({ x: e.clientX - viewOffset().x, y: e.clientY - viewOffset().y });
      e.preventDefault();
    } else if (e.button === 2) {
      // Right click - show node menu
      setNodeMenuPos({ x: e.clientX, y: e.clientY });
      setShowNodeMenu(true);
      e.preventDefault();
    } else if (e.button === 0) {
      // Left click on canvas - deselect all nodes
      setSelectedNodes(new Set<number>());
    }
  };

  const handleCanvasMouseMove = (e: MouseEvent) => {
    if (isPanning()) {
      const panS = panStart();
      setViewOffset({
        x: e.clientX - panS.x,
        y: e.clientY - panS.y,
      });
    }

    const dragging = draggingNode();
    if (dragging !== null) {
      const node = nodes().find(n => n.id === dragging);
      if (node) {
        const worldPos = screenToWorld(e.clientX, e.clientY);
        const offset = dragOffset();
        setNodes(nodes().map(n =>
          n.id === dragging
            ? { ...n, x: worldPos.x - offset.x, y: worldPos.y - offset.y }
            : n
        ));
      }
    }
  };

  const handleCanvasMouseUp = (e: MouseEvent) => {
    setIsPanning(false);
    setDraggingNode(null);

    // Only cancel connection if clicking on empty canvas (not on a port)
    const target = e.target as HTMLElement;
    if (connecting() && !target.classList.contains('port')) {
      console.log('Cancelling connection (clicked on canvas)');
      setConnecting(null);
    }
  };

  const handleNodeMouseDown = (e: MouseEvent, nodeId: number) => {
    // Check if click target is a port - if so, ignore node drag
    const target = e.target as HTMLElement;
    if (target.classList.contains('port')) {
      console.log('Clicked on port, ignoring node drag');
      return;
    }

    if (e.button === 0) {
      e.stopPropagation();
      e.preventDefault(); // Prevent text selection
      // Shift+click for multi-selection
      selectNodeAndSetOutput(nodeId, e.shiftKey);
      setDraggingNode(nodeId);

      const node = nodes().find(n => n.id === nodeId)!;
      const worldPos = screenToWorld(e.clientX, e.clientY);
      setDragOffset({
        x: worldPos.x - node.x,
        y: worldPos.y - node.y,
      });
    }
  };

  const handleNodeDoubleClick = async (e: MouseEvent, nodeId: number) => {
    e.stopPropagation();
    e.preventDefault();

    const node = nodes().find(n => n.id === nodeId);
    if (node && node.type === 'Actor') {
      try {
        // Navigate into the ActorNode's geometry network
        await invoke('navigate_to_actor_node', { actorNodeUid: nodeId });

        // Update breadcrumb
        setBreadcrumb([...breadcrumb(), { label: node.name, actorNodeUid: nodeId }]);

        // Refresh graph to show nested nodes
        await refreshGraph();

        console.log(`Drilled into ActorNode ${nodeId}: ${node.name}`);
      } catch (error) {
        console.error('Failed to navigate to actor node:', error);
        alert(`Failed to drill into actor node: ${error}`);
      }
    }
  };

  const handleNodeContextMenu = (e: MouseEvent, nodeId: number) => {
    e.preventDefault();
    e.stopPropagation();
    if (confirm(`Delete node ${nodeId}?`)) {
      deleteNode(nodeId);
    }
  };

  const handleOutputPortClick = (e: MouseEvent, nodeId: number, portName: string) => {
    e.stopPropagation();
    e.preventDefault();
    setConnecting({ nodeId, isOutput: true, portName });
    console.log(`Started connection from node ${nodeId}, port ${portName}`);
  };

  const handleInputPortClick = (e: MouseEvent, nodeId: number, portName: string) => {
    e.stopPropagation();
    e.preventDefault();
    const conn = connecting();
    if (conn && conn.isOutput && conn.portName) {
      console.log(`Connecting node ${conn.nodeId}:${conn.portName} to node ${nodeId}:${portName}`);
      connectNodes(conn.nodeId, conn.portName, nodeId, portName);
      setConnecting(null);
    } else {
      console.log(`No output connection active, can't connect to input port`);
    }
  };

  const handleCanvasWheel = (e: WheelEvent) => {
    e.preventDefault();
    const delta = -e.deltaY * 0.001;
    const newZoom = Math.max(0.1, Math.min(3, zoom() + delta));
    setZoom(newZoom);
  };

  // Delete selected nodes
  const deleteSelectedNodes = async () => {
    const selection = Array.from(selectedNodes());
    if (selection.length === 0) return;

    for (const nodeId of selection) {
      await deleteNode(nodeId);
    }
  };

  // Copy selected nodes to clipboard
  const copySelectedNodes = () => {
    const selection = selectedNodes();
    if (selection.size === 0) return;

    const selectedNodeData = nodes().filter(n => selection.has(n.id));
    const selectedConnections = connections().filter(
      c => selection.has(c.from_node) && selection.has(c.to_node)
    );

    setClipboard({ nodes: selectedNodeData, connections: selectedConnections });
    console.log(`Copied ${selectedNodeData.length} nodes to clipboard`);
  };

  // Paste nodes from clipboard
  const pasteNodes = async () => {
    const clip = clipboard();
    if (!clip || clip.nodes.length === 0) return;

    try {
      // Duplicate nodes via backend
      const nodeUids = clip.nodes.map(n => n.id);
      const result = await invoke<{ original_uid: number; new_uid: number }[]>('duplicate_nodes', { nodeUids });

      if (result.length > 0) {
        // Refresh the graph to see new nodes
        await refreshGraph();

        // Select the newly created nodes
        const newNodeIds = new Set(result.map(r => r.new_uid));
        setSelectedNodes(newNodeIds);

        console.log(`Pasted ${result.length} nodes`);
      }
    } catch (error) {
      console.error('Failed to paste nodes:', error);
    }
  };

  // Cut selected nodes (copy + delete)
  const cutSelectedNodes = async () => {
    copySelectedNodes();
    await deleteSelectedNodes();
  };

  // Duplicate selected nodes (directly duplicate without using clipboard)
  const duplicateSelectedNodes = async () => {
    const selection = selectedNodes();
    if (selection.size === 0) return;

    try {
      const nodeUids = Array.from(selection);
      const result = await invoke<{ original_uid: number; new_uid: number }[]>('duplicate_nodes', { nodeUids });

      if (result.length > 0) {
        await refreshGraph();
        const newNodeIds = new Set(result.map(r => r.new_uid));
        setSelectedNodes(newNodeIds);
        console.log(`Duplicated ${result.length} nodes`);
      }
    } catch (error) {
      console.error('Failed to duplicate nodes:', error);
    }
  };

  // Select all nodes
  const selectAllNodes = () => {
    setSelectedNodes(new Set(nodes().map(n => n.id)));
  };

  // Keyboard event handler
  const handleKeyDown = (e: KeyboardEvent) => {
    // Don't handle if typing in an input field
    if (e.target instanceof HTMLInputElement || e.target instanceof HTMLTextAreaElement) {
      return;
    }

    const isMac = navigator.platform.toUpperCase().indexOf('MAC') >= 0;
    const modKey = isMac ? e.metaKey : e.ctrlKey;

    if (e.key === 'Delete' || e.key === 'Backspace') {
      e.preventDefault();
      deleteSelectedNodes();
    } else if (modKey && e.key === 'c') {
      e.preventDefault();
      copySelectedNodes();
    } else if (modKey && e.key === 'v') {
      e.preventDefault();
      pasteNodes();
    } else if (modKey && e.key === 'x') {
      e.preventDefault();
      cutSelectedNodes();
    } else if (modKey && e.key === 'd') {
      e.preventDefault();
      duplicateSelectedNodes();
    } else if (modKey && e.key === 'a') {
      e.preventDefault();
      selectAllNodes();
    }
  };

  onMount(() => {
    refreshGraph();

    // Provide handle to parent
    if (props.onReady) {
      props.onReady({
        refresh: refreshGraph
      });
    }

    // Prevent context menu
    canvasRef?.addEventListener('contextmenu', (e) => e.preventDefault());

    // Add keyboard event listener
    window.addEventListener('keydown', handleKeyDown);
  });

  // Cleanup on unmount
  onCleanup(() => {
    window.removeEventListener('keydown', handleKeyDown);
  });

  const getSelectedNodeData = () => {
    const selection = selectedNodes();
    if (selection.size === 0) return null;
    const firstSelected = Array.from(selection)[0];
    return nodes().find(n => n.id === firstSelected);
  };

  return (
    <div class="node-graph">
      {/* Breadcrumb Navigation */}
      <div class="node-graph-breadcrumb">
        <For each={breadcrumb()}>
          {(item, index) => (
            <>
              <button
                class="breadcrumb-item"
                onClick={() => navigateToBreadcrumb(index())}
              >
                {item.label}
              </button>
              <Show when={index() < breadcrumb().length - 1}>
                <span class="breadcrumb-separator">/</span>
              </Show>
            </>
          )}
        </For>
      </div>

      <div class="node-graph-toolbar">
        <button onClick={refreshGraph}>Refresh</button>
        <button
          onClick={toggleOutputLock}
          class={outputLocked() ? 'active' : ''}
          title={outputLocked() ? 'Output locked - click to unlock' : 'Output unlocked - auto-updates on selection'}
        >
          {outputLocked() ? '🔒 Locked' : '🔓 Auto'}
        </button>
        <span class="zoom-indicator">Zoom: {(zoom() * 100).toFixed(0)}%</span>
      </div>

      <div class="node-graph-content">
        <div
          ref={canvasRef}
          class="node-graph-canvas"
          onMouseDown={handleCanvasMouseDown}
          onMouseMove={handleCanvasMouseMove}
          onMouseUp={handleCanvasMouseUp}
          onWheel={handleCanvasWheel}
        >
          {/* Transform container - single transform for all nodes */}
          <div
            class="node-graph-transform"
            style={{ transform: getCanvasTransform() }}
          >
            {/* Connection lines - in world space */}
            <svg class="connections-layer">
              <For each={connections()}>
                {(conn) => {
                  // Use a function to make the path reactive to node position changes
                  const getPath = () => {
                    const fromNode = nodes().find(n => n.id === conn.from_node);
                    const toNode = nodes().find(n => n.id === conn.to_node);
                    if (!fromNode || !toNode) return '';

                    // Get ports for each node
                    const fromPorts = nodePorts().get(fromNode.id) || [];
                    const toPorts = nodePorts().get(toNode.id) || [];
                    const fromOutputPorts = fromPorts.filter(p => p.port_type === 'output');
                    const toInputPorts = toPorts.filter(p => p.port_type === 'input');

                    // Port dimensions: 12px width + 2px border = 14px, gap = 8px
                    const portSize = 14;
                    const portGap = 8;
                    const nodeWidth = 100;

                    // Calculate port X position based on port name and index
                    const getPortX = (ports: PortInfo[], portName: string | undefined, nodeX: number): number => {
                      if (!portName || ports.length === 0) {
                        return nodeX + nodeWidth / 2; // Default to center
                      }
                      const portIndex = ports.findIndex(p => p.name === portName);
                      if (portIndex === -1) {
                        return nodeX + nodeWidth / 2; // Port not found, use center
                      }
                      const totalWidth = ports.length * portSize + (ports.length - 1) * portGap;
                      const startX = nodeX + nodeWidth / 2 - totalWidth / 2;
                      return startX + portIndex * (portSize + portGap) + portSize / 2;
                    };

                    // Calculate positions
                    const fromX = getPortX(fromOutputPorts, conn.from_port, fromNode.x);
                    const fromY = fromNode.y + 50;  // Output ports at bottom
                    const toX = getPortX(toInputPorts, conn.to_port, toNode.x);
                    const toY = toNode.y;           // Input ports at top

                    const midY = (fromY + toY) / 2;

                    // Bezier curve going downward
                    return `M ${fromX} ${fromY} C ${fromX} ${midY}, ${toX} ${midY}, ${toX} ${toY}`;
                  };

                  return (
                    <path
                      d={getPath()}
                      stroke="#888"
                      stroke-width={2}
                      fill="none"
                      class="connection-line"
                      onContextMenu={(e) => {
                        e.preventDefault();
                        e.stopPropagation();
                        if (confirm(`Delete connection from node ${conn.from_node} to node ${conn.to_node}?`)) {
                          disconnectNodes(conn.from_node, conn.to_node);
                        }
                      }}
                    />
                  );
                }}
              </For>
            </svg>

            {/* Nodes - positioned in world space */}
            <For each={nodes()}>
              {(node) => {
                const isSelected = selectedNodes().has(node.id);
                const hasError = nodeErrors().has(node.id);
                const errorMessage = nodeErrors().get(node.id);
                const isActorNode = node.type === 'Actor';
                const isGeometryNode = ['Transform', 'Merge'].includes(node.type);
                const isPrimitiveNode = ['Cube', 'Sphere', 'Torus', 'Plane', 'Cylinder', 'Cone'].includes(node.type);

                // Get ports for this node
                const ports = nodePorts().get(node.id) || [];
                const inputPorts = ports.filter(p => p.port_type === 'input');
                const outputPorts = ports.filter(p => p.port_type === 'output');

                // Determine node class based on type
                let nodeTypeClass = '';
                if (isActorNode) {
                  nodeTypeClass = 'actor-node';
                } else if (isGeometryNode) {
                  nodeTypeClass = 'geometry-node';
                } else if (isPrimitiveNode) {
                  nodeTypeClass = 'primitive-node';
                }

                return (
                  <div
                    class={`node ${isSelected ? 'selected' : ''} ${nodeTypeClass} ${hasError ? 'error' : ''}`}
                    style={{
                      left: `${node.x}px`,
                      top: `${node.y}px`,
                    }}
                    title={hasError ? `Error: ${errorMessage}` : ''}
                    onMouseDown={(e) => handleNodeMouseDown(e, node.id)}
                    onDblClick={(e) => handleNodeDoubleClick(e, node.id)}
                    onContextMenu={(e) => handleNodeContextMenu(e, node.id)}
                  >
                    {/* Input ports - on the left side */}
                    <div class="input-ports">
                      <For each={inputPorts}>
                        {(port) => (
                          <div
                            class="port input-port"
                            style={{
                              'background-color': getPortColor(port.data_type),
                              'box-shadow': connecting() && connecting()!.isOutput
                                ? '0 0 12px rgba(74, 158, 255, 0.8)'
                                : undefined
                            }}
                            onMouseDown={(e) => handleInputPortClick(e, node.id, port.name)}
                            title={`${port.name} (${port.data_type})`}
                          >
                            <span class="port-label">{port.name}</span>
                          </div>
                        )}
                      </For>
                    </div>

                    <div class="node-header">{node.name}</div>
                    <div class="node-body">
                      {/* Body content can go here */}
                    </div>

                    {/* Output ports - on the right side */}
                    <div class="output-ports">
                      <For each={outputPorts}>
                        {(port) => (
                          <div
                            class="port output-port"
                            style={{
                              'background-color': getPortColor(port.data_type),
                              'box-shadow': connecting() && connecting()!.nodeId === node.id
                                ? '0 0 12px rgba(255, 158, 74, 0.8)'
                                : undefined
                            }}
                            onMouseDown={(e) => handleOutputPortClick(e, node.id, port.name)}
                            title={`${port.name} (${port.data_type})`}
                          >
                            <span class="port-label">{port.name}</span>
                          </div>
                        )}
                      </For>
                    </div>
                  </div>
                );
              }}
            </For>
          </div>

          {/* Node creation menu - fixed position (not affected by transform) */}
          <Show when={showNodeMenu()}>
            <div
              class="node-menu"
              style={{
                left: `${nodeMenuPos().x}px`,
                top: `${nodeMenuPos().y}px`,
              }}
              onMouseLeave={() => setShowNodeMenu(false)}
            >
              <div class="node-menu-header">
                Add Node {graphContext().level === 'scene' ? '(Scene)' : '(Geometry)'}
              </div>
              <For each={getAvailableNodeTypes()}>
                {(nodeType) => (
                  <button
                    class="node-menu-item"
                    onClick={() => createNode(nodeType.type)}
                  >
                    {nodeType.label}
                  </button>
                )}
              </For>
            </div>
          </Show>
        </div>

        {/* Properties Panel */}
        <Show when={selectedNodes().size > 0}>
          <div class="node-properties-panel">
            <Show when={getSelectedNodeData()} fallback={<div>No node selected</div>}>
              {(nodeData) => (
                <>
                  <div class="properties-header">
                    <h3>{nodeData().name}</h3>
                    <div class="properties-type">{nodeData().type}</div>
                  </div>
                  <div class="properties-content">
                    <Show when={nodeData().parameters && nodeData().parameters!.length > 0} fallback={<div class="no-parameters">No parameters</div>}>
                      <For each={nodeData().parameters}>
                        {(param) => (
                          <div class="parameter-row">
                            <label>
                              <span class="parameter-label">{param.name}</span>

                              {/* Float parameter */}
                              <Show when={param.value?.type === 'Float'}>
                                <input
                                  type="number"
                                  value={param.value?.value ?? 0}
                                  step="0.1"
                                  onChange={(e) => {
                                    const value = parseFloat(e.currentTarget.value);
                                    if (!isNaN(value)) {
                                      updateParameter(nodeData().id, param.name, { type: 'Float', value });
                                    }
                                  }}
                                />
                              </Show>

                              {/* Int parameter */}
                              <Show when={param.value?.type === 'Int'}>
                                <input
                                  type="number"
                                  value={param.value?.value ?? 0}
                                  step="1"
                                  onChange={(e) => {
                                    const value = parseInt(e.currentTarget.value);
                                    if (!isNaN(value)) {
                                      updateParameter(nodeData().id, param.name, { type: 'Int', value });
                                    }
                                  }}
                                />
                              </Show>

                              {/* Bool parameter */}
                              <Show when={param.value?.type === 'Bool'}>
                                <input
                                  type="checkbox"
                                  checked={param.value?.value ?? false}
                                  onChange={(e) => {
                                    updateParameter(nodeData().id, param.name, { type: 'Bool', value: e.currentTarget.checked });
                                  }}
                                />
                              </Show>

                              {/* String parameter */}
                              <Show when={param.value?.type === 'String'}>
                                <input
                                  type="text"
                                  value={param.value?.value ?? ''}
                                  onInput={(e) => {
                                    updateParameter(nodeData().id, param.name, { type: 'String', value: e.currentTarget.value });
                                  }}
                                />
                              </Show>

                              {/* Color parameter */}
                              <Show when={param.value?.type === 'Color'}>
                                <input
                                  type="color"
                                  value={param.value?.value ?? '#ffffff'}
                                  onInput={(e) => {
                                    updateParameter(nodeData().id, param.name, { type: 'Color', value: e.currentTarget.value });
                                  }}
                                />
                              </Show>

                              {/* Vec3 parameter */}
                              <Show when={param.value?.type === 'Vec3'}>
                                {(() => {
                                  const vec3 = getVec3Value(param.value);
                                  return (
                                    <div class="vec3-inputs">
                                      <input
                                        type="number"
                                        value={vec3?.x ?? 0}
                                        step="0.1"
                                        placeholder="X"
                                        onChange={(e) => {
                                          const x = parseFloat(e.currentTarget.value);
                                          if (!isNaN(x)) {
                                            updateParameter(nodeData().id, param.name, {
                                              type: 'Vec3',
                                              value: { x, y: vec3?.y ?? 0, z: vec3?.z ?? 0 }
                                            });
                                          }
                                        }}
                                      />
                                      <input
                                        type="number"
                                        value={vec3?.y ?? 0}
                                        step="0.1"
                                        placeholder="Y"
                                        onChange={(e) => {
                                          const y = parseFloat(e.currentTarget.value);
                                          if (!isNaN(y)) {
                                            updateParameter(nodeData().id, param.name, {
                                              type: 'Vec3',
                                              value: { x: vec3?.x ?? 0, y, z: vec3?.z ?? 0 }
                                            });
                                          }
                                        }}
                                      />
                                      <input
                                        type="number"
                                        value={vec3?.z ?? 0}
                                        step="0.1"
                                        placeholder="Z"
                                        onChange={(e) => {
                                          const z = parseFloat(e.currentTarget.value);
                                          if (!isNaN(z)) {
                                            updateParameter(nodeData().id, param.name, {
                                              type: 'Vec3',
                                              value: { x: vec3?.x ?? 0, y: vec3?.y ?? 0, z }
                                            });
                                          }
                                        }}
                                      />
                                    </div>
                                  );
                                })()}
                              </Show>

                              {/* Fallback for null/unknown parameter types - show raw value */}
                              <Show when={!param.value || !['Float', 'Int', 'Bool', 'String', 'Color', 'Vec3'].includes(param.value?.type)}>
                                <span class="parameter-value-unknown">
                                  {param.value ? `${param.value.type}: ${JSON.stringify(param.value.value ?? param.value)}` : 'null'}
                                </span>
                              </Show>
                            </label>
                          </div>
                        )}
                      </For>
                    </Show>
                  </div>
                </>
              )}
            </Show>
          </div>
        </Show>
      </div>
    </div>
  );
};
