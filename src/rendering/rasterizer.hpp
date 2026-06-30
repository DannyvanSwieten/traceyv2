#pragma once

#include "../scene/scene_compiler.hpp"
#include "../scene/camera.hpp"
#include "../core/types.hpp"
#include "../device/device.hpp"
#include "../device/image_2d.hpp"
#include "graphics_pipeline.hpp"
#include "graphics_pipeline_layout.hpp"
#include "graphics_command_buffer.hpp"

#include <memory>
#include <vector>
#include <filesystem>
#include <volk.h>

namespace tracey
{
    /// Configuration for rasterization renderer
    struct RasterizerConfig
    {
        // Output resolution
        uint32_t width = 1280;
        uint32_t height = 720;

        // Shader file paths
        std::filesystem::path vertexShader;
        std::filesystem::path fragmentShader;

        // Optional secondary point-sprite pass overlaid in the same render
        // pass. When set, callers can toggle `setShowPoints(true)` at runtime
        // to enable the second draw.
        std::filesystem::path pointsVertexShader;
        std::filesystem::path pointsFragmentShader;

        // Optional tertiary wireframe (POLYGON_MODE_LINE) pass overlaid in
        // the same render pass. Toggled at runtime via setShowEdges.
        std::filesystem::path linesVertexShader;
        std::filesystem::path linesFragmentShader;

        // Optional reference ground-grid pass on the y=0 plane. Toggled at
        // runtime via setShowGround.
        std::filesystem::path groundVertexShader;
        std::filesystem::path groundFragmentShader;

        // Optional translate-gizmo overlay (three colored world-axis lines
        // anchored at the selected actor). Toggled at runtime via
        // setGizmoVisible; anchor + length via setGizmoAnchor.
        std::filesystem::path gizmoVertexShader;
        std::filesystem::path gizmoFragmentShader;

        // Optional composition-guides overlay (rule-of-thirds / safe-area
        // lines in NDC). Toggled at runtime via setCompositionGuides.
        std::filesystem::path guidesVertexShader;
        std::filesystem::path guidesFragmentShader;

        // Optional skeleton overlay (world-space bone line list). Fed at
        // runtime via setBoneSegments.
        std::filesystem::path bonesVertexShader;
        std::filesystem::path bonesFragmentShader;

        // Rendering options
        bool useDepthBuffer = true;
        bool depthTestEnable = true;
        bool cullBackFaces = true;
        bool alphaBlending = false;

        // Output format
        ImageFormat colorFormat = ImageFormat::R8G8B8A8Unorm;
    };

    /// High-level rasterization renderer for realtime preview
    /// Provides fast rendering using traditional graphics pipeline
    class Rasterizer
    {
    public:
        /// Construct a rasterizer with rendering configuration
        /// Scene is NOT coupled at construction - passed to render() instead
        /// @param device The device to use for rendering (not owned)
        /// @param config Rendering configuration (shaders, resolution, etc.)
        Rasterizer(Device *device, const RasterizerConfig &config);

        ~Rasterizer();

        // Non-copyable, movable
        Rasterizer(const Rasterizer &) = delete;
        Rasterizer &operator=(const Rasterizer &) = delete;
        Rasterizer(Rasterizer &&) = default;
        Rasterizer &operator=(Rasterizer &&) = default;

        /// Render a scene with the given camera
        /// @param scene The compiled scene to render
        /// @param camera The camera to render from
        /// @return Rendering time in milliseconds
        // `sceneGeneration` identifies the compiled scene's revision (bumped by
        // compile_scene / refresh_tlas_only). The per-instance GPU buffers are
        // cached and only rebuilt when it changes — during camera-only navigation
        // it's stable, so we skip the per-frame allocate+upload. Pass 0 to force a
        // rebuild every frame (legacy behaviour).
        double render(const SceneCompiler::CompiledScene &scene,
                      const Camera &camera,
                      uint64_t sceneGeneration = 0);

