use serde::{Deserialize, Serialize};
use chrono::{DateTime, Utc};
use std::path::Path;
use std::fs;

/// Recent project entry
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RecentProject {
    pub name: String,
    pub path: String,
    pub last_opened: DateTime<Utc>,
    pub thumbnail: Option<String>,  // Future: base64 encoded preview
}

/// Recent projects list
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RecentProjects {
    pub recent: Vec<RecentProject>,
    pub max_recent: usize,
}

impl Default for RecentProjects {
    fn default() -> Self {
        Self {
            recent: Vec::new(),
            max_recent: 10,
        }
    }
}

impl RecentProjects {
    /// Load recent projects from app data directory
    pub fn load(app_data_dir: &Path) -> Self {
        let path = app_data_dir.join("recent_projects.json");

        if let Ok(json) = fs::read_to_string(&path) {
            serde_json::from_str(&json).unwrap_or_else(|_| Self::default())
        } else {
            Self::default()
        }
    }

    /// Save recent projects to app data directory
    pub fn save(&self, app_data_dir: &Path) -> Result<(), String> {
        // Ensure app data directory exists
        fs::create_dir_all(app_data_dir)
            .map_err(|e| format!("Failed to create app data directory: {}", e))?;

        let path = app_data_dir.join("recent_projects.json");

        let json = serde_json::to_string_pretty(self)
            .map_err(|e| format!("Failed to serialize recent projects: {}", e))?;

        fs::write(path, json)
            .map_err(|e| format!("Failed to write recent projects: {}", e))
    }

    /// Add or update a project in the recent list
    pub fn add(&mut self, project_path: String, project_name: String) {
        // Remove if already exists
        self.recent.retain(|p| p.path != project_path);

        // Add to front
        self.recent.insert(0, RecentProject {
            name: project_name,
            path: project_path,
            last_opened: Utc::now(),
            thumbnail: None,
        });

        // Limit to max_recent
        self.recent.truncate(self.max_recent);
    }

    /// Remove a project from the recent list
    pub fn remove(&mut self, project_path: &str) {
        self.recent.retain(|p| p.path != project_path);
    }

    /// Get all recent projects
    pub fn list(&self) -> &[RecentProject] {
        &self.recent
    }

    /// Clear all recent projects
    pub fn clear(&mut self) {
        self.recent.clear();
    }
}
