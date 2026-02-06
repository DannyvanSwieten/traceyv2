//! Procedural node graph Tauri commands

use crate::ffi::{self, raw::TraceyNodeType, raw::TraceyResult, Transform};
use crate::scene::Actor;
use crate::AppState;
use serde::{Deserialize, Serialize};
use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use tauri::State;

/// Node type for creating nodes from the frontend
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum NodeType {
    Actor,
    Cube,
    Sphere,
    Torus,
    Plane,
    Cylinder,
    Cone,
    Transform,
    Merge,
    Material,
    MathFloat,
    MathVector,
}

impl From<NodeType> for TraceyNodeType {
    fn from(node_type: NodeType) -> Self {
        match node_type {
            NodeType::Actor => TraceyNodeType::Actor,
            NodeType::Cube => TraceyNodeType::PrimitiveCube,
            NodeType::Sphere => TraceyNodeType::PrimitiveSphere,
            NodeType::Torus => TraceyNodeType::PrimitiveTorus,
            NodeType::Plane => TraceyNodeType::PrimitivePlane,
            NodeType::Cylinder => TraceyNodeType::PrimitiveCylinder,
            NodeType::Cone => TraceyNodeType::PrimitiveCone,
            NodeType::Transform => TraceyNodeType::GeometryTransform,
            NodeType::Merge => TraceyNodeType::GeometryMerge,
            NodeType::Material => TraceyNodeType::Material,
            NodeType::MathFloat => TraceyNodeType::MathFloat,
            NodeType::MathVector => TraceyNodeType::MathVector,
        }
    }
}

/// Parameter value that can be set on nodes
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type", content = "value")]
pub enum ParameterValue {
    Float(f32),
    Vec3 { x: f32, y: f32, z: f32 },
    Int(i32),
    Bool(bool),
}

/// Connection between two nodes
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct NodeConnection {
    pub from_node: u64,
    pub to_node: u64,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub from_port: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub to_port: Option<String>,
}

/// Parameter information
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ParameterInfo {
    pub name: String,
    pub value: Option<ParameterValue>,
}

/// Node details including name, type, and parameters
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct NodeDetails {
    pub id: u64,
    pub name: String,
    pub node_type: String,
    pub parameters: Vec<ParameterInfo>,
}

/// Data type for port validation
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum DataType {
    Float,
    Vec2,
    Vec3,
    Vec4,
    Mat3,
    Mat4,
    Int,
    UInt,
    Bool,
    Sampler2D,
    Geometry,
    DataType,
    Scene3D,
}

/// Port type (input or output)
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum PortType {
    Input,
    Output,
}

/// Port information structure
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PortInfo {
    pub name: String,
    pub data_type: DataType,
    pub port_type: PortType,
}

/// Create a new node in the current graph (context-aware with validation)
#[tauri::command]
pub async fn create_node(
    state: State<'_, AppState>,
    node_type: NodeType,
    name: String,
) -> Result<u64, String> {
    // Check if we need to create a corresponding Actor (before node_type is moved)
    let is_scene_actor_node;
    let node_uid: u64;
    let actor_name = name.clone();

    {
        let context = state
            .current_graph_context
            .lock()
            .map_err(|_| "Failed to lock graph context")?;

        // Validate node type for current context
        match context.level {
            crate::GraphLevel::Scene => {
                // Scene level: only Actor nodes allowed
                if !matches!(node_type, NodeType::Actor) {
                    return Err(format!(
                        "Cannot create {:?} node at scene level. Only Actor nodes are allowed here.",
                        node_type
                    ));
                }
            }
            crate::GraphLevel::GeometryNetwork => {
                // Geometry network: only SOP nodes allowed (no Actor nodes)
                if matches!(node_type, NodeType::Actor) {
                    return Err(
                        "Cannot create Actor node in geometry network. Actor nodes belong at scene level."
                            .to_string(),
                    );
                }
            }
        }

        is_scene_actor_node = matches!(node_type, NodeType::Actor) && context.level == crate::GraphLevel::Scene;

        let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
        let scene_ptr = engine.get_scene_ptr().ok_or("Failed to get scene pointer")?;
        let name_c = CString::new(name).map_err(|e| format!("Invalid name: {}", e))?;

        node_uid = unsafe {
            // Get the appropriate graph based on context
            let graph = match context.level {
                crate::GraphLevel::Scene => {
                    // Create in scene-level graph
                    ffi::raw::tracey_scene_get_node_graph(scene_ptr)
                }
                crate::GraphLevel::GeometryNetwork => {
                    // Create in nested geometry network
                    let actor_node_uid = context
                        .actor_node_uid
                        .ok_or("No actor node UID in geometry network context")?;

                    let scene_graph = ffi::raw::tracey_scene_get_node_graph(scene_ptr);
                    if scene_graph.is_null() {
                        return Err("Failed to get scene graph".to_string());
                    }

                    let node = ffi::raw::tracey_node_graph_get_node(scene_graph, actor_node_uid);
                    if node.is_null() {
                        return Err("ActorNode not found".to_string());
                    }

                    ffi::raw::tracey_actor_node_get_geometry_network(node)
                }
            };

            if graph.is_null() {
                return Err("Failed to get node graph".to_string());
            }

            let node_type_c: TraceyNodeType = node_type.into();
            let uid = ffi::raw::tracey_node_graph_create_node(graph, node_type_c, name_c.as_ptr());

            if uid == u64::MAX {
                let error = std::ffi::CStr::from_ptr(ffi::raw::tracey_get_last_error())
                    .to_string_lossy()
                    .into_owned();
                return Err(format!("Failed to create node: {}", error));
            }

            // If we created an ActorNode at scene level, also create a corresponding Actor in the C++ scene
            if is_scene_actor_node {
                // Create an Actor with the same UID as the ActorNode for synchronization
                let result = ffi::raw::tracey_scene_create_actor_with_uid(scene_ptr, uid, name_c.as_ptr());

                // Verify the actor was created successfully
                if result != TraceyResult::Success {
                    // Log warning but don't fail - the node was created successfully
                    eprintln!("Warning: Failed to create corresponding Actor for ActorNode {}", uid);
                }
            }

            uid
        };
    } // Release context and engine locks

    // If we created an ActorNode at scene level, also add to Rust SceneState
    if is_scene_actor_node {
        let mut scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;

        // Create Actor in Rust SceneState with the same UID (parented to root)
        let actor = Actor {
            id: node_uid,
            name: actor_name,
            transform: Transform::identity(),
            children: Vec::new(),
            parent: Some(0), // Parent to root
            instances: Vec::new(),
        };
        scene.actors.insert(node_uid, actor);

        // Add to root's children list
        if let Some(root) = scene.actors.get_mut(&0) {
            root.children.push(node_uid);
        }
    }

    Ok(node_uid)
}

