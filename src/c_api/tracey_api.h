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
typedef struct TraceyRasterizer TraceyRasterizer;

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

/// Wait for all GPU operations on the device to complete
/// @param device Device handle
void tracey_device_wait_idle(TraceyDevice* device);

// ============================================================================
// Scene Management
// ============================================================================

/// Create an empty scene
/// @return Scene handle, or NULL on failure
TraceyScene* tracey_scene_create(void);

/// Destroy a scene and free all associated resources
/// @param scene Scene handle (can be NULL)
void tracey_scene_destroy(TraceyScene* scene);

/// Clear all actors and objects from the scene (but keep the scene itself)
/// @param scene Scene handle
void tracey_scene_clear(TraceyScene* scene);

/// Create a new actor in the scene
/// @param scene Scene handle
/// @param name Actor name (UTF-8 string, can be NULL for unnamed actor)
/// @return Actor UID (unique identifier), or UINT64_MAX on failure
uint64_t tracey_scene_create_actor(
    TraceyScene* scene,
    const char* name
);

/// Remove an actor from the scene
/// @param scene Scene handle
/// @param actorUid Actor unique identifier to remove
/// @return TRACEY_SUCCESS or error code
TraceyResult tracey_scene_remove_actor(
    TraceyScene* scene,
    uint64_t actorUid
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

/// Load a GLTF/GLB file into the scene with project root for asset resolution
/// @param scene Scene handle
/// @param filePath Path to GLTF/GLB file
/// @param projectRoot Optional project root directory for asset path resolution (can be NULL)
/// @return TRACEY_SUCCESS or error code
TraceyResult tracey_scene_load_gltf_with_project(
    TraceyScene* scene,
    const char* filePath,
    const char* projectRoot
);

/// Add a GLTF/GLB file to the scene without clearing existing actors
/// @param scene Scene handle
/// @param filePath Path to GLTF/GLB file
/// @param projectRoot Optional project root directory for asset path resolution (can be NULL)
/// @return TRACEY_SUCCESS or error code
TraceyResult tracey_scene_add_gltf_with_project(
    TraceyScene* scene,
    const char* filePath,
    const char* projectRoot
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
// Scene Query Functions
// ============================================================================

/// Get actor name
/// @param scene Scene handle
/// @param actorUid Actor unique identifier
/// @return Actor name (valid until scene is modified), or NULL on error
const char* tracey_scene_get_actor_name(
    TraceyScene* scene,
    uint64_t actorUid
);

/// Get actor children UIDs
/// @param scene Scene handle
/// @param actorUid Actor unique identifier
/// @param outUids Buffer to receive child UIDs
/// @param maxCount Maximum number of UIDs to write
/// @return Number of children written
uint32_t tracey_scene_get_actor_children(
    TraceyScene* scene,
    uint64_t actorUid,
    uint64_t* outUids,
    uint32_t maxCount
);

/// Get actor instance count
/// @param scene Scene handle
/// @param actorUid Actor unique identifier
/// @return Number of instances on this actor
uint32_t tracey_scene_get_actor_instance_count(
    TraceyScene* scene,
    uint64_t actorUid
);

/// Get actor instance info
/// @param scene Scene handle
/// @param actorUid Actor unique identifier
/// @param instanceIndex Index of the instance
/// @param outInfo Pointer to receive instance info
/// @return TRACEY_SUCCESS or error code
TraceyResult tracey_scene_get_actor_instance(
    TraceyScene* scene,
    uint64_t actorUid,
    uint32_t instanceIndex,
    TraceyInstanceInfo* outInfo
);

/// Get number of mesh objects in scene
/// @param scene Scene handle
/// @return Number of mesh objects
uint32_t tracey_scene_get_mesh_count(TraceyScene* scene);

/// Get mesh names
/// @param scene Scene handle
/// @param outNames Buffer to receive mesh name pointers
/// @param maxCount Maximum number of names to write
/// @return Number of names written
uint32_t tracey_scene_get_mesh_names(
    TraceyScene* scene,
    const char** outNames,
    uint32_t maxCount
);

/// Get mesh info by name
/// @param scene Scene handle
/// @param name Mesh name
/// @param outInfo Pointer to receive mesh info
/// @return TRACEY_SUCCESS or error code
TraceyResult tracey_scene_get_mesh_info(
    TraceyScene* scene,
    const char* name,
    TraceyMeshInfo* outInfo
);

/// Get number of embedded textures in scene
/// @param scene Scene handle
/// @return Number of embedded textures
uint32_t tracey_scene_get_texture_count(TraceyScene* scene);

/// Get texture IDs
/// @param scene Scene handle
/// @param outIds Buffer to receive texture ID pointers
/// @param maxCount Maximum number of IDs to write
/// @return Number of IDs written
uint32_t tracey_scene_get_texture_ids(
    TraceyScene* scene,
    const char** outIds,
    uint32_t maxCount
);

/// Get embedded texture info by ID
/// @param scene Scene handle
/// @param id Texture ID
/// @param outInfo Pointer to receive texture info
/// @return TRACEY_SUCCESS or error code
TraceyResult tracey_scene_get_texture_info(
    TraceyScene* scene,
    const char* id,
    TraceyTextureInfo* outInfo
);

// ============================================================================
// Material Editing Functions
// ============================================================================

/// Get the number of material properties on an actor's instance
/// @param scene Scene handle
/// @param actorUid Actor unique identifier
/// @param instanceIndex Index of the instance
/// @return Number of properties, or 0 on error
uint32_t tracey_scene_get_instance_material_property_count(
    TraceyScene* scene,
    uint64_t actorUid,
    uint32_t instanceIndex
);

/// Get material property by name
/// @param scene Scene handle
/// @param actorUid Actor unique identifier
/// @param instanceIndex Index of the instance
/// @param propertyName Name of the property
/// @param outProperty Pointer to receive property data
/// @return TRACEY_SUCCESS or error code
TraceyResult tracey_scene_get_instance_material_property_by_name(
    TraceyScene* scene,
    uint64_t actorUid,
    uint32_t instanceIndex,
    const char* propertyName,
    TraceyMaterialProperty* outProperty
);

/// Set a float material property
/// @param scene Scene handle
/// @param actorUid Actor unique identifier
/// @param instanceIndex Index of the instance
/// @param propertyName Name of the property
/// @param value Float value
/// @return TRACEY_SUCCESS or error code
TraceyResult tracey_scene_set_instance_material_float(
    TraceyScene* scene,
    uint64_t actorUid,
    uint32_t instanceIndex,
    const char* propertyName,
    float value
);

/// Set a Vec3 material property (for albedo, emission, etc.)
/// @param scene Scene handle
/// @param actorUid Actor unique identifier
/// @param instanceIndex Index of the instance
/// @param propertyName Name of the property
/// @param value Vec3 value
/// @return TRACEY_SUCCESS or error code
TraceyResult tracey_scene_set_instance_material_vec3(
    TraceyScene* scene,
    uint64_t actorUid,
    uint32_t instanceIndex,
    const char* propertyName,
    TraceyVec3 value
);

/// Set a Vec4 material property (for RGBA values)
/// @param scene Scene handle
/// @param actorUid Actor unique identifier
/// @param instanceIndex Index of the instance
/// @param propertyName Name of the property
/// @param value Vec4 value
/// @return TRACEY_SUCCESS or error code
TraceyResult tracey_scene_set_instance_material_vec4(
    TraceyScene* scene,
    uint64_t actorUid,
    uint32_t instanceIndex,
    const char* propertyName,
    TraceyVec4 value
);

/// Set a texture material property
/// @param scene Scene handle
/// @param actorUid Actor unique identifier
/// @param instanceIndex Index of the instance
/// @param propertyName Name of the property (e.g., "albedoMap")
/// @param texturePath Path to texture or embedded texture ID
/// @return TRACEY_SUCCESS or error code
TraceyResult tracey_scene_set_instance_material_texture(
    TraceyScene* scene,
    uint64_t actorUid,
    uint32_t instanceIndex,
    const char* propertyName,
    const char* texturePath
);

/// Get the shader ID for an instance's material
/// @param scene Scene handle
/// @param actorUid Actor unique identifier
/// @param instanceIndex Index of the instance
/// @return Shader ID string (valid until scene is modified), or NULL on error
const char* tracey_scene_get_instance_material_shader_id(
    TraceyScene* scene,
    uint64_t actorUid,
    uint32_t instanceIndex
);

// ============================================================================
// Primitive Creation Functions
// ============================================================================

/// Add a cube primitive to the scene
/// @param scene Scene handle
/// @param name Name for the mesh object
/// @param size Cube size (default 1.0)
/// @return Actor UID for the created primitive, or UINT64_MAX on failure
uint64_t tracey_scene_add_cube(
    TraceyScene* scene,
    const char* name,
    float size
);

/// Add a sphere primitive to the scene
/// @param scene Scene handle
/// @param name Name for the mesh object
/// @param radius Sphere radius (default 1.0)
/// @param segments Number of horizontal segments (default 16)
/// @param rings Number of vertical rings (default 16)
/// @return Actor UID for the created primitive, or UINT64_MAX on failure
uint64_t tracey_scene_add_sphere(
    TraceyScene* scene,
    const char* name,
    float radius,
    uint32_t segments,
    uint32_t rings
);

/// Add a torus primitive to the scene
/// @param scene Scene handle
/// @param name Name for the mesh object
/// @param majorRadius Distance from center to tube center (default 1.0)
/// @param minorRadius Radius of the tube (default 0.3)
/// @param majorSegments Segments around the torus (default 32)
/// @param minorSegments Segments around the tube (default 16)
/// @return Actor UID for the created primitive, or UINT64_MAX on failure
uint64_t tracey_scene_add_torus(
    TraceyScene* scene,
    const char* name,
    float majorRadius,
    float minorRadius,
    uint32_t majorSegments,
    uint32_t minorSegments
);

/// Add a plane primitive to the scene
/// @param scene Scene handle
/// @param name Name for the mesh object
/// @param width Width of the plane (default 1.0)
/// @param depth Depth of the plane (default 1.0)
/// @return Actor UID for the created primitive, or UINT64_MAX on failure
uint64_t tracey_scene_add_plane(
    TraceyScene* scene,
    const char* name,
    float width,
    float depth
);

/// Add a cylinder primitive to the scene
/// @param scene Scene handle
/// @param name Name for the mesh object
/// @param radius Cylinder radius (default 0.5)
/// @param height Cylinder height (default 1.0)
/// @param segments Number of segments around the cylinder (default 32)
/// @return Actor UID for the created primitive, or UINT64_MAX on failure
uint64_t tracey_scene_add_cylinder(
    TraceyScene* scene,
    const char* name,
    float radius,
    float height,
    uint32_t segments
);

/// Add a cone primitive to the scene
/// @param scene Scene handle
/// @param name Name for the mesh object
/// @param radius Cone base radius (default 0.5)
/// @param height Cone height (default 1.0)
/// @param segments Number of segments around the cone (default 32)
/// @return Actor UID for the created primitive, or UINT64_MAX on failure
uint64_t tracey_scene_add_cone(
    TraceyScene* scene,
    const char* name,
    float radius,
    float height,
    uint32_t segments
);

// ============================================================================
// Primitive Creation with Pre-assigned Actor ID
// These functions create primitives using a pre-assigned actor ID from Rust
// ============================================================================

/// Add a cube primitive to an existing actor
/// @param scene Scene handle
/// @param actorUid Pre-assigned actor UID
/// @param name Name for the mesh object
/// @param size Cube size
/// @return TRACEY_SUCCESS or error code
TraceyResult tracey_scene_add_cube_with_id(
    TraceyScene* scene,
    uint64_t actorUid,
    const char* name,
    float size
);

/// Add a sphere primitive to an existing actor
TraceyResult tracey_scene_add_sphere_with_id(
    TraceyScene* scene,
    uint64_t actorUid,
    const char* name,
    float radius,
    uint32_t segments,
    uint32_t rings
);

/// Add a torus primitive to an existing actor
TraceyResult tracey_scene_add_torus_with_id(
    TraceyScene* scene,
    uint64_t actorUid,
    const char* name,
    float majorRadius,
    float minorRadius,
    uint32_t majorSegments,
    uint32_t minorSegments
);

/// Add a plane primitive to an existing actor
TraceyResult tracey_scene_add_plane_with_id(
    TraceyScene* scene,
    uint64_t actorUid,
    const char* name,
    float width,
    float depth
);

/// Add a cylinder primitive to an existing actor
TraceyResult tracey_scene_add_cylinder_with_id(
    TraceyScene* scene,
    uint64_t actorUid,
    const char* name,
    float radius,
    float height,
    uint32_t segments
);

/// Add a cone primitive to an existing actor
TraceyResult tracey_scene_add_cone_with_id(
    TraceyScene* scene,
    uint64_t actorUid,
    const char* name,
    float radius,
    float height,
    uint32_t segments
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

/// Update only instance transforms in a compiled scene (fast update for animations)
/// This rebuilds the TLAS but keeps BLASes, vertex buffers, materials, and textures
/// @param device Device to use
/// @param scene Scene with updated transforms
/// @param compiledScene Existing compiled scene to update
/// @return 0 on success, -1 on failure
int tracey_update_scene_transforms(
    TraceyDevice* device,
    TraceyScene* scene,
    TraceyCompiledScene* compiledScene
);

/// Update only material properties in a compiled scene (fast update for material editing)
/// This re-uploads material data to GPU but keeps TLAS, BLASes, vertex buffers, and textures
/// @param device Device to use
/// @param scene Scene with updated material values
/// @param compiledScene Existing compiled scene to update
/// @return 0 on success, -1 on failure
int tracey_update_scene_materials(
    TraceyDevice* device,
    TraceyScene* scene,
    TraceyCompiledScene* compiledScene
);

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

/// Get samples per frame setting
/// @param pathTracer Path tracer handle
/// @return Samples per frame
uint32_t tracey_path_tracer_get_samples_per_frame(TraceyPathTracer* pathTracer);

/// Set samples per frame setting
/// @param pathTracer Path tracer handle
/// @param samples New samples per frame value
/// @return TRACEY_SUCCESS or error code
TraceyResult tracey_path_tracer_set_samples_per_frame(TraceyPathTracer* pathTracer, uint32_t samples);

/// Get max bounces setting
/// @param pathTracer Path tracer handle
/// @return Max bounces (ray depth)
uint32_t tracey_path_tracer_get_max_bounces(TraceyPathTracer* pathTracer);

/// Set max bounces setting
/// @param pathTracer Path tracer handle
/// @param bounces New max bounces value
/// @return TRACEY_SUCCESS or error code
TraceyResult tracey_path_tracer_set_max_bounces(TraceyPathTracer* pathTracer, uint32_t bounces);

// ============================================================================
// Rasterizer
// ============================================================================

/// Create a rasterizer for realtime rendering
/// @param device Device to use for rendering
/// @param config Rasterizer configuration (shaders, resolution, etc.)
/// @return Rasterizer handle, or NULL on failure
TraceyRasterizer* tracey_rasterizer_create(
    TraceyDevice* device,
    const TraceyRasterizerConfig* config
);

/// Destroy a rasterizer and free resources
/// @param rasterizer Rasterizer handle (can be NULL)
void tracey_rasterizer_destroy(TraceyRasterizer* rasterizer);

/// Render a frame using rasterization
/// @param rasterizer Rasterizer handle
/// @param compiledScene Compiled scene to render
/// @param camera Camera to render from
/// @return Render time in milliseconds, or negative value on error
double tracey_rasterizer_render(
    TraceyRasterizer* rasterizer,
    TraceyCompiledScene* compiledScene,
    const TraceyCamera* camera
);

/// Read back rendered image to CPU memory
/// @param rasterizer Rasterizer handle
/// @param outBuffer Buffer to receive pixel data (must be large enough)
/// @param bufferSize Size of outBuffer in bytes
/// @return Number of bytes written, or 0 on error
/// @note Output: width * height * 4 bytes (R8G8B8A8)
size_t tracey_rasterizer_readback(
    TraceyRasterizer* rasterizer,
    void* outBuffer,
    size_t bufferSize
);

/// Get rasterizer output resolution
/// @param rasterizer Rasterizer handle
/// @param outWidth Pointer to receive width
/// @param outHeight Pointer to receive height
/// @return TRACEY_SUCCESS or error code
TraceyResult tracey_rasterizer_get_resolution(
    TraceyRasterizer* rasterizer,
    uint32_t* outWidth,
    uint32_t* outHeight
);

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

// ============================================================================
// Native Window Presentation
// ============================================================================

/// Opaque presenter handle for native window presentation
typedef struct TraceyPresenter TraceyPresenter;

/// Presenter configuration
typedef struct TraceyPresenterConfig {
    uint32_t width;                 ///< Window width in pixels
    uint32_t height;                ///< Window height in pixels
    bool enableHDR;                 ///< Enable HDR if available
    uint32_t desiredImageCount;     ///< Number of swapchain images (2=double, 3=triple)
} TraceyPresenterConfig;

/// Viewport bounds for region-based presentation
typedef struct TraceyViewportBounds {
    int32_t x;                      ///< Viewport X position in window
    int32_t y;                      ///< Viewport Y position in window
    uint32_t width;                 ///< Viewport width
    uint32_t height;                ///< Viewport height
} TraceyViewportBounds;

/// Create a presenter for native window
/// @param device The compute device (must be GPU-based Vulkan device)
/// @param nativeWindowHandle Platform-specific window handle:
///   - macOS: CAMetalLayer* (from NSView's layer)
///   - Windows: HWND
///   - Linux X11: Window (cast to void*)
///   - Linux Wayland: wl_surface*
/// @param nativeDisplayHandle Platform-specific display handle:
///   - macOS: NULL (not needed)
///   - Windows: HINSTANCE (from GetModuleHandle)
///   - Linux X11: Display*
///   - Linux Wayland: wl_display*
/// @param config Presenter configuration
/// @return Presenter instance, or NULL on failure
TraceyPresenter* tracey_presenter_create(
    TraceyDevice* device,
    void* nativeWindowHandle,
    void* nativeDisplayHandle,
    const TraceyPresenterConfig* config
);

/// Destroy a presenter
/// @param presenter The presenter to destroy
void tracey_presenter_destroy(TraceyPresenter* presenter);

/// Present PathTracer output to window
/// @param presenter The presenter instance
/// @param pathTracer The path tracer whose output to present
/// @return TRACEY_SUCCESS on success, error code on failure
TraceyResult tracey_presenter_present_pathtracer(
    TraceyPresenter* presenter,
    TraceyPathTracer* pathTracer
);

/// Present Rasterizer output to window
/// @param presenter The presenter instance
/// @param rasterizer The rasterizer whose output to present
/// @return TRACEY_SUCCESS on success, error code on failure
TraceyResult tracey_presenter_present_rasterizer(
    TraceyPresenter* presenter,
    TraceyRasterizer* rasterizer
);

/// Present PathTracer output to a specific region of the window
/// Used for viewport-based rendering with full-window swapchain
/// @param presenter The presenter instance
/// @param pathTracer The path tracer whose output to present
/// @param bounds Viewport bounds within the window
/// @return TRACEY_SUCCESS on success, error code on failure
TraceyResult tracey_presenter_present_pathtracer_to_region(
    TraceyPresenter* presenter,
    TraceyPathTracer* pathTracer,
    const TraceyViewportBounds* bounds
);

/// Present Rasterizer output to a specific region of the window
/// Used for viewport-based rendering with full-window swapchain
/// @param presenter The presenter instance
/// @param rasterizer The rasterizer whose output to present
/// @param bounds Viewport bounds within the window
/// @return TRACEY_SUCCESS on success, error code on failure
TraceyResult tracey_presenter_present_rasterizer_to_region(
    TraceyPresenter* presenter,
    TraceyRasterizer* rasterizer,
    const TraceyViewportBounds* bounds
);

/// Resize the presenter's swapchain
/// @param presenter The presenter instance
/// @param newWidth New width in pixels
/// @param newHeight New height in pixels
/// @return TRACEY_SUCCESS on success, error code on failure
TraceyResult tracey_presenter_resize(
    TraceyPresenter* presenter,
    uint32_t newWidth,
    uint32_t newHeight
);

/// Wait for all presentation operations to complete
/// @param presenter The presenter instance
void tracey_presenter_wait_idle(TraceyPresenter* presenter);

// ============================================================================
// Procedural Node System (Phase 1)
// ============================================================================

/// Opaque handles for node system
typedef struct TraceyNodeGraph TraceyNodeGraph;
typedef struct TraceyNode TraceyNode;
typedef struct TraceyParameter TraceyParameter;

/// Node type enumeration
typedef enum TraceyNodeType {
    TRACEY_NODE_ACTOR = 0,
    TRACEY_NODE_PRIMITIVE_CUBE = 1,
    TRACEY_NODE_PRIMITIVE_SPHERE = 2,
    TRACEY_NODE_PRIMITIVE_TORUS = 3,
    TRACEY_NODE_PRIMITIVE_PLANE = 4,
    TRACEY_NODE_PRIMITIVE_CYLINDER = 5,
    TRACEY_NODE_PRIMITIVE_CONE = 6,
    TRACEY_NODE_GEOMETRY_TRANSFORM = 7,
    TRACEY_NODE_GEOMETRY_MERGE = 8,
    TRACEY_NODE_MATERIAL = 9,
    TRACEY_NODE_MATH_FLOAT = 10,
    TRACEY_NODE_MATH_VECTOR = 11
} TraceyNodeType;

/// Parameter type enumeration
typedef enum TraceyParameterType {
    TRACEY_PARAM_FLOAT = 0,
    TRACEY_PARAM_VEC2 = 1,
    TRACEY_PARAM_VEC3 = 2,
    TRACEY_PARAM_VEC4 = 3,
    TRACEY_PARAM_INT = 4,
    TRACEY_PARAM_BOOL = 5,
    TRACEY_PARAM_STRING = 6,
    TRACEY_PARAM_COLOR = 7,
    TRACEY_PARAM_TEXTURE = 8
} TraceyParameterType;

/// Get the scene's node graph
/// @param scene Scene handle
/// @return Node graph handle, or NULL if scene is invalid
TraceyNodeGraph* tracey_scene_get_node_graph(TraceyScene* scene);

/// Create a node in the graph
/// @param graph Node graph handle
/// @param type Type of node to create
/// @param name Node name (can be NULL for auto-generated name)
/// @return Node UID, or UINT64_MAX on failure
uint64_t tracey_node_graph_create_node(
    TraceyNodeGraph* graph,
    TraceyNodeType type,
    const char* name
);

/// Get a node by UID
/// @param graph Node graph handle
/// @param nodeUid Node UID
/// @return Node handle, or NULL if not found
TraceyNode* tracey_node_graph_get_node(
    TraceyNodeGraph* graph,
    uint64_t nodeUid
);

/// Remove a node from the graph
/// @param graph Node graph handle
/// @param nodeUid Node UID to remove
/// @return TRACEY_SUCCESS on success, error code on failure
TraceyResult tracey_node_graph_remove_node(
    TraceyNodeGraph* graph,
    uint64_t nodeUid
);

/// Get node type
/// @param node Node handle
/// @return Node type
TraceyNodeType tracey_node_get_type(TraceyNode* node);

/// Get node name
/// @param node Node handle
/// @return Node name (valid until node is modified/destroyed)
const char* tracey_node_get_name(TraceyNode* node);

/// Set node name
/// @param node Node handle
/// @param name New name for the node
/// @return TRACEY_SUCCESS on success, error code on failure
TraceyResult tracey_node_set_name(TraceyNode* node, const char* name);

/// Get node UID
/// @param node Node handle
/// @return Node UID
uint64_t tracey_node_get_uid(TraceyNode* node);

/// Get a parameter by name
/// @param node Node handle
/// @param paramName Parameter name
/// @return Parameter handle, or NULL if not found
TraceyParameter* tracey_node_get_parameter(
    TraceyNode* node,
    const char* paramName
);

/// Get parameter count
/// @param node Node handle
/// @return Number of parameters on the node
uint32_t tracey_node_get_parameter_count(TraceyNode* node);

/// Get parameter type
/// @param param Parameter handle
/// @return Parameter type
TraceyParameterType tracey_parameter_get_type(TraceyParameter* param);

/// Get parameter name
/// @param param Parameter handle
/// @return Parameter name (valid until parameter is destroyed)
const char* tracey_parameter_get_name(TraceyParameter* param);

/// Set parameter value (float)
/// @param param Parameter handle
/// @param value Float value
/// @return TRACEY_SUCCESS on success, error code on failure
TraceyResult tracey_parameter_set_float(TraceyParameter* param, float value);

/// Set parameter value (vec3)
/// @param param Parameter handle
/// @param value Vec3 value
/// @return TRACEY_SUCCESS on success, error code on failure
TraceyResult tracey_parameter_set_vec3(TraceyParameter* param, TraceyVec3 value);

/// Set parameter value (int)
/// @param param Parameter handle
/// @param value Integer value
/// @return TRACEY_SUCCESS on success, error code on failure
TraceyResult tracey_parameter_set_int(TraceyParameter* param, int value);

/// Set parameter value (bool)
/// @param param Parameter handle
/// @param value Boolean value (0 = false, non-zero = true)
/// @return TRACEY_SUCCESS on success, error code on failure
TraceyResult tracey_parameter_set_bool(TraceyParameter* param, int value);

/// Get parameter value (float)
/// @param param Parameter handle
/// @param outValue Pointer to receive the value
/// @return TRACEY_SUCCESS on success, error code on failure
TraceyResult tracey_parameter_get_float(TraceyParameter* param, float* outValue);

/// Get parameter value (vec3)
/// @param param Parameter handle
/// @param outValue Pointer to receive the value
/// @return TRACEY_SUCCESS on success, error code on failure
TraceyResult tracey_parameter_get_vec3(TraceyParameter* param, TraceyVec3* outValue);

/// Get parameter value (int)
/// @param param Parameter handle
/// @param outValue Pointer to receive the value
/// @return TRACEY_SUCCESS on success, error code on failure
TraceyResult tracey_parameter_get_int(TraceyParameter* param, int* outValue);

/// Get parameter value (bool)
/// @param param Parameter handle
/// @param outValue Pointer to receive the value (0 = false, 1 = true)
/// @return TRACEY_SUCCESS on success, error code on failure
TraceyResult tracey_parameter_get_bool(TraceyParameter* param, int* outValue);

#ifdef __cplusplus
}
#endif
