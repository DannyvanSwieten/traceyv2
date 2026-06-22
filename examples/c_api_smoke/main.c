/*
 * c_api_smoke — exercises the Tracey C embedding API from PURE C (no C++).
 *
 * This is the acceptance test for the embedding boundary: it builds a lit cube
 * scene through tracey_c.h alone, renders it, reads back the beauty buffer, and
 * asserts the result is non-trivial (a real image, not a flat fill). If this
 * links and passes, a third-party C/C++/FFI consumer can drive the renderer
 * with nothing but the flat ABI.
 *
 * Usage: c_api_smoke [--backend cpu|metal|auto] [--size N] [--spp N] [--out f.ppm]
 * Exit code 0 on success, 1 on failure.
 */

#include "tracey_c.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Unit cube centered at origin. 8 shared vertices, 12 triangles. Normals are
 * omitted (NULL) so the engine computes per-face geometric normals. */
static const float kCubePositions[8 * 3] = {
    -1.f, -1.f, -1.f,  1.f, -1.f, -1.f,  1.f,  1.f, -1.f,  -1.f,  1.f, -1.f, /* z- */
    -1.f, -1.f,  1.f,  1.f, -1.f,  1.f,  1.f,  1.f,  1.f,  -1.f,  1.f,  1.f, /* z+ */
};
static const uint32_t kCubeIndices[12 * 3] = {
    0, 1, 2,  0, 2, 3, /* back   z- */
    5, 4, 7,  5, 7, 6, /* front  z+ */
    4, 0, 3,  4, 3, 7, /* left   x- */
    1, 5, 6,  1, 6, 2, /* right  x+ */
    3, 2, 6,  3, 6, 7, /* top    y+ */
    4, 5, 1,  4, 1, 0, /* bottom y- */
};