/// Remove a node from the node graph (context-aware)
#[tauri::command]
pub async fn remove_node(state: State<'_, AppState>, node_uid: u64) -> Result<(), String> {
    let context = state
        .current_graph_context
        .lock()
        .map_err(|_| "Failed to lock graph context")?;

    let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
    let scene_ptr = engine.get_scene_ptr().ok_or("Failed to get scene pointer")?;

    unsafe {
        // Get the appropriate graph based on context
        let graph = match context.level {
            crate::GraphLevel::Scene => {
                ffi::raw::tracey_scene_get_node_graph(scene_ptr)
            }
            crate::GraphLevel::GeometryNetwork => {
                let actor_node_uid = context
                    .actor_node_uid
                    .ok_or("No actor node UID in geometry network context")?;

                let scene_graph = ffi::raw::tracey_scene_get_node_graph(scene_ptr);
                if scene_graph.is_null() {
                    return Err("Failed to get scene graph".to_string());
                }

                let node = ffi::raw::tracey_node_graph_get_node(scene_graph, actor_node_uid);
                if node.is_null() {
                    return Err("ActorNode not found".to_string());
                }

                ffi::raw::tracey_actor_node_get_geometry_network(node)
            }
        };

        if graph.is_null() {
            return Err("Failed to get node graph".to_string());
        }

        let result = ffi::raw::tracey_node_graph_remove_node(graph, node_uid);
        if result != TraceyResult::Success {
            let error = std::ffi::CStr::from_ptr(ffi::raw::tracey_get_last_error())
                .to_string_lossy()
                .into_owned();
            return Err(format!("Failed to remove node: {}", error));
        }

        Ok(())
    }
}

/// Set a parameter value on a node (context-aware)
/// Note: Does NOT auto-evaluate - frontend handles evaluation via debounced evaluateGraph()
#[tauri::command]
pub async fn set_node_parameter(
    state: State<'_, AppState>,
    node_uid: u64,
    param_name: String,
    value: ParameterValue,
) -> Result<(), String> {
    let context = state
        .current_graph_context
        .lock()
        .map_err(|_| "Failed to lock graph context")?;

    let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
    let scene_ptr = engine.get_scene_ptr().ok_or("Failed to get scene pointer")?;

    let param_name_c = CString::new(param_name).map_err(|e| format!("Invalid parameter name: {}", e))?;

    unsafe {
        // Get the scene graph (always needed for evaluation)
        let scene_graph = ffi::raw::tracey_scene_get_node_graph(scene_ptr);
        if scene_graph.is_null() {
            return Err("Failed to get scene graph".to_string());
        }

        // Get the appropriate graph based on context for parameter setting
        let graph = match context.level {
            crate::GraphLevel::Scene => scene_graph,
            crate::GraphLevel::GeometryNetwork => {
                let actor_node_uid = context
                    .actor_node_uid
                    .ok_or("No actor node UID in geometry network context")?;

                let node = ffi::raw::tracey_node_graph_get_node(scene_graph, actor_node_uid);
                if node.is_null() {
                    return Err("ActorNode not found".to_string());
                }

                ffi::raw::tracey_actor_node_get_geometry_network(node)
            }
        };

        if graph.is_null() {
            return Err("Failed to get node graph".to_string());
        }

        let node = ffi::raw::tracey_node_graph_get_node(graph, node_uid);
        if node.is_null() {
            return Err("Node not found".to_string());
        }

        let param = ffi::raw::tracey_node_get_parameter(node, param_name_c.as_ptr());
        if param.is_null() {
            return Err("Parameter not found".to_string());
        }

        let result = match value {
            ParameterValue::Float(v) => ffi::raw::tracey_parameter_set_float(param, v),
            ParameterValue::Vec3 { x, y, z } => {
                let vec3 = ffi::raw::TraceyVec3 { x, y, z };
                ffi::raw::tracey_parameter_set_vec3(param, &vec3)
            }
            ParameterValue::Int(v) => ffi::raw::tracey_parameter_set_int(param, v),
            ParameterValue::Bool(v) => ffi::raw::tracey_parameter_set_bool(param, v),
        };

        if result != TraceyResult::Success {
            let error = std::ffi::CStr::from_ptr(ffi::raw::tracey_get_last_error())
                .to_string_lossy()
                .into_owned();
            return Err(format!("Failed to set parameter: {}", error));
        }

        // NOTE: We DON'T auto-evaluate here anymore. The frontend handles evaluation
        // via debounced evaluateGraph() to batch parameter changes and prevent
        // unnecessary scene recompilations that reset material state.
        // The parameter value is set, but graph evaluation is deferred to the frontend.
    }

    Ok(())
}

/// Connect two nodes together (context-aware)
#[tauri::command]
pub async fn connect_nodes(
    state: State<'_, AppState>,
    from_node: u64,
    from_port: String,
    to_node: u64,
    to_port: String,
) -> Result<(), String> {
    let context = state
        .current_graph_context
        .lock()
        .map_err(|_| "Failed to lock graph context")?;

    let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
    let scene_ptr = engine.get_scene_ptr().ok_or("Failed to get scene pointer")?;

    let from_port_c = CString::new(from_port).map_err(|e| format!("Invalid from_port: {}", e))?;
    let to_port_c = CString::new(to_port).map_err(|e| format!("Invalid to_port: {}", e))?;

    unsafe {
        // Get the appropriate graph based on context
        let graph = match context.level {
            crate::GraphLevel::Scene => {
                ffi::raw::tracey_scene_get_node_graph(scene_ptr)
            }
            crate::GraphLevel::GeometryNetwork => {
                let actor_node_uid = context
                    .actor_node_uid
                    .ok_or("No actor node UID in geometry network context")?;

                let scene_graph = ffi::raw::tracey_scene_get_node_graph(scene_ptr);
                if scene_graph.is_null() {
                    return Err("Failed to get scene graph".to_string());
                }

                let node = ffi::raw::tracey_node_graph_get_node(scene_graph, actor_node_uid);
                if node.is_null() {
                    return Err("ActorNode not found".to_string());
                }

                ffi::raw::tracey_actor_node_get_geometry_network(node)
            }
        };

        if graph.is_null() {
            return Err("Failed to get node graph".to_string());
        }

        let result = ffi::raw::tracey_node_graph_connect(
            graph,
            from_node,
            from_port_c.as_ptr(),
            to_node,
            to_port_c.as_ptr(),
        );

        if result != TraceyResult::Success {
            let error = std::ffi::CStr::from_ptr(ffi::raw::tracey_get_last_error())
                .to_string_lossy()
                .into_owned();
            return Err(format!("Failed to connect nodes: {}", error));
        }

        Ok(())
    }
}

