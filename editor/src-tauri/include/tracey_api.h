#pragma once

#include "tracey_types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Opaque Handle Types
// ============================================================================

// These are opaque pointers - internal structure is hidden from C clients
typedef struct TraceyDevice TraceyDevice;
typedef struct TraceyScene TraceyScene;
typedef struct TraceyCompiledScene TraceyCompiledScene;
typedef struct TraceyPathTracer TraceyPathTracer;

// ============================================================================
// Device Management
// ============================================================================

/// Create a rendering device
/// @param deviceType CPU or GPU device
/// @param backend Rendering backend (None, Compute, or RTX)
/// @return Device handle, or NULL on failure (check tracey_get_last_error)
TraceyDevice* tracey_create_device(
    TraceyDeviceType deviceType,
    TraceyDeviceBackend backend
);

/// Destroy a device and free all associated resources
/// @param device Device handle (can be NULL)
void tracey_destroy_device(TraceyDevice* device);

// ============================================================================
// Scene Management
// ============================================================================

/// Create an empty scene
/// @return Scene handle, or NULL on failure
TraceyScene* tracey_scene_create(void);

/// Destroy a scene and free all associated resources
/// @param scene Scene handle (can be NULL)
void tracey_scene_destroy(TraceyScene* scene);

/// Create a new actor in the scene
/// @param scene Scene handle
/// @param name Actor name (UTF-8 string, can be NULL for unnamed actor)
/// @return Actor UID (unique identifier), or UINT64_MAX on failure
uint64_t tracey_scene_create_actor(
    TraceyScene* scene,
    const char* name
);

/// Get an actor's transform
/// @param scene Scene handle
/// @param actorUid Actor unique identifier
/// @param outTransform Pointer to receive transform data
/// @return TRACEY_SUCCESS or error code
TraceyResult tracey_scene_get_actor_transform(
    TraceyScene* scene,
    uint64_t actorUid,
    TraceyTransform* outTransform
);

/// Set an actor's transform
/// @param scene Scene handle
/// @param actorUid Actor unique identifier
/// @param transform New transform
/// @return TRACEY_SUCCESS or error code
TraceyResult tracey_scene_set_actor_transform(
    TraceyScene* scene,
    uint64_t actorUid,
    const TraceyTransform* transform
);

/// Set an actor's name
/// @param scene Scene handle
/// @param actorUid Actor unique identifier
/// @param name New name (UTF-8 string)
/// @return TRACEY_SUCCESS or error code
TraceyResult tracey_scene_set_actor_name(
    TraceyScene* scene,
    uint64_t actorUid,
    const char* name
);

/// Add a scene instance to an actor
/// @param scene Scene handle
/// @param actorUid Actor unique identifier
/// @param instance Instance data (objectRef must point to valid scene object)
/// @return TRACEY_SUCCESS or error code
TraceyResult tracey_scene_add_instance(
    TraceyScene* scene,
    uint64_t actorUid,
    const TraceySceneInstance* instance
);

/// Add or update the scene camera
/// @param scene Scene handle
/// @param camera Camera parameters
/// @return TRACEY_SUCCESS or error code
TraceyResult tracey_scene_set_camera(
    TraceyScene* scene,
    const TraceyCamera* camera
);

/// Get the scene camera
/// @param scene Scene handle
/// @param outCamera Pointer to receive camera data
/// @return TRACEY_SUCCESS or error code (TRACEY_ERROR_NOT_FOUND if no camera set)
TraceyResult tracey_scene_get_camera(
    TraceyScene* scene,
    TraceyCamera* outCamera
);

/// Load a GLTF/GLB file into the scene
/// @param scene Scene handle
/// @param filePath Path to GLTF/GLB file
/// @return TRACEY_SUCCESS or error code
TraceyResult tracey_scene_load_gltf(
    TraceyScene* scene,
    const char* filePath
);

