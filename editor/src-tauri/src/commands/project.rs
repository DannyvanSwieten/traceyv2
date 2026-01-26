use crate::project::{
    ProjectState, AssetType, copy_asset_to_project, extract_gltf_textures, extract_gltf_binaries,
    RecentProjects, RecentProject,
};
use crate::AppState;
use std::path::{Path, PathBuf};
use std::fs;
use tauri::State;

/// Create a new project
#[tauri::command]
pub async fn project_new(
    state: State<'_, AppState>,
    name: String,
    directory: String,
) -> Result<String, String> {
    // Create project directory
    let project_dir = PathBuf::from(&directory).join(&name);

    if project_dir.exists() {
        return Err(format!("Project directory already exists: {:?}", project_dir));
    }

    fs::create_dir_all(&project_dir)
        .map_err(|e| format!("Failed to create project directory: {}", e))?;

    // Create subdirectories
    fs::create_dir_all(project_dir.join("assets/models"))
        .map_err(|e| format!("Failed to create models directory: {}", e))?;

    fs::create_dir_all(project_dir.join("assets/textures"))
        .map_err(|e| format!("Failed to create textures directory: {}", e))?;

    fs::create_dir_all(project_dir.join("shaders"))
        .map_err(|e| format!("Failed to create shaders directory: {}", e))?;

    // Copy default ISF shaders
    copy_default_shaders(&project_dir)?;

    // Create new project state
    let mut project = ProjectState::new(name.clone(), project_dir.clone())?;

    // Save project file
    project.save()?;

    // Add to recent projects
    let project_file = project_dir.join("project.tracey");
    let project_path = project_file.to_string_lossy().to_string();

    {
        let mut recent = state.recent_projects.lock()
            .map_err(|_| "Failed to lock recent projects")?;

        recent.add(project_path.clone(), name);
        recent.save(&state.app_data_dir)?;
    }

    // Store project in app state
    {
        let mut current_project = state.project.lock()
            .map_err(|_| "Failed to lock project state")?;

        *current_project = Some(project);
    }

    Ok(project_path)
}

/// Open an existing project
#[tauri::command]
pub async fn project_open(
    state: State<'_, AppState>,
    project_path: String,
) -> Result<(), String> {
    // Load project
    let project = ProjectState::load(Path::new(&project_path))?;

    // Add to recent projects
    {
        let mut recent = state.recent_projects.lock()
            .map_err(|_| "Failed to lock recent projects")?;

        recent.add(project_path.clone(), project.metadata.name.clone());
        recent.save(&state.app_data_dir)?;
    }

    // Update scene state
    {
        let mut scene = state.scene.lock()
            .map_err(|_| "Failed to lock scene state")?;

        *scene = project.scene.clone();
    }

    // Store project in app state
    {
        let mut current_project = state.project.lock()
            .map_err(|_| "Failed to lock project state")?;

        *current_project = Some(project);
    }

    Ok(())
}

/// Save the current project
#[tauri::command]
pub async fn project_save(state: State<'_, AppState>) -> Result<(), String> {
    // Get mutable project
    let mut project = {
        let mut project_guard = state.project.lock()
            .map_err(|_| "Failed to lock project state")?;

        project_guard
            .as_mut()
            .ok_or("No project is currently open")?
            .clone()
    };

    // Update scene state in project
    {
        let scene = state.scene.lock()
            .map_err(|_| "Failed to lock scene state")?;

        project.scene = scene.clone();
    }

    // Save project
    project.save()?;

    // Update stored project
    {
        let mut project_guard = state.project.lock()
            .map_err(|_| "Failed to lock project state")?;

        *project_guard = Some(project);
    }

    Ok(())
}