/// Disconnect two nodes (context-aware)
#[tauri::command]
pub async fn disconnect_nodes(
    state: State<'_, AppState>,
    from_node: u64,
    to_node: u64,
) -> Result<(), String> {
    let context = state
        .current_graph_context
        .lock()
        .map_err(|_| "Failed to lock graph context")?;

    let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
    let scene_ptr = engine.get_scene_ptr().ok_or("Failed to get scene pointer")?;

    unsafe {
        // Get the appropriate graph based on context
        let graph = match context.level {
            crate::GraphLevel::Scene => {
                ffi::raw::tracey_scene_get_node_graph(scene_ptr)
            }
            crate::GraphLevel::GeometryNetwork => {
                let actor_node_uid = context
                    .actor_node_uid
                    .ok_or("No actor node UID in geometry network context")?;

                let scene_graph = ffi::raw::tracey_scene_get_node_graph(scene_ptr);
                if scene_graph.is_null() {
                    return Err("Failed to get scene graph".to_string());
                }

                let node = ffi::raw::tracey_node_graph_get_node(scene_graph, actor_node_uid);
                if node.is_null() {
                    return Err("ActorNode not found".to_string());
                }

                ffi::raw::tracey_actor_node_get_geometry_network(node)
            }
        };

        if graph.is_null() {
            return Err("Failed to get node graph".to_string());
        }

        let result = ffi::raw::tracey_node_graph_disconnect(graph, from_node, to_node);

        if result != TraceyResult::Success {
            let error = std::ffi::CStr::from_ptr(ffi::raw::tracey_get_last_error())
                .to_string_lossy()
                .into_owned();
            return Err(format!("Failed to disconnect nodes: {}", error));
        }

        Ok(())
    }
}

/// Set a node as an output of the graph (context-aware)
#[tauri::command]
pub async fn set_graph_output(
    state: State<'_, AppState>,
    output_name: String,
    node_uid: u64,
) -> Result<(), String> {
    let context = state
        .current_graph_context
        .lock()
        .map_err(|_| "Failed to lock graph context")?;

    let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
    let scene_ptr = engine.get_scene_ptr().ok_or("Failed to get scene pointer")?;

    let output_name_c = CString::new(output_name).map_err(|e| format!("Invalid output name: {}", e))?;

    unsafe {
        // Get the appropriate graph based on context
        let graph = match context.level {
            crate::GraphLevel::Scene => {
                ffi::raw::tracey_scene_get_node_graph(scene_ptr)
            }
            crate::GraphLevel::GeometryNetwork => {
                let actor_node_uid = context
                    .actor_node_uid
                    .ok_or("No actor node UID in geometry network context")?;

                let scene_graph = ffi::raw::tracey_scene_get_node_graph(scene_ptr);
                if scene_graph.is_null() {
                    return Err("Failed to get scene graph".to_string());
                }

                let node = ffi::raw::tracey_node_graph_get_node(scene_graph, actor_node_uid);
                if node.is_null() {
                    return Err("ActorNode not found".to_string());
                }

                ffi::raw::tracey_actor_node_get_geometry_network(node)
            }
        };

        if graph.is_null() {
            return Err("Failed to get node graph".to_string());
        }

        let result = ffi::raw::tracey_node_graph_set_output(graph, output_name_c.as_ptr(), node_uid);

        if result != TraceyResult::Success {
            let error = std::ffi::CStr::from_ptr(ffi::raw::tracey_get_last_error())
                .to_string_lossy()
                .into_owned();
            return Err(format!("Failed to set graph output: {}", error));
        }

        Ok(())
    }
}

/// Evaluate the node graph
#[tauri::command]
pub async fn evaluate_graph(
    state: State<'_, AppState>,
    time: f64,
    frame: u32,
) -> Result<(), String> {
    let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;

    let scene_ptr = engine.get_scene_ptr().ok_or("Failed to get scene pointer")?;

    unsafe {
        let graph = ffi::raw::tracey_scene_get_node_graph(scene_ptr);
        if graph.is_null() {
            return Err("Failed to get node graph".to_string());
        }

        let result = ffi::raw::tracey_node_graph_evaluate(graph, time, frame);

        if result != TraceyResult::Success {
            let error = std::ffi::CStr::from_ptr(ffi::raw::tracey_get_last_error())
                .to_string_lossy()
                .into_owned();
            return Err(format!("Failed to evaluate graph: {}", error));
        }

        // Sync the node graph output to the scene
        let sync_result = ffi::raw::tracey_scene_sync_from_node_graph(scene_ptr);
        if sync_result != TraceyResult::Success {
            let error = std::ffi::CStr::from_ptr(ffi::raw::tracey_get_last_error())
                .to_string_lossy()
                .into_owned();
            return Err(format!("Failed to sync node graph to scene: {}", error));
        }

        Ok(())
    }
}

/// Get all nodes in the current graph (context-aware: scene or geometry network)
#[tauri::command]
pub async fn get_all_nodes(state: State<'_, AppState>) -> Result<Vec<u64>, String> {
    let context = state
        .current_graph_context
        .lock()
        .map_err(|_| "Failed to lock graph context")?;

    let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
    let scene_ptr = engine.get_scene_ptr().ok_or("Failed to get scene pointer")?;

    unsafe {
        // Get the appropriate graph based on context
        let graph = match context.level {
            crate::GraphLevel::Scene => {
                // Query scene-level graph
                ffi::raw::tracey_scene_get_node_graph(scene_ptr)
            }
            crate::GraphLevel::GeometryNetwork => {
                // Query nested geometry network
                let actor_node_uid = context
                    .actor_node_uid
                    .ok_or("No actor node UID in geometry network context")?;

                let scene_graph = ffi::raw::tracey_scene_get_node_graph(scene_ptr);
                if scene_graph.is_null() {
                    return Err("Failed to get scene graph".to_string());
                }

                let node = ffi::raw::tracey_node_graph_get_node(scene_graph, actor_node_uid);
                if node.is_null() {
                    return Err("ActorNode not found".to_string());
                }

                ffi::raw::tracey_actor_node_get_geometry_network(node)
            }
        };

        if graph.is_null() {
            return Err("Failed to get node graph".to_string());
        }

        let node_count = ffi::raw::tracey_node_graph_get_node_count(graph);
        if node_count == 0 {
            return Ok(Vec::new());
        }

        let mut node_uids = vec![0u64; node_count as usize];
        let actual_count = ffi::raw::tracey_node_graph_get_all_nodes(
            graph,
            node_uids.as_mut_ptr(),
            node_count,
        );

        node_uids.truncate(actual_count as usize);
        Ok(node_uids)
    }
}

