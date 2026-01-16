#pragma once

#include "isf_parser.hpp"
#include "../ray_tracing_pipeline/data_structure.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace tracey
{

  class Device;
  class RayTracingPipeline;
  class RayTracingPipelineLayoutDescriptor;
  class ShaderBindingTable;
  class ShaderModule;

  class ISFPipelineBuilder
  {
  public:
    explicit ISFPipelineBuilder(Device &device);
    ~ISFPipelineBuilder();

    // Add shader stages from ISF files
    void addRayGenShader(const std::filesystem::path &isfPath);
    void addHitShader(const std::filesystem::path &isfPath);
    void addMissShader(const std::filesystem::path &isfPath);
    void addResolveShader(const std::filesystem::path &isfPath);

    // Add shader stages from ISF source strings
    void addRayGenShaderSource(const std::string &isfSource);
    void addHitShaderSource(const std::string &isfSource);
    void addMissShaderSource(const std::string &isfSource);
    void addResolveShaderSource(const std::string &isfSource);

    // Build the pipeline with an external layout
    // The layout should already contain bindings for vertex buffers, acceleration structures, etc.
    // The builder will only add the payload structure extracted from ISF files
    std::unique_ptr<RayTracingPipeline> build(RayTracingPipelineLayoutDescriptor &layout);

    // Get shader binding table (available after build())
    ShaderBindingTable *getShaderBindingTable() const;

    // Get the merged inputs layout (available after adding shaders)
    // Use this to create a ShaderInputsBuffer
    StructureLayout getInputsLayout() const;

  private:
    Device &m_device;

    std::optional<ISFShaderDefinition> m_rayGenShader;
    std::vector<ISFShaderDefinition> m_hitShaders;
    std::vector<ISFShaderDefinition> m_missShaders;
    std::optional<ISFShaderDefinition> m_resolveShader;

    // Built objects (owned by this builder)
    std::unique_ptr<ShaderBindingTable> m_sbt;

    // Shader modules (owned by this builder)
    std::unique_ptr<ShaderModule> m_rayGenModule;
    std::vector<std::unique_ptr<ShaderModule>> m_hitModules;
    std::vector<std::unique_ptr<ShaderModule>> m_missModules;
    std::unique_ptr<ShaderModule> m_resolveModule;

    // Raw pointers for SBT creation
    std::vector<const ShaderModule *> m_hitModulePtrs;
    std::vector<const ShaderModule *> m_missModulePtrs;

    // Build helpers
    void buildShaderModules();
    void buildShaderBindingTable();
    StructureLayout mergePayloads() const;
    StructureLayout mergeInputs() const;
  };

} // namespace tracey
