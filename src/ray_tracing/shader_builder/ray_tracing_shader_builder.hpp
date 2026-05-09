#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace tracey
{
    class Device;
    class RayTracingPipeline;
    class RayTracingPipelineLayoutDescriptor;
    class ShaderBindingTable;
    class ShaderModule;

    // Assembles a ray-tracing pipeline from plain GLSL shader files.
    //
    // Each stage takes one shader (RayGen / ClosestHit / Miss / Resolve);
    // the wavefront topology only needs one of each. The user shader's entry
    // point is "shader" (matches the wavefront pipeline compiler's templates).
    //
    // Unlike the previous ISF builder, this class does NOT add anything to
    // the pipeline layout on the caller's behalf. The caller must declare:
    //   - all bindings (images, SSBOs, samplers, ...)
    //   - the wavefront RayPayload via layout.addPayload(...)
    //   - any uniform buffer the shaders reference (e.g. "shaderInputs")
    //
    // For file-based shaders, `#include "..."` directives are resolved via the
    // GLSL preprocessor relative to the shader file's directory.
    class RayTracingShaderBuilder
    {
    public:
        explicit RayTracingShaderBuilder(Device &device);
        ~RayTracingShaderBuilder();

        // File-based: read, run #include preprocessor, compile to ShaderModule.
        void setRayGenShader(const std::filesystem::path &glslPath);
        void setHitShader(const std::filesystem::path &glslPath);
        void setMissShader(const std::filesystem::path &glslPath);
        void setResolveShader(const std::filesystem::path &glslPath);

        // Source-based (no preprocessing). The caller is responsible for any
        // #include resolution.
        void setRayGenShaderSource(std::string source);
        void setHitShaderSource(std::string source);
        void setMissShaderSource(std::string source);
        void setResolveShaderSource(std::string source);

        // Compile the shaders, build the SBT, and create the wavefront pipeline
        // against the given layout. RayGen is required; the others are optional
        // and pass through as nullptr if not set.
        std::unique_ptr<RayTracingPipeline> build(RayTracingPipelineLayoutDescriptor &layout);

        // Available after build().
        ShaderBindingTable *getShaderBindingTable() const;

    private:
        Device &m_device;

        std::optional<std::string> m_rayGenSource;
        std::optional<std::string> m_hitSource;
        std::optional<std::string> m_missSource;
        std::optional<std::string> m_resolveSource;

        std::unique_ptr<ShaderModule> m_rayGenModule;
        std::unique_ptr<ShaderModule> m_hitModule;
        std::unique_ptr<ShaderModule> m_missModule;
        std::unique_ptr<ShaderModule> m_resolveModule;
        std::unique_ptr<ShaderBindingTable> m_sbt;
    };
}
