use serde::{Deserialize, Serialize};
use chrono::{DateTime, Utc};
use std::path::{Path, PathBuf};
use std::fs;
use uuid::Uuid;

/// Asset type enumeration
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum AssetType {
    Model,
    Texture,
}

impl AssetType {
    pub fn as_str(&self) -> &'static str {
        match self {
            AssetType::Model => "model",
            AssetType::Texture => "texture",
        }
    }

    pub fn from_str(s: &str) -> Option<Self> {
        match s.to_lowercase().as_str() {
            "model" => Some(AssetType::Model),
            "texture" => Some(AssetType::Texture),
            _ => None,
        }
    }

    pub fn subfolder(&self) -> &'static str {
        match self {
            AssetType::Model => "models",
            AssetType::Texture => "textures",
        }
    }
}

/// Asset entry in the registry
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AssetEntry {
    pub id: String,
    pub asset_type: AssetType,
    pub original_path: String,
    pub project_path: String,  // Relative to project root
    pub name: String,
    pub imported_at: DateTime<Utc>,
    pub extracted_textures: Vec<String>,
}

/// Asset registry
#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct AssetRegistry {
    pub entries: Vec<AssetEntry>,
}

impl AssetRegistry {
    pub fn add(&mut self, entry: AssetEntry) {
        self.entries.push(entry);
    }

    pub fn find_by_name(&self, name: &str) -> Option<&AssetEntry> {
        self.entries.iter().find(|e| e.name == name)
    }

    pub fn find_by_id(&self, id: &str) -> Option<&AssetEntry> {
        self.entries.iter().find(|e| e.id == id)
    }
}

/// Copy asset to project directory with duplicate handling
pub fn copy_asset_to_project(
    source_path: &Path,
    project_dir: &Path,
    asset_type: AssetType,
) -> Result<AssetEntry, String> {
    // Get source filename
    let source_filename = source_path
        .file_name()
        .ok_or("Invalid source path")?
        .to_str()
        .ok_or("Invalid UTF-8 in filename")?;

    // Get file stem and extension
    let file_stem = source_path
        .file_stem()
        .ok_or("Invalid source path")?
        .to_str()
        .ok_or("Invalid UTF-8 in filename")?;

    let extension = source_path
        .extension()
        .and_then(|e| e.to_str())
        .unwrap_or("");

    // Create destination directory
    let dest_dir = project_dir.join("assets").join(asset_type.subfolder());
    fs::create_dir_all(&dest_dir)
        .map_err(|e| format!("Failed to create asset directory: {}", e))?;

    // Find unique filename (handle duplicates)
    let mut dest_filename = source_filename.to_string();
    let mut dest_path = dest_dir.join(&dest_filename);
    let mut counter = 1;

    while dest_path.exists() {
        dest_filename = if extension.is_empty() {
            format!("{}_{}", file_stem, counter)
        } else {
            format!("{}_{}.{}", file_stem, counter, extension)
        };
        dest_path = dest_dir.join(&dest_filename);
        counter += 1;
    }

    // Copy file
    fs::copy(source_path, &dest_path)
        .map_err(|e| format!("Failed to copy asset: {}", e))?;

    // For glTF/glb models, also copy any sibling .bin file with the same base name
    if asset_type == AssetType::Model {
        let source_dir = source_path.parent().ok_or("Invalid source path")?;
        let bin_filename = format!("{}.bin", file_stem);
        let bin_source_path = source_dir.join(&bin_filename);

        if bin_source_path.exists() {
            let bin_dest_path = dest_dir.join(&bin_filename);
            println!("Found sibling .bin file, copying: {:?} -> {:?}", bin_source_path, bin_dest_path);
            fs::copy(&bin_source_path, &bin_dest_path)
                .map_err(|e| format!("Failed to copy .bin file: {}", e))?;
        }
    }

    // Create relative path (from project root)
    let relative_path = PathBuf::from("assets")
        .join(asset_type.subfolder())
        .join(&dest_filename);

    // Create asset entry
    let entry = AssetEntry {
        id: Uuid::new_v4().to_string(),
        asset_type,
        original_path: source_path.to_string_lossy().to_string(),
        project_path: relative_path.to_string_lossy().to_string(),
        name: dest_filename.clone(),
        imported_at: Utc::now(),
        extracted_textures: Vec::new(),
    };

    Ok(entry)
}