/// Get all connections in the current graph (context-aware)
#[tauri::command]
pub async fn get_all_connections(state: State<'_, AppState>) -> Result<Vec<NodeConnection>, String> {
    let context = state
        .current_graph_context
        .lock()
        .map_err(|_| "Failed to lock graph context")?;

    let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
    let scene_ptr = engine.get_scene_ptr().ok_or("Failed to get scene pointer")?;

    unsafe {
        // Get the appropriate graph based on context
        let graph = match context.level {
            crate::GraphLevel::Scene => {
                ffi::raw::tracey_scene_get_node_graph(scene_ptr)
            }
            crate::GraphLevel::GeometryNetwork => {
                let actor_node_uid = context
                    .actor_node_uid
                    .ok_or("No actor node UID in geometry network context")?;

                let scene_graph = ffi::raw::tracey_scene_get_node_graph(scene_ptr);
                if scene_graph.is_null() {
                    return Err("Failed to get scene graph".to_string());
                }

                let node = ffi::raw::tracey_node_graph_get_node(scene_graph, actor_node_uid);
                if node.is_null() {
                    return Err("ActorNode not found".to_string());
                }

                ffi::raw::tracey_actor_node_get_geometry_network(node)
            }
        };

        if graph.is_null() {
            return Err("Failed to get node graph".to_string());
        }

        let connection_count = ffi::raw::tracey_node_graph_get_connection_count(graph);
        if connection_count == 0 {
            return Ok(Vec::new());
        }

        let mut connections = Vec::with_capacity(connection_count as usize);
        for i in 0..connection_count {
            let mut from_node = 0u64;
            let mut to_node = 0u64;
            let mut from_port_ptr: *const c_char = std::ptr::null();
            let mut to_port_ptr: *const c_char = std::ptr::null();

            let result = ffi::raw::tracey_node_graph_get_connection(
                graph,
                i,
                &mut from_node,
                &mut to_node,
                &mut from_port_ptr,
                &mut to_port_ptr,
            );

            if result == TraceyResult::Success {
                // Convert C strings to Rust strings
                let from_port = if !from_port_ptr.is_null() {
                    Some(CStr::from_ptr(from_port_ptr).to_string_lossy().to_string())
                } else {
                    None
                };
                let to_port = if !to_port_ptr.is_null() {
                    Some(CStr::from_ptr(to_port_ptr).to_string_lossy().to_string())
                } else {
                    None
                };

                connections.push(NodeConnection {
                    from_node,
                    to_node,
                    from_port,
                    to_port,
                });
            }
        }

        Ok(connections)
    }
}

/// Get details about a specific node (name, type, parameters) - context-aware
#[tauri::command]
pub async fn get_node_details(
    state: State<'_, AppState>,
    node_uid: u64,
) -> Result<NodeDetails, String> {
    let context = state
        .current_graph_context
        .lock()
        .map_err(|_| "Failed to lock graph context")?;

    let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
    let scene_ptr = engine.get_scene_ptr().ok_or("Failed to get scene pointer")?;

    unsafe {
        // Get the appropriate graph based on context
        let graph = match context.level {
            crate::GraphLevel::Scene => {
                ffi::raw::tracey_scene_get_node_graph(scene_ptr)
            }
            crate::GraphLevel::GeometryNetwork => {
                let actor_node_uid = context
                    .actor_node_uid
                    .ok_or("No actor node UID in geometry network context")?;

                let scene_graph = ffi::raw::tracey_scene_get_node_graph(scene_ptr);
                if scene_graph.is_null() {
                    return Err("Failed to get scene graph".to_string());
                }

                let node = ffi::raw::tracey_node_graph_get_node(scene_graph, actor_node_uid);
                if node.is_null() {
                    return Err("ActorNode not found".to_string());
                }

                ffi::raw::tracey_actor_node_get_geometry_network(node)
            }
        };

        if graph.is_null() {
            return Err("Failed to get node graph".to_string());
        }

        let node = ffi::raw::tracey_node_graph_get_node(graph, node_uid);
        if node.is_null() {
            return Err("Node not found".to_string());
        }

        // Get node name
        let name_ptr = ffi::raw::tracey_node_get_name(node);
        let name = if !name_ptr.is_null() {
            std::ffi::CStr::from_ptr(name_ptr)
                .to_string_lossy()
                .into_owned()
        } else {
            format!("Node {}", node_uid)
        };

        // Get node type
        let node_type_enum = ffi::raw::tracey_node_get_type(node);
        let node_type = match node_type_enum {
            TraceyNodeType::Actor => "Actor",
            TraceyNodeType::PrimitiveCube => "Cube",
            TraceyNodeType::PrimitiveSphere => "Sphere",
            TraceyNodeType::PrimitiveTorus => "Torus",
            TraceyNodeType::PrimitivePlane => "Plane",
            TraceyNodeType::PrimitiveCylinder => "Cylinder",
            TraceyNodeType::PrimitiveCone => "Cone",
            TraceyNodeType::GeometryTransform => "Transform",
            TraceyNodeType::GeometryMerge => "Merge",
            TraceyNodeType::Material => "Material",
            TraceyNodeType::MathFloat => "MathFloat",
            TraceyNodeType::MathVector => "MathVector",
            _ => "Unknown",
        }.to_string();

        // Get parameters
        let param_count = ffi::raw::tracey_node_get_parameter_count(node);
        let mut parameters = Vec::new();

        for i in 0..param_count {
            let param_name_ptr = ffi::raw::tracey_node_get_parameter_name(node, i);
            if !param_name_ptr.is_null() {
                let param_name = std::ffi::CStr::from_ptr(param_name_ptr)
                    .to_string_lossy()
                    .into_owned();

                let param_name_c = CString::new(param_name.clone())
                    .map_err(|e| format!("Invalid parameter name: {}", e))?;
                let param = ffi::raw::tracey_node_get_parameter(node, param_name_c.as_ptr());

                let value = if !param.is_null() {
                    // Try Float first
                    let mut float_value: f32 = 0.0;
                    let float_result = ffi::raw::tracey_parameter_get_float(param, &mut float_value);
                    if float_result == TraceyResult::Success {
                        Some(ParameterValue::Float(float_value))
                    } else {
                        // Try Int
                        let mut int_value: i32 = 0;
                        let int_result = ffi::raw::tracey_parameter_get_int(param, &mut int_value);
                        if int_result == TraceyResult::Success {
                            Some(ParameterValue::Int(int_value))
                        } else {
                            // Try Bool
                            let mut bool_value: bool = false;
                            let bool_result = ffi::raw::tracey_parameter_get_bool(param, &mut bool_value);
                            if bool_result == TraceyResult::Success {
                                Some(ParameterValue::Bool(bool_value))
                            } else {
                                // Try Vec3
                                let mut vec3_value = ffi::raw::TraceyVec3 { x: 0.0, y: 0.0, z: 0.0 };
                                let vec3_result = ffi::raw::tracey_parameter_get_vec3(param, &mut vec3_value);
                                if vec3_result == TraceyResult::Success {
                                    Some(ParameterValue::Vec3 {
                                        x: vec3_value.x,
                                        y: vec3_value.y,
                                        z: vec3_value.z
                                    })
                                } else {
                                    // Unknown parameter type
                                    None
                                }
                            }
                        }
                    }
                } else {
                    None
                };

                parameters.push(ParameterInfo {
                    name: param_name,
                    value,
                });
            }
        }

        Ok(NodeDetails {
            id: node_uid,
            name,
            node_type,
            parameters,
        })
    }
}

