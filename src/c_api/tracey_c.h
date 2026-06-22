/*
 * tracey_c.h — Tracey path tracer C embedding API.
 *
 * This is the stable C boundary for embedding the Tracey path tracer in
 * third-party applications. It is pure C (C99) with a flat, dependency-free
 * ABI: opaque handles + POD structs only — no C++ types, no STL, no glm, and
 * deliberately NO OpenUSD / MaterialX / OIDN in the surface. Those remain
 * optional modules behind the renderer; the embedding boundary stays small.
 *
 * Typical use:
 *
 *     tracey_device   dev = tracey_device_create(TRACEY_BACKEND_AUTO);
 *     tracey_scene    scn = tracey_scene_create();
 *
 *     int mesh = tracey_scene_add_mesh(scn, "cube", positions, vcount,
 *                                      normals, uvs, indices, icount);
 *     tracey_material mat = tracey_material_default();
 *     mat.base_color[0] = 0.8f;
 *     tracey_scene_add_instance(scn, "cube", &mat, identity4x4);
 *
 *     tracey_camera cam = tracey_camera_default();
 *     // ... set cam.position / cam.target ...
 *     tracey_scene_set_camera(scn, &cam);
 *
 *     tracey_render_config cfg = tracey_render_config_default();
 *     cfg.width = 512; cfg.height = 512;
 *     tracey_renderer r = tracey_renderer_create(dev, &cfg);
 *
 *     tracey_render(r, scn, 64);                  // 64 samples, accumulated
 *     float *px = malloc(512 * 512 * 4 * sizeof(float));
 *     tracey_readback_beauty(r, px);              // RGBA32F (HDR)
 *
 *     tracey_renderer_destroy(r);
 *     tracey_scene_destroy(scn);
 *     tracey_device_destroy(dev);                 // device last
 *
 * Ownership / lifetime:
 *   - Handles are owned by the caller; destroy them with the matching
 *     *_destroy function. The renderer borrows the device — destroy the
 *     renderer BEFORE the device.
 *   - Mesh/instance/light/camera data passed to add_* functions is copied;
 *     the caller's buffers need not outlive the call.
 *
 * Errors:
 *   - Handle-returning functions return NULL on failure.
 *   - int-returning functions return >= 0 on success (often an id), < 0 on
 *     failure. size_t-returning readbacks return 0 on failure.
 *   - tracey_last_error() returns a human-readable description of the most
 *     recent failure (not thread-safe; intended for diagnostics).
 */

#ifndef TRACEY_C_H
#define TRACEY_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque handles ──────────────────────────────────────────────────────── */

typedef struct tracey_device_t   *tracey_device;
typedef struct tracey_scene_t     *tracey_scene;
typedef struct tracey_renderer_t  *tracey_renderer;

/* ── Enums ───────────────────────────────────────────────────────────────── */

/* Renderer backend. AUTO picks the best available on this machine
 * (Metal RT on Apple silicon, else the CPU reference). */
typedef enum tracey_backend
{
    TRACEY_BACKEND_AUTO   = 0,
    TRACEY_BACKEND_METAL  = 1,
    TRACEY_BACKEND_VULKAN = 2, /* stub today */
    TRACEY_BACKEND_CPU    = 3
} tracey_backend;

typedef enum tracey_light_type
{
    TRACEY_LIGHT_POINT   = 0,
    TRACEY_LIGHT_DISTANT = 1,
    TRACEY_LIGHT_DOME    = 2,
    TRACEY_LIGHT_AREA    = 3
} tracey_light_type;

/* Auxiliary render outputs (arbitrary output variables). Match the engine's
 * AovKind. Available only when render config enable_aovs is non-zero. */
typedef enum tracey_aov
{
    TRACEY_AOV_ALBEDO      = 0,
    TRACEY_AOV_NORMAL      = 1,
    TRACEY_AOV_DEPTH       = 2,
    TRACEY_AOV_POSITION    = 3,
    TRACEY_AOV_EMISSION    = 4,
    TRACEY_AOV_INSTANCE_ID = 5,
    TRACEY_AOV_COUNT       = 6
} tracey_aov;

/* ── POD parameter structs ───────────────────────────────────────────────── */

/* PBR material. All colors are linear RGB. Use tracey_material_default() to
 * obtain sensible defaults, then override fields. The advanced lobe factors
 * (clearcoat/sheen/subsurface/anisotropy) are 0 = off → standard look. */
typedef struct tracey_material
{
    float base_color[3];       /* albedo                       (default 0.8,0.8,0.8) */
    float metallic;            /* 0 dielectric .. 1 metal      (default 0)           */
    float roughness;           /* 0 mirror .. 1 diffuse        (default 0.5)         */
    float ior;                 /* index of refraction          (default 1.5)         */
    float opacity;             /* 1 opaque .. 0 transparent    (default 1)           */
    float transmission;        /* 0 opaque .. 1 glass          (default 0)           */
    float emission[3];         /* emissive color               (default 0,0,0)       */
    float emission_strength;   /* emissive multiplier          (default 1)           */
    float clearcoat;           /* 0 .. 1 coat layer            (default 0)           */
    float clearcoat_roughness; /* coat roughness               (default 0)           */
    float sheen;               /* 0 .. 1 fabric sheen          (default 0)           */
    float subsurface;          /* 0 .. 1 wrap-diffusion SSS    (default 0)           */
    float subsurface_color[3]; /* SSS scatter tint             (default 1,1,1)       */
    float anisotropy;          /* -1 .. 1 GGX anisotropy       (default 0)           */
} tracey_material;

