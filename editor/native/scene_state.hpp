#pragma once

#include <string>

namespace tracey {
class Scene;
}

namespace tracey_editor {

// JSON serialization helpers for the editor scene.
// The C++ tracey::Scene is the source of truth; these helpers persist its
// editor-relevant state (actors with names + transforms, camera) to disk.
//
// Throws std::runtime_error on I/O / parse failure.
void save_scene_to_file(const tracey::Scene& scene, const std::string& path);
void load_scene_from_file(tracey::Scene& scene, const std::string& path);

}  // namespace tracey_editor
