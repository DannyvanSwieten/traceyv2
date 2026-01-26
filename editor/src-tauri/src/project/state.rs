use serde::{Deserialize, Serialize};
use chrono::{DateTime, Utc};
use std::path::{Path, PathBuf};
use std::fs;

use crate::scene::state::SceneState;
use super::asset::AssetRegistry;

/// Project metadata
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProjectMetadata {
    pub name: String,
    pub description: String,
    pub created: DateTime<Utc>,
    pub modified: DateTime<Utc>,
}

/// Shader modification timestamps
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ShaderModifiedTimes {
    pub ray_gen: DateTime<Utc>,
    pub hit: DateTime<Utc>,
    pub miss: DateTime<Utc>,
    pub resolve: DateTime<Utc>,
}

/// ISF shader configuration
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ShaderConfig {
    pub ray_gen: String,      // Relative path: "shaders/ray_gen.isf"
    pub hit: String,
    pub miss: String,
    pub resolve: String,
    pub modified: ShaderModifiedTimes,
}

impl ShaderConfig {
    pub fn default_paths() -> Self {
        Self {
            ray_gen: "shaders/ray_gen.isf".to_string(),
            hit: "shaders/diffuse_hit.isf".to_string(),
            miss: "shaders/sky_miss.isf".to_string(),
            resolve: "shaders/resolve.isf".to_string(),
            modified: ShaderModifiedTimes {
                ray_gen: Utc::now(),
                hit: Utc::now(),
                miss: Utc::now(),
                resolve: Utc::now(),
            },
        }
    }
}

/// Render settings stored in project
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProjectRenderSettings {
    pub width: u32,
    pub height: u32,
    pub hdr_output: bool,
    pub samples_per_frame: u32,
    pub max_bounces: u32,
    pub render_mode: String,
}

impl Default for ProjectRenderSettings {
    fn default() -> Self {
        Self {
            width: 1280,
            height: 720,
            hdr_output: false,
            samples_per_frame: 16,
            max_bounces: 8,
            render_mode: "PathTracer".to_string(),
        }
    }
}

/// Shader type enumeration
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum ShaderType {
    RayGen,
    Hit,
    Miss,
    Resolve,
}

impl ShaderType {
    pub fn as_str(&self) -> &'static str {
        match self {
            ShaderType::RayGen => "ray_gen",
            ShaderType::Hit => "hit",
            ShaderType::Miss => "miss",
            ShaderType::Resolve => "resolve",
        }
    }
}

/// Main project state
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProjectState {
    pub version: String,
    pub metadata: ProjectMetadata,
    pub scene: SceneState,
    pub assets: AssetRegistry,
    pub shaders: ShaderConfig,
    pub render_settings: ProjectRenderSettings,

    #[serde(skip)]
    pub project_dir: PathBuf,
}

impl ProjectState {
    /// Create a new project
    pub fn new(name: String, project_dir: PathBuf) -> Result<Self, String> {
        let now = Utc::now();

        Ok(Self {
            version: "1.0.0".to_string(),
            metadata: ProjectMetadata {
                name,
                description: String::new(),
                created: now,
                modified: now,
            },
            scene: SceneState::default(),
            assets: AssetRegistry::default(),
            shaders: ShaderConfig::default_paths(),
            render_settings: ProjectRenderSettings::default(),
            project_dir,
        })
    }

    /// Save project to project.tracey file
    pub fn save(&mut self) -> Result<(), String> {
        // Update modified timestamp
        self.metadata.modified = Utc::now();

        // Serialize to JSON
        let json = serde_json::to_string_pretty(&self)
            .map_err(|e| format!("Failed to serialize project: {}", e))?;

        // Write to project.tracey
        let project_file = self.project_dir.join("project.tracey");
        fs::write(&project_file, json)
            .map_err(|e| format!("Failed to write project file: {}", e))?;

        Ok(())
    }

    /// Load project from project.tracey file
    pub fn load(project_file: &Path) -> Result<Self, String> {
        // Read file
        let json = fs::read_to_string(project_file)
            .map_err(|e| format!("Failed to read project file: {}", e))?;

        // Deserialize
        let mut project: Self = serde_json::from_str(&json)
            .map_err(|e| format!("Failed to parse project file: {}", e))?;

        // Set project_dir (parent of project.tracey)
        project.project_dir = project_file
            .parent()
            .ok_or("Invalid project file path")?
            .to_path_buf();

        Ok(project)
    }

    /// Get absolute path to a shader file
    pub fn get_shader_path(&self, shader_type: ShaderType) -> PathBuf {
        let relative_path = match shader_type {
            ShaderType::RayGen => &self.shaders.ray_gen,
            ShaderType::Hit => &self.shaders.hit,
            ShaderType::Miss => &self.shaders.miss,
            ShaderType::Resolve => &self.shaders.resolve,
        };

        self.project_dir.join(relative_path)
    }

    /// Check for shader modifications and return list of modified shaders
    pub fn check_shader_modifications(&mut self) -> Vec<ShaderType> {
        let mut modified = Vec::new();

        let shaders = vec![
            (ShaderType::RayGen, &self.shaders.ray_gen, &mut self.shaders.modified.ray_gen),
            (ShaderType::Hit, &self.shaders.hit, &mut self.shaders.modified.hit),
            (ShaderType::Miss, &self.shaders.miss, &mut self.shaders.modified.miss),
            (ShaderType::Resolve, &self.shaders.resolve, &mut self.shaders.modified.resolve),
        ];

        for (shader_type, relative_path, last_modified) in shaders {
            let path = self.project_dir.join(relative_path);

            if let Ok(metadata) = fs::metadata(&path) {
                if let Ok(file_modified) = metadata.modified() {
                    let file_time: DateTime<Utc> = file_modified.into();

                    if file_time > *last_modified {
                        modified.push(shader_type);
                        *last_modified = file_time;
                    }
                }
            }
        }

        // Save if any shaders were modified (to update timestamps)
        if !modified.is_empty() {
            let _ = self.save();
        }

        modified
    }
}
