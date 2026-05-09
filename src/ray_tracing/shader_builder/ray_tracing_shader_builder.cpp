#include "ray_tracing_shader_builder.hpp"

#include "glsl_preprocessor.hpp"

#include "../../device/device.hpp"
#include "../ray_tracing_pipeline/ray_tracing_pipeline.hpp"
#include "../ray_tracing_pipeline/shader_binding_table.hpp"
#include "../shader_module/shader_module.hpp"

#include <fstream>
#include <span>
#include <sstream>
#include <stdexcept>

namespace tracey
{
    namespace
    {
        std::string readFile(const std::filesystem::path &p)
        {
            std::ifstream f(p);
            if (!f.is_open())
            {
                throw std::runtime_error("RayTracingShaderBuilder: failed to open '" + p.string() + "'");
            }
            std::stringstream buf;
            buf << f.rdbuf();
            return buf.str();
        }

        std::string loadAndPreprocess(const std::filesystem::path &p)
        {
            return preprocessGlsl(readFile(p), p.parent_path());
        }
    }

    RayTracingShaderBuilder::RayTracingShaderBuilder(Device &device) : m_device(device) {}
    RayTracingShaderBuilder::~RayTracingShaderBuilder() = default;

    void RayTracingShaderBuilder::setRayGenShader(const std::filesystem::path &p) { m_rayGenSource = loadAndPreprocess(p); }
    void RayTracingShaderBuilder::setHitShader(const std::filesystem::path &p)    { m_hitSource    = loadAndPreprocess(p); }
    void RayTracingShaderBuilder::setMissShader(const std::filesystem::path &p)   { m_missSource   = loadAndPreprocess(p); }
    void RayTracingShaderBuilder::setResolveShader(const std::filesystem::path &p) { m_resolveSource = loadAndPreprocess(p); }

    void RayTracingShaderBuilder::setRayGenShaderSource(std::string s) { m_rayGenSource = std::move(s); }
    void RayTracingShaderBuilder::setHitShaderSource(std::string s)    { m_hitSource    = std::move(s); }
    void RayTracingShaderBuilder::setMissShaderSource(std::string s)   { m_missSource   = std::move(s); }
    void RayTracingShaderBuilder::setResolveShaderSource(std::string s) { m_resolveSource = std::move(s); }

    std::unique_ptr<RayTracingPipeline> RayTracingShaderBuilder::build(RayTracingPipelineLayoutDescriptor &layout)
    {
        if (!m_rayGenSource)
        {
            throw std::runtime_error("RayTracingShaderBuilder::build: ray-gen shader is required");
        }

        // Compile each provided stage. Entry point is "shader" to match the
        // wavefront pipeline compiler's user-source insertion templates.
        m_rayGenModule.reset(m_device.createShaderModule(
            ShaderStage::RayGeneration, *m_rayGenSource, "shader"));

        if (m_hitSource)
        {
            m_hitModule.reset(m_device.createShaderModule(
                ShaderStage::ClosestHit, *m_hitSource, "shader"));
        }
        if (m_missSource)
        {
            m_missModule.reset(m_device.createShaderModule(
                ShaderStage::Miss, *m_missSource, "shader"));
        }
        if (m_resolveSource)
        {
            m_resolveModule.reset(m_device.createShaderModule(
                ShaderStage::Resolve, *m_resolveSource, "shader"));
        }

        // Pack into spans for the SBT factory. Single hit/miss is the only
        // shape we need — the uber-VM in the hit shader handles per-material
        // dispatch from a buffer, so there's no SBT-per-material story.
        const ShaderModule *hitPtr = m_hitModule.get();
        const ShaderModule *missPtr = m_missModule.get();
        std::span<const ShaderModule *> hitSpan(m_hitModule ? &hitPtr : nullptr,
                                                m_hitModule ? 1 : 0);
        std::span<const ShaderModule *> missSpan(m_missModule ? &missPtr : nullptr,
                                                 m_missModule ? 1 : 0);

        m_sbt.reset(m_device.createShaderBindingTable(
            m_rayGenModule.get(), hitSpan, missSpan, m_resolveModule.get()));

        return std::unique_ptr<RayTracingPipeline>(
            m_device.createWaveFrontRayTracingPipeline(layout, m_sbt.get()));
    }

    ShaderBindingTable *RayTracingShaderBuilder::getShaderBindingTable() const
    {
        return m_sbt.get();
    }
}