/* Light source. Position + direction come from the 4x4 transform passed to
 * tracey_scene_add_light (translation = position; -Z column = direction). */
typedef struct tracey_light
{
    tracey_light_type type;
    float color[3];      /* linear RGB tint        (default 1,1,1) */
    float intensity;     /* scalar multiplier      (default 1)     */
    float radius;        /* point: soft radius     (default 0)     */
    float size[2];       /* area: width,height     (default 1,1)   */
    const char *hdri_path; /* dome: optional HDRI path, or NULL    */
} tracey_light;

/* Camera, specified as a look-at rig (most portable for embedders). */
typedef struct tracey_camera
{
    float position[3];   /* eye position                          */
    float target[3];     /* look-at point                         */
    float up[3];         /* up vector            (default 0,1,0)   */
    float fov_degrees;   /* vertical FOV         (default 45)      */
    float aperture;      /* lens aperture; 0 = pinhole (no DOF)    */
    float focal_distance;/* focus distance; <=0 = distance to target */
} tracey_camera;

/* Per-renderer configuration. Width/height are fixed for the renderer's
 * lifetime; create a new renderer to change resolution. */
typedef struct tracey_render_config
{
    uint32_t width;             /* (default 512)  */
    uint32_t height;            /* (default 512)  */
    uint32_t samples_per_frame; /* internal per render() call (default 1) */
    uint32_t max_bounces;       /* path depth     (default 8)  */
    int      hdr_output;        /* non-zero = RGBA32F readback; 0 = RGBA8 (default 1) */
    int      enable_aovs;       /* non-zero = compute AOV layers (default 0)          */
    tracey_backend backend;     /* (default TRACEY_BACKEND_AUTO) */
} tracey_render_config;

/* ── Defaults ────────────────────────────────────────────────────────────── */

tracey_material      tracey_material_default(void);
tracey_camera        tracey_camera_default(void);
tracey_render_config tracey_render_config_default(void);

/* ── Library ─────────────────────────────────────────────────────────────── */

/* Semantic version string of the linked Tracey build, e.g. "0.1.0". */
const char *tracey_version(void);

/* Human-readable description of the most recent failure, or "" if none.
 * Not thread-safe; intended for diagnostics. */
const char *tracey_last_error(void);

/* ── Device ──────────────────────────────────────────────────────────────── */

/* Create a compute device for the given backend. Returns NULL on failure. */
tracey_device tracey_device_create(tracey_backend backend);
void          tracey_device_destroy(tracey_device device);

/* ── Scene ───────────────────────────────────────────────────────────────── */

tracey_scene tracey_scene_create(void);
void         tracey_scene_destroy(tracey_scene scene);

/* Add a triangle mesh by unique name.
 *   positions   : vertex_count * 3 floats (xyz), required.
 *   normals     : vertex_count * 3 floats, or NULL (engine computes face normals).
 *   uvs         : vertex_count * 2 floats, or NULL.
 *   indices     : index_count uint32 (3 per triangle), required.
 * Returns 0 on success, < 0 on failure (e.g. duplicate name, empty geometry). */
int tracey_scene_add_mesh(tracey_scene scene, const char *name,
                          const float *positions, uint32_t vertex_count,
                          const float *normals, const float *uvs,
                          const uint32_t *indices, uint32_t index_count);

/* Instantiate a previously-added mesh with a material and a column-major 4x4
 * world transform (16 floats). If material is NULL, the mesh's default
 * material is used. Returns 0 on success, < 0 on failure. */
int tracey_scene_add_instance(tracey_scene scene, const char *mesh_name,
                              const tracey_material *material,
                              const float *transform4x4);

/* Add a light with a column-major 4x4 world transform (16 floats).
 * Returns 0 on success, < 0 on failure. */
int tracey_scene_add_light(tracey_scene scene, const tracey_light *light,
                           const float *transform4x4);

/* Set the scene camera. A camera is required before rendering. */
void tracey_scene_set_camera(tracey_scene scene, const tracey_camera *camera);

/* ── Renderer ────────────────────────────────────────────────────────────── */

/* Create a renderer on the device with the given config. The renderer borrows
 * the device (do not destroy the device first). Returns NULL on failure. */
tracey_renderer tracey_renderer_create(tracey_device device,
                                       const tracey_render_config *config);
void            tracey_renderer_destroy(tracey_renderer renderer);

/* Compile `scene` and render `sample_count` samples, accumulating into the
 * renderer's framebuffer (the accumulator is cleared at the start of the
 * call). The scene must have a camera set. Returns 0 on success, < 0 on
 * failure. After this returns, the readback functions observe the result. */
int tracey_render(tracey_renderer renderer, tracey_scene scene,
                  uint32_t sample_count);

/* Copy the beauty image into `out`. The caller must allocate
 * width*height*4*sizeof(float) bytes when hdr_output is set, else
 * width*height*4 bytes (RGBA8). Returns the number of bytes written, 0 on
 * failure. */
size_t tracey_readback_beauty(tracey_renderer renderer, void *out);

/* Copy an AOV layer into `out` (RGBA32F, width*height*4 floats). Requires the
 * renderer was created with enable_aovs. Returns bytes written, 0 if the AOV
 * is unavailable. */
size_t tracey_readback_aov(tracey_renderer renderer, tracey_aov aov, void *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TRACEY_C_H */