static void identity4x4(float *m)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void writePpm(const char *path, const float *rgba, uint32_t w, uint32_t h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (size_t i = 0; i < (size_t)w * h; ++i)
    {
        for (int c = 0; c < 3; ++c)
        {
            float v = rgba[i * 4 + c];
            if (v < 0.f) v = 0.f;
            if (v > 1.f) v = 1.f;
            fputc((int)(v * 255.0f + 0.5f), f);
        }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    tracey_backend backend = TRACEY_BACKEND_CPU; /* deterministic + portable default */
    uint32_t size = 256;
    uint32_t spp = 16;
    const char *out = NULL;

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc)
        {
            const char *b = argv[++i];
            if (strcmp(b, "metal") == 0) backend = TRACEY_BACKEND_METAL;
            else if (strcmp(b, "auto") == 0) backend = TRACEY_BACKEND_AUTO;
            else backend = TRACEY_BACKEND_CPU;
        }
        else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) size = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--spp") == 0 && i + 1 < argc) spp = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out = argv[++i];
    }

    printf("tracey C API smoke — version %s\n", tracey_version());
    printf("backend=%d size=%u spp=%u\n", (int)backend, size, spp);

    tracey_device dev = tracey_device_create(backend);
    if (!dev) { fprintf(stderr, "device_create failed: %s\n", tracey_last_error()); return 1; }

    tracey_scene scn = tracey_scene_create();
    if (!scn) { fprintf(stderr, "scene_create failed: %s\n", tracey_last_error()); return 1; }

    if (tracey_scene_add_mesh(scn, "cube", kCubePositions, 8,
                              NULL, NULL, kCubeIndices, 36) != 0)
    {
        fprintf(stderr, "add_mesh failed: %s\n", tracey_last_error());
        return 1;
    }

    /* A warm-ish dielectric cube. */
    tracey_material mat = tracey_material_default();
    mat.base_color[0] = 0.85f; mat.base_color[1] = 0.45f; mat.base_color[2] = 0.25f;
    mat.roughness = 0.4f;

    float xform[16];
    identity4x4(xform);
    if (tracey_scene_add_instance(scn, "cube", &mat, xform) != 0)
    {
        fprintf(stderr, "add_instance failed: %s\n", tracey_last_error());
        return 1;
    }

    /* Dome light (procedural sky) so the cube is clearly lit from all sides. */
    tracey_light dome;
    memset(&dome, 0, sizeof(dome));
    dome.type = TRACEY_LIGHT_DOME;
    dome.color[0] = dome.color[1] = dome.color[2] = 1.0f;
    dome.intensity = 1.0f;
    identity4x4(xform);
    if (tracey_scene_add_light(scn, &dome, xform) != 0)
    {
        fprintf(stderr, "add_light failed: %s\n", tracey_last_error());
        return 1;
    }

    /* Three-quarter view of the cube. */
    tracey_camera cam = tracey_camera_default();
    cam.position[0] = 3.5f; cam.position[1] = 3.0f; cam.position[2] = 5.0f;
    cam.target[0] = 0.0f;   cam.target[1] = 0.0f;   cam.target[2] = 0.0f;
    cam.fov_degrees = 40.0f;
    tracey_scene_set_camera(scn, &cam);

    tracey_render_config cfg = tracey_render_config_default();
    cfg.width = size;
    cfg.height = size;
    cfg.max_bounces = 6;
    cfg.hdr_output = 1;
    cfg.enable_aovs = 1; /* exercise the AOV readback path too */
    cfg.backend = backend;

    tracey_renderer r = tracey_renderer_create(dev, &cfg);
    if (!r) { fprintf(stderr, "renderer_create failed: %s\n", tracey_last_error()); return 1; }

    if (tracey_render(r, scn, spp) != 0)
    {
        fprintf(stderr, "render failed: %s\n", tracey_last_error());
        return 1;
    }

    const size_t pixels = (size_t)size * size;
    float *beauty = (float *)malloc(pixels * 4 * sizeof(float));
    if (!beauty) { fprintf(stderr, "oom\n"); return 1; }

    size_t bytes = tracey_readback_beauty(r, beauty);
    if (bytes == 0)
    {
        fprintf(stderr, "readback_beauty failed: %s\n", tracey_last_error());
        return 1;
    }

    /* Validate the image is non-trivial: compute luminance min/max/mean and
     * count how many pixels differ meaningfully from the mean. A flat fill
     * (all-black, or a uniform error color) would fail these checks. */
    double lmin = 1e30, lmax = -1e30, lsum = 0.0;
    size_t lit = 0;
    for (size_t i = 0; i < pixels; ++i)
    {
        const float rr = beauty[i * 4 + 0], gg = beauty[i * 4 + 1], bb = beauty[i * 4 + 2];
        const double lum = 0.2126 * rr + 0.7152 * gg + 0.0722 * bb;
        if (lum < lmin) lmin = lum;
        if (lum > lmax) lmax = lum;
        lsum += lum;
        if (lum > 0.02) ++lit;
    }
    const double lmean = lsum / (double)pixels;
    printf("beauty: %zu bytes, lum min=%.4f max=%.4f mean=%.4f, lit=%.1f%%\n",
           bytes, lmin, lmax, lmean, 100.0 * (double)lit / (double)pixels);

    int ok = 1;
    if (bytes != pixels * 4 * sizeof(float)) { fprintf(stderr, "FAIL: unexpected readback size\n"); ok = 0; }
    if (lmax - lmin < 0.01)                  { fprintf(stderr, "FAIL: image is flat (no contrast)\n"); ok = 0; }
    if (lmean <= 0.0)                        { fprintf(stderr, "FAIL: image is black\n"); ok = 0; }
    if (lit == 0)                            { fprintf(stderr, "FAIL: no lit pixels\n"); ok = 0; }
    /* Note: lit==100%% is expected here — the dome light fills the background,
     * so every pixel is illuminated. Contrast (max-min) is the real proof that
     * the cube is distinguishable from the sky. */

    /* AOV path: the albedo layer should carry the cube's base color on the
     * pixels the cube covers. Validate it reads back and is non-trivial. */
    float *albedo = (float *)malloc(pixels * 4 * sizeof(float));
    if (albedo)
    {
        size_t abytes = tracey_readback_aov(r, TRACEY_AOV_ALBEDO, albedo);
        if (abytes == pixels * 4 * sizeof(float))
        {
            size_t cube_px = 0;
            for (size_t i = 0; i < pixels; ++i)
            {
                /* cube base_color was (0.85, 0.45, 0.25) — match loosely */
                const float rr = albedo[i * 4 + 0], gg = albedo[i * 4 + 1], bb = albedo[i * 4 + 2];
                if (fabsf(rr - 0.85f) < 0.1f && fabsf(gg - 0.45f) < 0.1f && fabsf(bb - 0.25f) < 0.1f)
                    ++cube_px;
            }
            printf("albedo AOV: %zu bytes, %zu px match cube base color\n", abytes, cube_px);
            if (cube_px == 0) { fprintf(stderr, "FAIL: albedo AOV has no cube pixels\n"); ok = 0; }
        }
        else
        {
            fprintf(stderr, "FAIL: albedo AOV readback size %zu\n", abytes);
            ok = 0;
        }
        free(albedo);
    }

    if (out) { writePpm(out, beauty, size, size); printf("wrote %s\n", out); }

    free(beauty);
    tracey_renderer_destroy(r);
    tracey_scene_destroy(scn);
    tracey_device_destroy(dev); /* device destroyed last */

    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