/// Extract binary files from GLTF and copy them to project
pub fn extract_gltf_binaries(
    gltf_path: &Path,
    project_dir: &Path,
) -> Result<Vec<String>, String> {
    let mut extracted_binaries = Vec::new();

    // Only process .gltf files (not .glb which has embedded data)
    if gltf_path.extension().and_then(|e| e.to_str()) != Some("gltf") {
        println!("Skipping binary extraction for non-.gltf file: {:?}", gltf_path);
        return Ok(extracted_binaries);
    }

    println!("Extracting binaries from glTF: {:?}", gltf_path);

    // Read GLTF file
    let gltf_content = fs::read_to_string(gltf_path)
        .map_err(|e| format!("Failed to read GLTF file: {}", e))?;

    // Parse JSON
    let gltf_json: serde_json::Value = serde_json::from_str(&gltf_content)
        .map_err(|e| format!("Failed to parse GLTF JSON: {}", e))?;

    // Get base directory for resolving relative paths
    let base_dir = gltf_path
        .parent()
        .ok_or("Invalid GLTF path")?;

    println!("Base directory for binary files: {:?}", base_dir);

    // Create models directory for bin files
    let models_dir = project_dir.join("assets").join("models");
    fs::create_dir_all(&models_dir)
        .map_err(|e| format!("Failed to create models directory: {}", e))?;

    // Extract buffer URIs from GLTF JSON
    if let Some(buffers) = gltf_json.get("buffers").and_then(|v| v.as_array()) {
        println!("Found {} buffer(s) in glTF", buffers.len());
        for buffer in buffers {
            // Get URI from buffer
            if let Some(uri) = buffer.get("uri").and_then(|v| v.as_str()) {
                println!("Processing buffer URI: {}", uri);

                // Skip data URIs (embedded)
                if uri.starts_with("data:") {
                    println!("Skipping embedded data URI");
                    continue;
                }

                // Resolve binary path
                let binary_path = base_dir.join(uri);
                println!("Looking for binary at: {:?}", binary_path);

                if !binary_path.exists() {
                    eprintln!("Warning: Binary file not found: {:?}", binary_path);
                    continue;
                }

                // Get filename
                let filename = binary_path
                    .file_name()
                    .ok_or("Invalid binary path")?
                    .to_str()
                    .ok_or("Invalid UTF-8 in filename")?;

                // Copy binary to project
                let dest_path = models_dir.join(filename);
                println!("Copying {} to {:?}", filename, dest_path);
                fs::copy(&binary_path, &dest_path)
                    .map_err(|e| format!("Failed to copy binary: {}", e))?;
                println!("Successfully copied binary file");

                // Add to extracted list (relative path)
                let relative_path = PathBuf::from("assets")
                    .join("models")
                    .join(filename);

                extracted_binaries.push(relative_path.to_string_lossy().to_string());
            }
        }
    }

    Ok(extracted_binaries)
}

/// Extract textures from GLTF and copy them to project
pub fn extract_gltf_textures(
    gltf_path: &Path,
    project_dir: &Path,
) -> Result<Vec<String>, String> {
    let mut extracted_textures = Vec::new();

    // Only process .gltf files (not .glb which has embedded textures)
    if gltf_path.extension().and_then(|e| e.to_str()) != Some("gltf") {
        return Ok(extracted_textures);
    }

    // Read GLTF file
    let gltf_content = fs::read_to_string(gltf_path)
        .map_err(|e| format!("Failed to read GLTF file: {}", e))?;

    // Parse JSON
    let gltf_json: serde_json::Value = serde_json::from_str(&gltf_content)
        .map_err(|e| format!("Failed to parse GLTF JSON: {}", e))?;

    // Get base directory for resolving relative paths
    let base_dir = gltf_path
        .parent()
        .ok_or("Invalid GLTF path")?;

    // Create textures directory
    let textures_dir = project_dir.join("assets").join("textures");
    fs::create_dir_all(&textures_dir)
        .map_err(|e| format!("Failed to create textures directory: {}", e))?;

    // Extract image URIs from GLTF JSON
    if let Some(images) = gltf_json.get("images").and_then(|v| v.as_array()) {
        for image in images {
            // Get URI from image
            if let Some(uri) = image.get("uri").and_then(|v| v.as_str()) {
                // Skip data URIs (embedded)
                if uri.starts_with("data:") {
                    continue;
                }

                // Resolve texture path
                let texture_path = base_dir.join(uri);

                if !texture_path.exists() {
                    eprintln!("Warning: Texture not found: {:?}", texture_path);
                    continue;
                }

                // Get filename
                let filename = texture_path
                    .file_name()
                    .ok_or("Invalid texture path")?
                    .to_str()
                    .ok_or("Invalid UTF-8 in filename")?;

                // Find unique destination filename
                let mut dest_filename = filename.to_string();
                let mut dest_path = textures_dir.join(&dest_filename);
                let mut counter = 1;

                let file_stem = texture_path
                    .file_stem()
                    .and_then(|s| s.to_str())
                    .unwrap_or("texture");

                let extension = texture_path
                    .extension()
                    .and_then(|e| e.to_str())
                    .unwrap_or("png");

                while dest_path.exists() {
                    dest_filename = format!("{}_{}.{}", file_stem, counter, extension);
                    dest_path = textures_dir.join(&dest_filename);
                    counter += 1;
                }

                // Copy texture
                fs::copy(&texture_path, &dest_path)
                    .map_err(|e| format!("Failed to copy texture: {}", e))?;

                // Add to extracted list (relative path)
                let relative_path = PathBuf::from("assets")
                    .join("textures")
                    .join(&dest_filename);

                extracted_textures.push(relative_path.to_string_lossy().to_string());
            }
        }
    }

    Ok(extracted_textures)
}
