#pragma once
#include <vector>
#include <string>
#include "../device/device.hpp"
#include "../ray_tracing/ray_tracing_pipeline/data_structure.hpp"

namespace tracey
{
    /// Descriptor set layout for graphics pipelines
    /// Describes all descriptor bindings (UBOs, textures, samplers, etc.)
    class GraphicsPipelineLayout
    {
    public:
        enum class DescriptorType
        {
            UniformBuffer,          // UBO (e.g., camera matrices)
            StorageBuffer,          // SSBO (e.g., material buffer)
            CombinedImageSampler,   // Legacy combined image+sampler
            Sampler,                // Separate sampler (for bindless)
            SampledImage,           // Separate sampled image (for bindless)
            SampledImageArray       // Array of sampled images (for bindless textures)
        };

        struct DescriptorBinding
        {
            std::string name;
            DescriptorType type;
            uint32_t binding;                            // Binding number in shader
            uint32_t set;                                // Descriptor set number (0, 1, 2...)
            ShaderStage stages;                          // Shader stages that access this
            std::optional<StructureLayout> structure;    // For UBO/SSBO
            uint32_t arrayCount = 1;                     // Array size (for texture arrays)
        };

        /// Add a uniform buffer (UBO) binding
        void addUniformBuffer(const std::string& name, uint32_t binding, uint32_t set,
                             ShaderStage stages, const StructureLayout& structure);

        /// Add a storage buffer (SSBO) binding
        void addStorageBuffer(const std::string& name, uint32_t binding, uint32_t set,
                             ShaderStage stages, const StructureLayout& structure);

        /// Add a combined image+sampler binding (legacy, limited count)
        void addCombinedImageSampler(const std::string& name, uint32_t binding, uint32_t set,
                                    ShaderStage stages, uint32_t arrayCount = 1);

        /// Add a separate sampler binding (for bindless)
        void addSampler(const std::string& name, uint32_t binding, uint32_t set, ShaderStage stages);

        /// Add a separate sampled image array (for bindless)
        void addSampledImageArray(const std::string& name, uint32_t binding, uint32_t set,
                                 ShaderStage stages, uint32_t arrayCount);

        /// Get all descriptor bindings
        const std::vector<DescriptorBinding>& bindings() const { return m_bindings; }

        /// Get bindings for a specific descriptor set
        std::vector<DescriptorBinding> bindingsForSet(uint32_t set) const;

        /// Get binding by name
        const DescriptorBinding* findBinding(const std::string& name) const;

    private:
        std::vector<DescriptorBinding> m_bindings;
    };
}