// ============================================================================
// Phase 2: Nested Graph Navigation Commands
// ============================================================================

/// Navigate into an ActorNode's geometry network
#[tauri::command]
pub async fn navigate_to_actor_node(
    state: State<'_, AppState>,
    actor_node_uid: u64,
) -> Result<(), String> {
    let mut context = state
        .current_graph_context
        .lock()
        .map_err(|_| "Failed to lock graph context")?;

    let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
    let scene_ptr = engine.get_scene_ptr().ok_or("Failed to get scene pointer")?;

    unsafe {
        let graph = ffi::raw::tracey_scene_get_node_graph(scene_ptr);
        if graph.is_null() {
            return Err("Failed to get node graph".to_string());
        }

        let node = ffi::raw::tracey_node_graph_get_node(graph, actor_node_uid);
        if node.is_null() {
            return Err("ActorNode not found".to_string());
        }

        // Verify this is an ActorNode
        let node_type = ffi::raw::tracey_node_get_type(node);
        if node_type != TraceyNodeType::Actor {
            return Err("Node is not an ActorNode".to_string());
        }

        // Get the geometry network
        let geometry_network = ffi::raw::tracey_actor_node_get_geometry_network(node);
        if geometry_network.is_null() {
            return Err("Failed to get geometry network".to_string());
        }

        // Update context
        context.level = crate::GraphLevel::GeometryNetwork;
        context.actor_node_uid = Some(actor_node_uid);

        Ok(())
    }
}

/// Navigate back to the scene-level graph
#[tauri::command]
pub async fn navigate_to_scene_graph(state: State<'_, AppState>) -> Result<(), String> {
    let mut context = state
        .current_graph_context
        .lock()
        .map_err(|_| "Failed to lock graph context")?;

    // Reset to scene level
    context.level = crate::GraphLevel::Scene;
    context.actor_node_uid = None;

    Ok(())
}

/// Get the current graph context (level and actor node UID if in geometry network)
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GraphContextInfo {
    pub level: String,
    pub actor_node_uid: Option<u64>,
}

#[tauri::command]
pub async fn get_graph_context(state: State<'_, AppState>) -> Result<GraphContextInfo, String> {
    let context = state
        .current_graph_context
        .lock()
        .map_err(|_| "Failed to lock graph context")?;

    Ok(GraphContextInfo {
        level: match context.level {
            crate::GraphLevel::Scene => "scene".to_string(),
            crate::GraphLevel::GeometryNetwork => "geometry_network".to_string(),
        },
        actor_node_uid: context.actor_node_uid,
    })
}

// ============================================================================
// Add Object Menu convenience command
// ============================================================================

/// Parameters for primitive creation
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type")]
pub enum PrimitiveType {
    #[serde(rename = "cube")]
    Cube { size: Option<f32> },
    #[serde(rename = "sphere")]
    Sphere {
        radius: Option<f32>,
        segments: Option<u32>,
        rings: Option<u32>,
    },
    #[serde(rename = "torus")]
    Torus {
        major_radius: Option<f32>,
        minor_radius: Option<f32>,
        major_segments: Option<u32>,
        minor_segments: Option<u32>,
    },
    #[serde(rename = "plane")]
    Plane {
        width: Option<f32>,
        depth: Option<f32>,
    },
    #[serde(rename = "cylinder")]
    Cylinder {
        radius: Option<f32>,
        height: Option<f32>,
        segments: Option<u32>,
    },
    #[serde(rename = "cone")]
    Cone {
        radius: Option<f32>,
        height: Option<f32>,
        segments: Option<u32>,
    },
}

impl PrimitiveType {
    fn node_type(&self) -> TraceyNodeType {
        match self {
            PrimitiveType::Cube { .. } => TraceyNodeType::PrimitiveCube,
            PrimitiveType::Sphere { .. } => TraceyNodeType::PrimitiveSphere,
            PrimitiveType::Torus { .. } => TraceyNodeType::PrimitiveTorus,
            PrimitiveType::Plane { .. } => TraceyNodeType::PrimitivePlane,
            PrimitiveType::Cylinder { .. } => TraceyNodeType::PrimitiveCylinder,
            PrimitiveType::Cone { .. } => TraceyNodeType::PrimitiveCone,
        }
    }

    fn type_name(&self) -> &'static str {
        match self {
            PrimitiveType::Cube { .. } => "Cube",
            PrimitiveType::Sphere { .. } => "Sphere",
            PrimitiveType::Torus { .. } => "Torus",
            PrimitiveType::Plane { .. } => "Plane",
            PrimitiveType::Cylinder { .. } => "Cylinder",
            PrimitiveType::Cone { .. } => "Cone",
        }
    }
}