/// Get the number of actors in the scene
/// @param scene Scene handle
/// @return Actor count
uint32_t tracey_scene_get_actor_count(TraceyScene* scene);

/// Get actor UIDs (for iteration)
/// @param scene Scene handle
/// @param outUids Buffer to receive UIDs (must be large enough for all actors)
/// @param maxCount Maximum number of UIDs to write
/// @return Number of UIDs written
uint32_t tracey_scene_get_actor_uids(
    TraceyScene* scene,
    uint64_t* outUids,
    uint32_t maxCount
);

// ============================================================================
// Scene Compilation
// ============================================================================

/// Compile a scene for rendering (builds acceleration structures)
/// @param device Device to use for compilation
/// @param scene Scene to compile
/// @return Compiled scene handle, or NULL on failure
TraceyCompiledScene* tracey_compile_scene(
    TraceyDevice* device,
    TraceyScene* scene
);

/// Destroy a compiled scene and free GPU resources
/// @param compiledScene Compiled scene handle (can be NULL)
void tracey_destroy_compiled_scene(TraceyCompiledScene* compiledScene);

// ============================================================================
// Path Tracer
// ============================================================================

/// Create a path tracer
/// @param device Device to use for rendering
/// @param config Path tracer configuration (shaders, resolution, etc.)
/// @return Path tracer handle, or NULL on failure
TraceyPathTracer* tracey_path_tracer_create(
    TraceyDevice* device,
    const TraceyPathTracerConfig* config
);

/// Destroy a path tracer and free resources
/// @param pathTracer Path tracer handle (can be NULL)
void tracey_path_tracer_destroy(TraceyPathTracer* pathTracer);

/// Render a frame
/// @param pathTracer Path tracer handle
/// @param compiledScene Compiled scene to render
/// @param camera Camera to render from
/// @param clearAccumulation If true, clears previous samples (restarts accumulation)
/// @return Render time in milliseconds, or negative value on error
double tracey_path_tracer_render(
    TraceyPathTracer* pathTracer,
    TraceyCompiledScene* compiledScene,
    const TraceyCamera* camera,
    bool clearAccumulation
);

/// Read back rendered image to CPU memory
/// @param pathTracer Path tracer handle
/// @param outBuffer Buffer to receive pixel data (must be large enough)
/// @param bufferSize Size of outBuffer in bytes
/// @return Number of bytes written, or 0 on error
/// @note For HDR output: width * height * 16 bytes (R32G32B32A32)
/// @note For LDR output: width * height * 4 bytes (R8G8B8A8)
size_t tracey_path_tracer_readback(
    TraceyPathTracer* pathTracer,
    void* outBuffer,
    size_t bufferSize
);

/// Get path tracer output resolution
/// @param pathTracer Path tracer handle
/// @param outWidth Pointer to receive width
/// @param outHeight Pointer to receive height
/// @return TRACEY_SUCCESS or error code
TraceyResult tracey_path_tracer_get_resolution(
    TraceyPathTracer* pathTracer,
    uint32_t* outWidth,
    uint32_t* outHeight
);

/// Get current sample count
/// @param pathTracer Path tracer handle
/// @return Sample count (incremented each render without clearAccumulation)
uint32_t tracey_path_tracer_get_sample_count(TraceyPathTracer* pathTracer);

// ============================================================================
// Error Handling
// ============================================================================

/// Get the last error message (thread-local)
/// @return Error message string, or NULL if no error
/// @note The returned string is valid until the next API call on this thread
const char* tracey_get_last_error(void);

/// Clear the last error message
void tracey_clear_last_error(void);

// ============================================================================
// Utility Functions
// ============================================================================

/// Get library version string
/// @return Version string (e.g., "1.0.0")
const char* tracey_get_version(void);

/// Create a default transform (identity)
TraceyTransform tracey_transform_identity(void);

/// Create a default camera
TraceyCamera tracey_camera_default(void);

#ifdef __cplusplus
}
#endif
