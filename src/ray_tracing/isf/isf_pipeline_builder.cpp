#include "isf_pipeline_builder.hpp"

#include "../../device/device.hpp"
#include "../ray_tracing_pipeline/cpu/cpu_shader_binding_table.hpp"
#include "../ray_tracing_pipeline/ray_tracing_pipeline.hpp"
#include "../ray_tracing_pipeline/ray_tracing_pipeline_layout.hpp"
#include "../shader_module/shader_module.hpp"

#include <stdexcept>

namespace tracey
{

    ISFPipelineBuilder::ISFPipelineBuilder(Device &device) : m_device(device) {}

    ISFPipelineBuilder::~ISFPipelineBuilder() = default;

    void ISFPipelineBuilder::addRayGenShader(const std::filesystem::path &isfPath)
    {
        m_rayGenShader = ISFParser::parseFile(isfPath);
    }

    void ISFPipelineBuilder::addHitShader(const std::filesystem::path &isfPath)
    {
        m_hitShaders.push_back(ISFParser::parseFile(isfPath));
    }

    void ISFPipelineBuilder::addMissShader(const std::filesystem::path &isfPath)
    {
        m_missShaders.push_back(ISFParser::parseFile(isfPath));
    }

    void ISFPipelineBuilder::addResolveShader(const std::filesystem::path &isfPath)
    {
        m_resolveShader = ISFParser::parseFile(isfPath);
    }

    void ISFPipelineBuilder::addRayGenShaderSource(const std::string &isfSource)
    {
        m_rayGenShader = ISFParser::parse(isfSource);
    }

    void ISFPipelineBuilder::addHitShaderSource(const std::string &isfSource)
    {
        m_hitShaders.push_back(ISFParser::parse(isfSource));
    }

    void ISFPipelineBuilder::addMissShaderSource(const std::string &isfSource)
    {
        m_missShaders.push_back(ISFParser::parse(isfSource));
    }

    void ISFPipelineBuilder::addResolveShaderSource(const std::string &isfSource)
    {
        m_resolveShader = ISFParser::parse(isfSource);
    }

    StructureLayout ISFPipelineBuilder::mergePayloads() const
    {
        // Collect all payloads from all shader stages
        std::vector<const ISFPayload *> allPayloads;

        if (m_rayGenShader)
        {
            for (const auto &payload : m_rayGenShader->payloads)
            {
                allPayloads.push_back(&payload);
            }
        }

        for (const auto &shader : m_hitShaders)
        {
            for (const auto &payload : shader.payloads)
            {
                // Check if this payload name already exists
                bool found = false;
                for (const auto *existing : allPayloads)
                {
                    if (existing->name == payload.name)
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    allPayloads.push_back(&payload);
                }
            }
        }

        for (const auto &shader : m_missShaders)
        {
            for (const auto &payload : shader.payloads)
            {
                bool found = false;
                for (const auto *existing : allPayloads)
                {
                    if (existing->name == payload.name)
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    allPayloads.push_back(&payload);
                }
            }
        }

        if (m_resolveShader)
        {
            for (const auto &payload : m_resolveShader->payloads)
            {
                bool found = false;
                for (const auto *existing : allPayloads)
                {
                    if (existing->name == payload.name)
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    allPayloads.push_back(&payload);
                }
            }
        }

        // If no payloads defined, create a default one
        if (allPayloads.empty())
        {
            StructureLayout layout("RayPayload");
            layout.addMember({"color", "vec3", 0, false, 0});
            return layout;
        }

        // Use the first payload as the primary structure
        const ISFPayload *primaryPayload = allPayloads[0];
        StructureLayout layout(primaryPayload->name);

        for (const auto &field : primaryPayload->fields)
        {
            layout.addMember({field.name, field.type, 0, false, 0});
        }

        return layout;
    }

    // Helper to convert ISF input type to GLSL type
    static std::string isfTypeToGlsl(const std::string &isfType)
    {
        if (isfType == "float")
            return "float";
        if (isfType == "color")
            return "vec4";
        if (isfType == "point2D")
            return "vec2";
        if (isfType == "bool")
            return "uint"; // bool in uniform buffer uses uint for std140/std430 alignment
        if (isfType == "long")
            return "int";
        return "float"; // Default fallback
    }

