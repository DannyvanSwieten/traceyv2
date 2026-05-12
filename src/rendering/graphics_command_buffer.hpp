#pragma once
#include <cstdint>

namespace tracey
{
    class GraphicsPipeline;
    class Buffer;
    class Image2D;
    class DescriptorSet;

    /// Abstract interface for graphics command buffer
    /// Records rendering commands (draw calls, state changes, etc.)
    class GraphicsCommandBuffer
    {
    public:
        virtual ~GraphicsCommandBuffer() = default;

        /// Begin recording commands
        virtual void begin() = 0;

        /// End recording commands
        virtual void end() = 0;

        /// Begin render pass
        /// Clears color and depth buffers, sets up rendering
        virtual void beginRenderPass(GraphicsPipeline* pipeline,
                                     float clearR = 0.0f, float clearG = 0.0f,
                                     float clearB = 0.0f, float clearA = 1.0f,
                                     float clearDepth = 1.0f) = 0;

        /// End render pass
        virtual void endRenderPass() = 0;

        /// Bind graphics pipeline
        virtual void bindPipeline(GraphicsPipeline* pipeline) = 0;

        /// Bind the optional secondary "points" pipeline (if the pipeline was
        /// configured with pointsVertexShader/pointsFragmentShader). Must be
        /// called inside the same render pass that bindPipeline started in.
        virtual void bindPointsPipeline(GraphicsPipeline* pipeline) = 0;

        /// Bind the optional "lines" pipeline (POLYGON_MODE_LINE wireframe).
        /// No-op if the pipeline wasn't configured with line shaders.
        virtual void bindLinesPipeline(GraphicsPipeline* pipeline) = 0;

        /// Bind the optional "ground" pipeline (y=0 reference grid). No-op
        /// if the pipeline wasn't configured with ground shaders.
        virtual void bindGroundPipeline(GraphicsPipeline* pipeline) = 0;

        /// Bind vertex buffer
        /// @param buffer Vertex buffer containing interleaved vertex data
        /// @param offset Byte offset into the buffer
        virtual void bindVertexBuffer(const Buffer* buffer, uint32_t offset = 0) = 0;

        /// Bind a vertex buffer at an explicit binding index. The main
        /// rasterizer pipeline declares two bindings (0 = position, 1 = Cd);
        /// this lets callers attach the color stream without overloading the
        /// canonical position-only `bindVertexBuffer` call.
        virtual void bindVertexBufferAt(const Buffer* buffer, uint32_t binding,
                                        uint32_t offset = 0) = 0;

        /// Bind index buffer
        /// @param buffer Index buffer containing triangle indices
        /// @param offset Byte offset into the buffer
        virtual void bindIndexBuffer(const Buffer* buffer, uint32_t offset = 0) = 0;

        /// Draw indexed triangles
        /// @param indexCount Number of indices to draw
        /// @param instanceCount Number of instances (default 1)
        /// @param firstIndex First index in the index buffer
        /// @param vertexOffset Offset added to vertex index
        /// @param firstInstance First instance ID
        virtual void drawIndexed(uint32_t indexCount, uint32_t instanceCount = 1,
                                uint32_t firstIndex = 0, int32_t vertexOffset = 0,
                                uint32_t firstInstance = 0) = 0;

        /// Draw non-indexed triangles
        /// @param vertexCount Number of vertices to draw
        /// @param instanceCount Number of instances (default 1)
        /// @param firstVertex First vertex in vertex buffer
        /// @param firstInstance First instance ID
        virtual void draw(uint32_t vertexCount, uint32_t instanceCount = 1,
                         uint32_t firstVertex = 0, uint32_t firstInstance = 0) = 0;

        /// Push constants to shaders
        /// @param data Pointer to data to push
        /// @param size Size of data in bytes
        /// @param offset Offset into push constant range
        virtual void pushConstants(const void* data, uint32_t size, uint32_t offset = 0) = 0;

        /// Bind descriptor set
        /// @param set Descriptor set to bind
        /// @param setIndex Descriptor set index (0, 1, 2, ...)
        virtual void bindDescriptorSet(DescriptorSet* set, uint32_t setIndex = 0) = 0;

        /// Copy rendered image to buffer (for readback to CPU)
        virtual void copyImageToBuffer(const Image2D* image, Buffer* buffer) = 0;

        /// Submit commands to GPU and wait for completion
        virtual void waitUntilCompleted() = 0;
    };
}