/// Add a primitive object via the node graph system
/// This creates an ActorNode with a geometry network containing the specified primitive
#[tauri::command]
pub async fn add_primitive_via_nodes(
    state: State<'_, AppState>,
    name: String,
    primitive: PrimitiveType,
) -> Result<Actor, String> {
    let actor_uid: u64;
    let geom_node_uid: u64;
    let actor_name = name.clone();

    {
        let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
        let scene_ptr = engine.get_scene_ptr().ok_or("Failed to get scene pointer")?;

        let name_c = CString::new(name.clone()).map_err(|e| format!("Invalid name: {}", e))?;
        let geom_name = format!("{}_geometry", primitive.type_name());
        let geom_name_c = CString::new(geom_name).map_err(|e| format!("Invalid name: {}", e))?;
        let output_name_c = CString::new("geometry").map_err(|e| format!("Invalid output name: {}", e))?;

        unsafe {
            // Step 1: Get scene-level graph
            let scene_graph = ffi::raw::tracey_scene_get_node_graph(scene_ptr);
            if scene_graph.is_null() {
                return Err("Failed to get scene graph".to_string());
            }

            // Step 2: Create ActorNode at scene level
            actor_uid = ffi::raw::tracey_node_graph_create_node(
                scene_graph,
                TraceyNodeType::Actor,
                name_c.as_ptr(),
            );
            if actor_uid == u64::MAX {
                let error = std::ffi::CStr::from_ptr(ffi::raw::tracey_get_last_error())
                    .to_string_lossy()
                    .into_owned();
                return Err(format!("Failed to create ActorNode: {}", error));
            }

            // Step 3: Create corresponding Actor in C++ scene
            let result = ffi::raw::tracey_scene_create_actor_with_uid(scene_ptr, actor_uid, name_c.as_ptr());
            if result != TraceyResult::Success {
                eprintln!("Warning: Failed to create corresponding Actor for ActorNode {}", actor_uid);
            }

            // Step 4: Get the ActorNode and its geometry network
            let actor_node = ffi::raw::tracey_node_graph_get_node(scene_graph, actor_uid);
            if actor_node.is_null() {
                return Err("Failed to get ActorNode".to_string());
            }

            let geometry_network = ffi::raw::tracey_actor_node_get_geometry_network(actor_node);
            if geometry_network.is_null() {
                return Err("Failed to get geometry network".to_string());
            }

            // Step 5: Create geometry node in the geometry network
            geom_node_uid = ffi::raw::tracey_node_graph_create_node(
                geometry_network,
                primitive.node_type(),
                geom_name_c.as_ptr(),
            );
            if geom_node_uid == u64::MAX {
                let error = std::ffi::CStr::from_ptr(ffi::raw::tracey_get_last_error())
                    .to_string_lossy()
                    .into_owned();
                return Err(format!("Failed to create geometry node: {}", error));
            }

            // Step 6: Set parameters on the geometry node
            let geom_node = ffi::raw::tracey_node_graph_get_node(geometry_network, geom_node_uid);
            if !geom_node.is_null() {
                match &primitive {
                    PrimitiveType::Cube { size } => {
                        if let Some(s) = size {
                            let param_name = CString::new("size").unwrap();
                            let param = ffi::raw::tracey_node_get_parameter(geom_node, param_name.as_ptr());
                            if !param.is_null() {
                                ffi::raw::tracey_parameter_set_float(param, *s);
                            }
                        }
                    }
                    PrimitiveType::Sphere { radius, segments, rings } => {
                        if let Some(r) = radius {
                            let param_name = CString::new("radius").unwrap();
                            let param = ffi::raw::tracey_node_get_parameter(geom_node, param_name.as_ptr());
                            if !param.is_null() {
                                ffi::raw::tracey_parameter_set_float(param, *r);
                            }
                        }
                        if let Some(s) = segments {
                            let param_name = CString::new("segments").unwrap();
                            let param = ffi::raw::tracey_node_get_parameter(geom_node, param_name.as_ptr());
                            if !param.is_null() {
                                ffi::raw::tracey_parameter_set_int(param, *s as i32);
                            }
                        }
                        if let Some(r) = rings {
                            let param_name = CString::new("rings").unwrap();
                            let param = ffi::raw::tracey_node_get_parameter(geom_node, param_name.as_ptr());
                            if !param.is_null() {
                                ffi::raw::tracey_parameter_set_int(param, *r as i32);
                            }
                        }
                    }
                    PrimitiveType::Torus { major_radius, minor_radius, major_segments, minor_segments } => {
                        if let Some(r) = major_radius {
                            let param_name = CString::new("major_radius").unwrap();
                            let param = ffi::raw::tracey_node_get_parameter(geom_node, param_name.as_ptr());
                            if !param.is_null() {
                                ffi::raw::tracey_parameter_set_float(param, *r);
                            }
                        }
                        if let Some(r) = minor_radius {
                            let param_name = CString::new("minor_radius").unwrap();
                            let param = ffi::raw::tracey_node_get_parameter(geom_node, param_name.as_ptr());
                            if !param.is_null() {
                                ffi::raw::tracey_parameter_set_float(param, *r);
                            }
                        }
                        if let Some(s) = major_segments {
                            let param_name = CString::new("major_segments").unwrap();
                            let param = ffi::raw::tracey_node_get_parameter(geom_node, param_name.as_ptr());
                            if !param.is_null() {
                                ffi::raw::tracey_parameter_set_int(param, *s as i32);
                            }
                        }
                        if let Some(s) = minor_segments {
                            let param_name = CString::new("minor_segments").unwrap();
                            let param = ffi::raw::tracey_node_get_parameter(geom_node, param_name.as_ptr());
                            if !param.is_null() {
                                ffi::raw::tracey_parameter_set_int(param, *s as i32);
                            }
                        }
                    }
                    PrimitiveType::Plane { width, depth } => {
                        if let Some(w) = width {
                            let param_name = CString::new("width").unwrap();
                            let param = ffi::raw::tracey_node_get_parameter(geom_node, param_name.as_ptr());
                            if !param.is_null() {
                                ffi::raw::tracey_parameter_set_float(param, *w);
                            }
                        }
                        if let Some(d) = depth {
                            let param_name = CString::new("depth").unwrap();
                            let param = ffi::raw::tracey_node_get_parameter(geom_node, param_name.as_ptr());
                            if !param.is_null() {
                                ffi::raw::tracey_parameter_set_float(param, *d);
                            }
                        }
                    }
                    PrimitiveType::Cylinder { radius, height, segments } => {
                        if let Some(r) = radius {
                            let param_name = CString::new("radius").unwrap();
                            let param = ffi::raw::tracey_node_get_parameter(geom_node, param_name.as_ptr());
                            if !param.is_null() {
                                ffi::raw::tracey_parameter_set_float(param, *r);
                            }
                        }
                        if let Some(h) = height {
                            let param_name = CString::new("height").unwrap();
                            let param = ffi::raw::tracey_node_get_parameter(geom_node, param_name.as_ptr());
                            if !param.is_null() {
                                ffi::raw::tracey_parameter_set_float(param, *h);
                            }
                        }
                        if let Some(s) = segments {
                            let param_name = CString::new("segments").unwrap();
                            let param = ffi::raw::tracey_node_get_parameter(geom_node, param_name.as_ptr());
                            if !param.is_null() {
                                ffi::raw::tracey_parameter_set_int(param, *s as i32);
                            }
                        }
                    }
                    PrimitiveType::Cone { radius, height, segments } => {
                        if let Some(r) = radius {
                            let param_name = CString::new("radius").unwrap();
                            let param = ffi::raw::tracey_node_get_parameter(geom_node, param_name.as_ptr());
                            if !param.is_null() {
                                ffi::raw::tracey_parameter_set_float(param, *r);
                            }
                        }
                        if let Some(h) = height {
                            let param_name = CString::new("height").unwrap();
                            let param = ffi::raw::tracey_node_get_parameter(geom_node, param_name.as_ptr());
                            if !param.is_null() {
                                ffi::raw::tracey_parameter_set_float(param, *h);
                            }
                        }
                        if let Some(s) = segments {
                            let param_name = CString::new("segments").unwrap();
                            let param = ffi::raw::tracey_node_get_parameter(geom_node, param_name.as_ptr());
                            if !param.is_null() {
                                ffi::raw::tracey_parameter_set_int(param, *s as i32);
                            }
                        }
                    }
                }
            }

            // Step 7: Set geometry node as output of the geometry network
            let result = ffi::raw::tracey_node_graph_set_output(
                geometry_network,
                output_name_c.as_ptr(),
                geom_node_uid,
            );
            if result != TraceyResult::Success {
                let error = std::ffi::CStr::from_ptr(ffi::raw::tracey_get_last_error())
                    .to_string_lossy()
                    .into_owned();
                return Err(format!("Failed to set geometry output: {}", error));
            }

            // Step 8: Evaluate and sync to scene
            let eval_result = ffi::raw::tracey_node_graph_evaluate(scene_graph, 0.0, 0);
            if eval_result != TraceyResult::Success {
                let error = std::ffi::CStr::from_ptr(ffi::raw::tracey_get_last_error())
                    .to_string_lossy()
                    .into_owned();
                return Err(format!("Failed to evaluate graph: {}", error));
            }

            let sync_result = ffi::raw::tracey_scene_sync_from_node_graph(scene_ptr);
            if sync_result != TraceyResult::Success {
                let error = std::ffi::CStr::from_ptr(ffi::raw::tracey_get_last_error())
                    .to_string_lossy()
                    .into_owned();
                return Err(format!("Failed to sync node graph to scene: {}", error));
            }
        }
    } // Release engine lock

    // Add to Rust SceneState (parented to root, UID 0)
    {
        let mut scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;

        let actor = Actor {
            id: actor_uid,
            name: actor_name.clone(),
            transform: Transform::identity(),
            children: Vec::new(),
            parent: Some(0), // Parent to root
            instances: Vec::new(),
        };
        scene.actors.insert(actor_uid, actor);

        // Add to root's children list
        if let Some(root) = scene.actors.get_mut(&0) {
            root.children.push(actor_uid);
        }
    }

    // Return the actor
    Ok(Actor {
        id: actor_uid,
        name: actor_name,
        transform: Transform::identity(),
        children: Vec::new(),
        parent: Some(0), // Parented to root
        instances: Vec::new(),
    })
}