        /// Get the output image
        /// Valid after calling render(). The pipeline owns this image; the
        /// rasterizer just exposes a view.
        Image2D *outputImage() const;

        /// Read back the output image to CPU memory
        /// @param outData Pointer to receive the image data (caller must allocate width*height*4*sizeof(uint8_t))
        /// @return Size of data copied in bytes
        size_t readback(void *outData);

        /// Get current resolution
        uint32_t width() const { return m_config.width; }
        uint32_t height() const { return m_config.height; }

        /// Toggle the secondary points overlay (alpha-blended circle splats
        /// drawn after the triangle pass). No-op if the points pipeline
        /// wasn't configured at construction.
        void setShowPoints(bool v) { m_showPoints = v; }
        bool showPoints() const { return m_showPoints; }

        /// Toggle the wireframe overlay (triangle edges via POLYGON_MODE_LINE).
        /// No-op if the lines pipeline wasn't configured at construction.
        void setShowEdges(bool v) { m_showEdges = v; }
        bool showEdges() const { return m_showEdges; }

        /// Toggle the reference ground-grid overlay (y=0 plane). No-op if the
        /// ground pipeline wasn't configured at construction.
        void setShowGround(bool v) { m_showGround = v; }
        bool showGround() const { return m_showGround; }

        /// Translate-gizmo overlay (three world-axis lines at the anchor).
        /// Visible only when both `visible` is true AND the gizmo pipeline
        /// was configured at construction. Anchor and length live alongside
        /// so the rasterizer can draw without a per-frame IPC round-trip.
        void setGizmoVisible(bool v) { m_gizmoVisible = v; }
        bool gizmoVisible() const { return m_gizmoVisible; }
        void setGizmoAnchor(const Vec3 &p) { m_gizmoAnchor = p; }
        const Vec3 &gizmoAnchor() const { return m_gizmoAnchor; }
        void setGizmoLength(float L) { m_gizmoLength = L; }
        float gizmoLength() const { return m_gizmoLength; }

        /// Composition-guide overlay drawn in NDC over everything (geometry
        /// and the PT composite). Bitmask: 0 = off, 1 = rule of thirds,
        /// 2 = safe areas (action 90% + title 80%), 3 = both. No-op if the
        /// guides pipeline wasn't configured at construction.
        void setCompositionGuides(int mode) { m_guidesMode = mode; }
        int compositionGuides() const { return m_guidesMode; }

        /// Skeleton overlay: world-space bone segments as endpoint pairs
        /// (segments[2k], segments[2k+1] are the ends of bone k). Drawn over
        /// everything (depth-test off) in the given color. Empty = nothing
        /// drawn. No-op if the bones pipeline wasn't configured.
        void setBoneSegments(std::vector<Vec3> segments) { m_boneSegments = std::move(segments); }
        void setBoneColor(const Vec3 &c) { m_boneColor = c; }
        const std::vector<Vec3> &boneSegments() const { return m_boneSegments; }

        /// Highlight overlay drawn with the same bones pipeline but a distinct
        /// color (e.g. a marker at the picked joint). Same endpoint-pair format.
        void setBoneHighlight(std::vector<Vec3> segments) { m_boneHighlight = std::move(segments); }
        void setBoneHighlightColor(const Vec3 &c) { m_boneHighlightColor = c; }

        /// Viewport background. Used as the render pass's clear color before
        /// any geometry / ground draws. Default is a muted blue-grey so a
        /// fresh scene looks like a viewport rather than the path tracer's
        /// black background. Components are linear [0,1]; the colorFormat
        /// (Unorm or Srgb) controls whether the framebuffer encodes them
        /// for display.
        void setBackgroundColor(float r, float g, float b, float a = 1.0f)
        {
            m_clearR = r; m_clearG = g; m_clearB = b; m_clearA = a;
        }
        void backgroundColor(float &r, float &g, float &b, float &a) const
        {
            r = m_clearR; g = m_clearG; b = m_clearB; a = m_clearA;
        }

    private:
        // Setup methods called from constructor
        void createPipeline();