/// Import an asset into the project
#[tauri::command]
pub async fn project_import_asset(
    state: State<'_, AppState>,
    asset_path: String,
    asset_type: String,
) -> Result<String, String> {
    // Parse asset type
    let asset_type = AssetType::from_str(&asset_type)
        .ok_or_else(|| format!("Invalid asset type: {}", asset_type))?;

    // Get project
    let mut project = {
        let project_guard = state.project.lock()
            .map_err(|_| "Failed to lock project state")?;

        project_guard
            .as_ref()
            .ok_or("No project is currently open")?
            .clone()
    };

    // Copy asset
    let source_path = Path::new(&asset_path);
    let mut entry = copy_asset_to_project(
        source_path,
        &project.project_dir,
        asset_type,
    )?;

    // If it's a GLTF model, extract binaries and textures
    if asset_type == AssetType::Model {
        // Extract .bin files (for .gltf format)
        let _binaries = extract_gltf_binaries(source_path, &project.project_dir)?;

        // Extract textures
        let textures = extract_gltf_textures(source_path, &project.project_dir)?;
        entry.extracted_textures = textures;
    }

    // Add to asset registry
    let asset_id = entry.id.clone();
    project.assets.add(entry);

    // Save project
    project.save()?;

    // Update stored project
    {
        let mut project_guard = state.project.lock()
            .map_err(|_| "Failed to lock project state")?;

        *project_guard = Some(project);
    }

    Ok(asset_id)
}

/// Check for shader modifications and return list of modified shaders
#[tauri::command]
pub async fn project_check_shaders(
    state: State<'_, AppState>,
) -> Result<Vec<String>, String> {
    // Get mutable project
    let mut project = {
        let project_guard = state.project.lock()
            .map_err(|_| "Failed to lock project state")?;

        project_guard
            .as_ref()
            .ok_or("No project is currently open")?
            .clone()
    };

    // Check modifications
    let modified = project.check_shader_modifications();

    // Update stored project if there were changes
    if !modified.is_empty() {
        let mut project_guard = state.project.lock()
            .map_err(|_| "Failed to lock project state")?;

        *project_guard = Some(project);
    }

    // Convert to strings
    let modified_names: Vec<String> = modified
        .iter()
        .map(|t| t.as_str().to_string())
        .collect();

    Ok(modified_names)
}

/// Get list of recent projects
#[tauri::command]
pub async fn get_recent_projects(
    state: State<'_, AppState>,
) -> Result<Vec<RecentProject>, String> {
    let recent = state.recent_projects.lock()
        .map_err(|_| "Failed to lock recent projects")?;

    Ok(recent.list().to_vec())
}

/// Remove a project from recent list
#[tauri::command]
pub async fn remove_recent_project(
    state: State<'_, AppState>,
    project_path: String,
) -> Result<(), String> {
    let mut recent = state.recent_projects.lock()
        .map_err(|_| "Failed to lock recent projects")?;

    recent.remove(&project_path);
    recent.save(&state.app_data_dir)?;

    Ok(())
}

/// Get current project info
#[tauri::command]
pub async fn get_project_info(
    state: State<'_, AppState>,
) -> Result<Option<serde_json::Value>, String> {
    let project = state.project.lock()
        .map_err(|_| "Failed to lock project state")?;

    if let Some(ref proj) = *project {
        let info = serde_json::json!({
            "name": proj.metadata.name,
            "description": proj.metadata.description,
            "version": proj.version,
            "created": proj.metadata.created,
            "modified": proj.metadata.modified,
            "project_dir": proj.project_dir.to_string_lossy(),
        });

        Ok(Some(info))
    } else {
        Ok(None)
    }
}

/// Copy default ISF shaders to project
fn copy_default_shaders(project_dir: &Path) -> Result<(), String> {
    // Determine source shader directory
    let source_shader_dir = if cfg!(debug_assertions) {
        // In development, use examples/scene_renderer/shaders
        PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .parent()
            .unwrap()
            .parent()
            .unwrap()
            .join("examples/scene_renderer/shaders")
    } else {
        // In production, use bundled shaders
        PathBuf::from("shaders")
    };

    let dest_shader_dir = project_dir.join("shaders");

    // List of default shaders to copy
    let shaders = vec![
        "ray_gen.isf",
        "diffuse_hit.isf",
        "sky_miss.isf",
        "resolve.isf",
        "pbr_lib.glsl",  // PBR library
    ];

    for shader in shaders {
        let source = source_shader_dir.join(shader);
        let dest = dest_shader_dir.join(shader);

        if !source.exists() {
            return Err(format!("Default shader not found: {:?}", source));
        }

        fs::copy(&source, &dest)
            .map_err(|e| format!("Failed to copy shader {}: {}", shader, e))?;
    }

    Ok(())
}