// ============================================================================
// Node Registry Query API
// ============================================================================

/// Node metadata descriptor
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct NodeDescriptor {
    pub node_type: String,
    pub name: String,
    pub description: String,
    pub category: String,
    pub icon: String,
}

/// Get all available node types with metadata
#[tauri::command]
pub fn get_available_node_types() -> Result<Vec<NodeDescriptor>, String> {
    unsafe {
        const MAX_NODES: usize = 50;
        let mut descriptors: Vec<ffi::raw::TraceyNodeDescriptor> = vec![
            std::mem::zeroed();
            MAX_NODES
        ];

        let count = ffi::raw::tracey_get_node_types(descriptors.as_mut_ptr(), MAX_NODES as i32);

        if count == 0 {
            let error = CStr::from_ptr(ffi::raw::tracey_get_last_error())
                .to_string_lossy()
                .into_owned();
            if !error.is_empty() {
                return Err(format!("Failed to get node types: {}", error));
            }
            return Ok(Vec::new());
        }

        let result: Vec<NodeDescriptor> = descriptors
            .iter()
            .take(count as usize)
            .map(|d| {
                let name = if !d.name.is_null() {
                    CStr::from_ptr(d.name).to_string_lossy().to_string()
                } else {
                    "Unknown".to_string()
                };

                let description = if !d.description.is_null() {
                    CStr::from_ptr(d.description).to_string_lossy().to_string()
                } else {
                    "".to_string()
                };

                let icon = if !d.icon.is_null() {
                    CStr::from_ptr(d.icon).to_string_lossy().to_string()
                } else {
                    "".to_string()
                };

                let category = match d.category {
                    0 => "Actor",
                    1 => "Primitive",
                    2 => "Geometry",
                    3 => "Math",
                    4 => "Utility",
                    _ => "Unknown",
                }
                .to_string();

                let node_type = format!("{:?}", d.node_type);

                NodeDescriptor {
                    node_type,
                    name,
                    description,
                    category,
                    icon,
                }
            })
            .collect();

        // Free the C strings
        for desc in &mut descriptors[..count as usize] {
            ffi::raw::tracey_free_node_descriptor(desc);
        }

        Ok(result)
    }
}

/// Get nodes by category
#[tauri::command]
pub fn get_nodes_by_category(category: String) -> Result<Vec<NodeDescriptor>, String> {
    let category_int = match category.as_str() {
        "Actor" | "actor" => 0,
        "Primitive" | "primitive" => 1,
        "Geometry" | "geometry" => 2,
        "Math" | "math" => 3,
        "Utility" | "utility" => 4,
        _ => return Err(format!("Unknown category: {}", category)),
    };

    unsafe {
        const MAX_NODES: usize = 50;
        let mut descriptors: Vec<ffi::raw::TraceyNodeDescriptor> = vec![
            std::mem::zeroed();
            MAX_NODES
        ];

        let count = ffi::raw::tracey_get_nodes_by_category(
            category_int,
            descriptors.as_mut_ptr(),
            MAX_NODES as i32,
        );

        if count == 0 {
            return Ok(Vec::new());
        }

        let result: Vec<NodeDescriptor> = descriptors
            .iter()
            .take(count as usize)
            .map(|d| {
                let name = if !d.name.is_null() {
                    CStr::from_ptr(d.name).to_string_lossy().to_string()
                } else {
                    "Unknown".to_string()
                };

                let description = if !d.description.is_null() {
                    CStr::from_ptr(d.description).to_string_lossy().to_string()
                } else {
                    "".to_string()
                };

                let icon = if !d.icon.is_null() {
                    CStr::from_ptr(d.icon).to_string_lossy().to_string()
                } else {
                    "".to_string()
                };

                let category = match d.category {
                    0 => "Actor",
                    1 => "Primitive",
                    2 => "Geometry",
                    3 => "Math",
                    4 => "Utility",
                    _ => "Unknown",
                }
                .to_string();

                let node_type = format!("{:?}", d.node_type);

                NodeDescriptor {
                    node_type,
                    name,
                    description,
                    category,
                    icon,
                }
            })
            .collect();

        // Free the C strings
        for desc in &mut descriptors[..count as usize] {
            ffi::raw::tracey_free_node_descriptor(desc);
        }

        Ok(result)
    }
}