        // Render helpers
        void updateCameraUniforms(const Camera &camera);
        void renderScene(const SceneCompiler::CompiledScene &scene, uint64_t sceneGeneration);

        // Not owned
        Device *m_device;

        // Configuration
        RasterizerConfig m_config;

        // Pipeline resources (owned)
        std::unique_ptr<GraphicsPipelineLayout> m_pipelineLayout;
        std::unique_ptr<GraphicsPipeline> m_pipeline;
        std::unique_ptr<GraphicsCommandBuffer> m_commandBuffer;

        // Color target is owned by m_pipeline; readback buffer is ours.
        std::unique_ptr<Buffer> m_readbackBuffer;

        // Per-frame transient buffers for the OVERLAYS (skeleton, guides) — small
        // and conditional, rebuilt each render. Cleared at the top of every render.
        std::vector<std::unique_ptr<Buffer>> m_transientInstanceBuffers;

        // CACHED per-batch INPUT_RATE_INSTANCE buffers for the main geometry. Built
        // once per scene revision and REUSED across frames — during camera-only
        // navigation the scene generation is stable, so we don't reallocate/upload
        // every frame (that per-frame allocator churn caused random nav hitches even
        // on light scenes). Rebuilt only when m_cachedInstanceGen != sceneGeneration.
        struct CachedInstanceBatch {
            uint32_t blasIndex = 0;
            uint32_t count = 0;
            std::unique_ptr<Buffer> instBuf;
        };
        std::vector<CachedInstanceBatch> m_cachedInstanceBatches;
        uint64_t m_cachedInstanceGen = ~0ull;

        bool m_showPoints = false;
        bool m_showEdges = false;
        bool m_showGround = false;

        bool m_gizmoVisible = false;
        Vec3 m_gizmoAnchor{0.0f, 0.0f, 0.0f};
        float m_gizmoLength = 1.0f;

        // Composition-guides bitmask (see setCompositionGuides). 0 = off.
        int m_guidesMode = 0;

        // Skeleton overlay: world-space bone endpoint pairs + line color.
        std::vector<Vec3> m_boneSegments;
        Vec3 m_boneColor{0.2f, 0.85f, 1.0f};
        // Picked-joint highlight (same pipeline, distinct color).
        std::vector<Vec3> m_boneHighlight;
        Vec3 m_boneHighlightColor{1.0f, 0.85f, 0.1f};

        // Mutable clear color. Defaults match the original hardcoded
        // values so existing scenes look identical at first launch.
        float m_clearR = 0.2f;
        float m_clearG = 0.3f;
        float m_clearB = 0.4f;
        float m_clearA = 1.0f;

        // Camera matrices (computed in updateCameraUniforms, used in renderScene)
        glm::mat4 m_viewMatrix;
        glm::mat4 m_projectionMatrix;
        // Camera world position — pushed to fragment shader so the PBR
        // BRDF can compute view direction per fragment without inverting
        // the view matrix in the shader.
        glm::vec3 m_cameraWorldPos{0.0f};

        // Lights descriptor set machinery. The pipeline's set-0 binding 0
        // is a storage-buffer slot expected to point at
        // CompiledScene::lightBuffer. We allocate one descriptor set out
        // of a private pool at construction, then rebind it to the
        // engine's current lightBuffer at the top of every render()
        // (the compile_scene path can swap the buffer when the light
        // count drifts, and a stale binding would dereference freed GPU
        // memory). One pool, one set — no per-frame churn.
        VkDescriptorPool m_lightsDescriptorPool = VK_NULL_HANDLE;
        VkDescriptorSet  m_lightsDescriptorSet  = VK_NULL_HANDLE;
        Buffer*          m_lastBoundLightBuffer = nullptr;

        // Cached at the top of render() from scene.lightCount so the
        // batched-draw push-constants don't need a reference to the
        // current CompiledScene.
        uint32_t         m_currentLightCount    = 0;
    };
} // namespace tracey