    StructureLayout ISFPipelineBuilder::mergeInputs() const
    {
        // Collect all inputs from all shader stages, avoiding duplicates by name
        std::vector<const ISFInput *> allInputs;

        auto addInputsFromShader = [&allInputs](const std::vector<ISFInput> &inputs)
        {
            for (const auto &input : inputs)
            {
                bool found = false;
                for (const auto *existing : allInputs)
                {
                    if (existing->name == input.name)
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    allInputs.push_back(&input);
                }
            }
        };

        if (m_rayGenShader)
        {
            addInputsFromShader(m_rayGenShader->inputs);
        }

        for (const auto &shader : m_hitShaders)
        {
            addInputsFromShader(shader.inputs);
        }

        for (const auto &shader : m_missShaders)
        {
            addInputsFromShader(shader.inputs);
        }

        if (m_resolveShader)
        {
            addInputsFromShader(m_resolveShader->inputs);
        }

        // Create structure layout for the uniform buffer
        StructureLayout layout("ShaderInputs");

        for (const auto *input : allInputs)
        {
            std::string glslType = isfTypeToGlsl(input->type);
            layout.addMember({input->name, glslType, 0, false, 0});
        }

        return layout;
    }

    void ISFPipelineBuilder::buildShaderModules()
    {
        if (!m_rayGenShader)
        {
            throw std::runtime_error("ISFPipelineBuilder: RayGeneration shader is required");
        }

        // Create ray generation module
        m_rayGenModule.reset(
            m_device.createShaderModule(ShaderStage::RayGeneration, m_rayGenShader->glslSource, "shader"));

        // Create hit shader modules
        m_hitModules.clear();
        m_hitModulePtrs.clear();
        for (const auto &shader : m_hitShaders)
        {
            auto module = std::unique_ptr<ShaderModule>(
                m_device.createShaderModule(ShaderStage::ClosestHit, shader.glslSource, "shader"));
            m_hitModulePtrs.push_back(module.get());
            m_hitModules.push_back(std::move(module));
        }

        // Create miss shader modules
        m_missModules.clear();
        m_missModulePtrs.clear();
        for (const auto &shader : m_missShaders)
        {
            auto module = std::unique_ptr<ShaderModule>(
                m_device.createShaderModule(ShaderStage::Miss, shader.glslSource, "shader"));
            m_missModulePtrs.push_back(module.get());
            m_missModules.push_back(std::move(module));
        }

        // Create resolve module if present
        if (m_resolveShader)
        {
            m_resolveModule.reset(
                m_device.createShaderModule(ShaderStage::Resolve, m_resolveShader->glslSource, "shader"));
        }
    }

    void ISFPipelineBuilder::buildShaderBindingTable()
    {
        std::span<const ShaderModule *> hitSpan(m_hitModulePtrs.data(), m_hitModulePtrs.size());
        std::span<const ShaderModule *> missSpan(m_missModulePtrs.data(), m_missModulePtrs.size());

        m_sbt.reset(m_device.createShaderBindingTable(m_rayGenModule.get(), hitSpan, missSpan,
                                                      m_resolveModule.get()));
    }

    std::unique_ptr<RayTracingPipeline> ISFPipelineBuilder::build(RayTracingPipelineLayoutDescriptor &layout)
    {
        // Add payload structure extracted from ISF files to the user-provided layout
        StructureLayout payloadLayout = mergePayloads();
        layout.addPayload(payloadLayout.name(), ShaderStage::RayGeneration, payloadLayout);

        // Add uniform buffer for merged shader inputs (if any inputs are defined)
        StructureLayout inputsLayout = mergeInputs();
        if (!inputsLayout.fields().empty())
        {
            // Available to all shader stages
            layout.addUniformBuffer("shaderInputs", ShaderStage::RayGeneration, inputsLayout);
        }

        buildShaderModules();
        buildShaderBindingTable();

        // Create and return the wavefront pipeline using the user-provided layout
        return std::unique_ptr<RayTracingPipeline>(
            m_device.createWaveFrontRayTracingPipeline(layout, m_sbt.get()));
    }

    ShaderBindingTable *ISFPipelineBuilder::getShaderBindingTable() const
    {
        return m_sbt.get();
    }

    StructureLayout ISFPipelineBuilder::getInputsLayout() const
    {
        return mergeInputs();
    }

} // namespace tracey