/// Get port information for a specific node (Phase 2 - Port System)
#[tauri::command]
pub async fn get_node_ports(
    state: State<'_, AppState>,
    node_uid: u64,
) -> Result<Vec<PortInfo>, String> {
    let context = state
        .current_graph_context
        .lock()
        .map_err(|_| "Failed to lock graph context")?;

    let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
    let scene_ptr = engine.get_scene_ptr().ok_or("Failed to get scene pointer")?;

    unsafe {
        // Get the appropriate graph based on context
        let graph = match context.level {
            crate::GraphLevel::Scene => {
                ffi::raw::tracey_scene_get_node_graph(scene_ptr)
            }
            crate::GraphLevel::GeometryNetwork => {
                let actor_node_uid = context
                    .actor_node_uid
                    .ok_or("No actor node UID in geometry network context")?;

                let scene_graph = ffi::raw::tracey_scene_get_node_graph(scene_ptr);
                if scene_graph.is_null() {
                    return Err("Failed to get scene graph".to_string());
                }

                let node = ffi::raw::tracey_node_graph_get_node(scene_graph, actor_node_uid);
                if node.is_null() {
                    return Err("ActorNode not found".to_string());
                }

                ffi::raw::tracey_actor_node_get_geometry_network(node)
            }
        };

        if graph.is_null() {
            return Err("Failed to get node graph".to_string());
        }

        let node = ffi::raw::tracey_node_graph_get_node(graph, node_uid);
        if node.is_null() {
            return Err("Node not found".to_string());
        }

        let mut ports = Vec::new();

        // Get input ports
        let input_count =
            ffi::raw::tracey_node_get_port_count(node, ffi::raw::TraceyPortType::Input);
        for i in 0..input_count {
            let mut port_info = ffi::raw::TraceyPortInfo {
                name: std::ptr::null(),
                data_type: ffi::raw::TraceyDataType::Float,
                port_type: ffi::raw::TraceyPortType::Input,
            };

            let result = ffi::raw::tracey_node_get_port(
                node,
                ffi::raw::TraceyPortType::Input,
                i,
                &mut port_info,
            );

            if result == TraceyResult::Success && !port_info.name.is_null() {
                let name = CStr::from_ptr(port_info.name).to_string_lossy().to_string();
                let data_type = match port_info.data_type {
                    ffi::raw::TraceyDataType::Float => DataType::Float,
                    ffi::raw::TraceyDataType::Vec2 => DataType::Vec2,
                    ffi::raw::TraceyDataType::Vec3 => DataType::Vec3,
                    ffi::raw::TraceyDataType::Vec4 => DataType::Vec4,
                    ffi::raw::TraceyDataType::Mat3 => DataType::Mat3,
                    ffi::raw::TraceyDataType::Mat4 => DataType::Mat4,
                    ffi::raw::TraceyDataType::Int => DataType::Int,
                    ffi::raw::TraceyDataType::UInt => DataType::UInt,
                    ffi::raw::TraceyDataType::Bool => DataType::Bool,
                    ffi::raw::TraceyDataType::Sampler2D => DataType::Sampler2D,
                    ffi::raw::TraceyDataType::Geometry => DataType::Geometry,
                    ffi::raw::TraceyDataType::DataType => DataType::DataType,
                    ffi::raw::TraceyDataType::Scene3D => DataType::Scene3D,
                };

                ports.push(PortInfo {
                    name,
                    data_type,
                    port_type: PortType::Input,
                });
            }
        }

        // Get output ports
        let output_count =
            ffi::raw::tracey_node_get_port_count(node, ffi::raw::TraceyPortType::Output);
        for i in 0..output_count {
            let mut port_info = ffi::raw::TraceyPortInfo {
                name: std::ptr::null(),
                data_type: ffi::raw::TraceyDataType::Float,
                port_type: ffi::raw::TraceyPortType::Output,
            };

            let result = ffi::raw::tracey_node_get_port(
                node,
                ffi::raw::TraceyPortType::Output,
                i,
                &mut port_info,
            );

            if result == TraceyResult::Success && !port_info.name.is_null() {
                let name = CStr::from_ptr(port_info.name).to_string_lossy().to_string();
                let data_type = match port_info.data_type {
                    ffi::raw::TraceyDataType::Float => DataType::Float,
                    ffi::raw::TraceyDataType::Vec2 => DataType::Vec2,
                    ffi::raw::TraceyDataType::Vec3 => DataType::Vec3,
                    ffi::raw::TraceyDataType::Vec4 => DataType::Vec4,
                    ffi::raw::TraceyDataType::Mat3 => DataType::Mat3,
                    ffi::raw::TraceyDataType::Mat4 => DataType::Mat4,
                    ffi::raw::TraceyDataType::Int => DataType::Int,
                    ffi::raw::TraceyDataType::UInt => DataType::UInt,
                    ffi::raw::TraceyDataType::Bool => DataType::Bool,
                    ffi::raw::TraceyDataType::Sampler2D => DataType::Sampler2D,
                    ffi::raw::TraceyDataType::Geometry => DataType::Geometry,
                    ffi::raw::TraceyDataType::DataType => DataType::DataType,
                    ffi::raw::TraceyDataType::Scene3D => DataType::Scene3D,
                };

                ports.push(PortInfo {
                    name,
                    data_type,
                    port_type: PortType::Output,
                });
            }
        }

        Ok(ports)
    }
}

/// Result of duplicating a node
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DuplicatedNode {
    pub original_uid: u64,
    pub new_uid: u64,
}

/// Duplicate multiple nodes (creates new nodes with same types)
/// Returns mapping from original UIDs to new UIDs
#[tauri::command]
pub async fn duplicate_nodes(
    state: State<'_, AppState>,
    node_uids: Vec<u64>,
) -> Result<Vec<DuplicatedNode>, String> {
    let context = state
        .current_graph_context
        .lock()
        .map_err(|_| "Failed to lock graph context")?;

    let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
    let scene_ptr = engine.get_scene_ptr().ok_or("Failed to get scene pointer")?;

    let mut duplicated = Vec::new();

    unsafe {
        // Get the appropriate graph based on context
        let graph = match context.level {
            crate::GraphLevel::Scene => {
                ffi::raw::tracey_scene_get_node_graph(scene_ptr)
            }
            crate::GraphLevel::GeometryNetwork => {
                let actor_node_uid = context
                    .actor_node_uid
                    .ok_or("No actor node UID in geometry network context")?;

                let scene_graph = ffi::raw::tracey_scene_get_node_graph(scene_ptr);
                if scene_graph.is_null() {
                    return Err("Failed to get scene graph".to_string());
                }

                let node = ffi::raw::tracey_node_graph_get_node(scene_graph, actor_node_uid);
                if node.is_null() {
                    return Err("ActorNode not found".to_string());
                }

                ffi::raw::tracey_actor_node_get_geometry_network(node)
            }
        };

        if graph.is_null() {
            return Err("Failed to get node graph".to_string());
        }

        for original_uid in node_uids {
            // Get the original node
            let original_node = ffi::raw::tracey_node_graph_get_node(graph, original_uid);
            if original_node.is_null() {
                continue; // Skip nodes that don't exist
            }

            // Get the node type
            let node_type = ffi::raw::tracey_node_get_type(original_node);

            // Get the node name and create a copy name
            let name_ptr = ffi::raw::tracey_node_get_name(original_node);
            let original_name = if !name_ptr.is_null() {
                CStr::from_ptr(name_ptr).to_string_lossy().to_string()
            } else {
                "Node".to_string()
            };
            let new_name = format!("{}_copy", original_name);
            let new_name_c = CString::new(new_name).map_err(|e| format!("Invalid name: {}", e))?;

            // Create a new node of the same type
            let new_uid = ffi::raw::tracey_node_graph_create_node(graph, node_type, new_name_c.as_ptr());

            if new_uid != u64::MAX {
                duplicated.push(DuplicatedNode {
                    original_uid,
                    new_uid,
                });
            }
        }
    }

    Ok(duplicated)
}
