#pragma once
#include <vector>
#include <string>
#include "../../device/device.hpp"
#include "data_structure.hpp"
namespace tracey
{
    enum class ImageLayoutFormat
    {
        RGBA8,
        RGBA32F
    };

    class RayTracingPipelineLayoutDescriptor
    {
    public:
        enum class DescriptorType
        {
            Image2D,
            StorageBuffer,
            UniformBuffer,
            RayPayload,
            AccelerationStructure,
            SampledTextureArray,  // Combined image+sampler (legacy, limited to 16)
            Sampler,              // Separate sampler (for bindless)
            SampledImageArray     // Separate sampled image array (for bindless)
        };

        void addImage2D(std::string name, ShaderStage stage, ImageLayoutFormat format = ImageLayoutFormat::RGBA8);
        void addStorageBuffer(std::string name, ShaderStage stage, const StructureLayout &structure);
        void addUniformBuffer(std::string name, ShaderStage stage, const StructureLayout &structure);
        void addAccelerationStructure(std::string name, ShaderStage stage);
        void addPayload(std::string name, ShaderStage stage, const StructureLayout &structure);
        void addSampledTextureArray(std::string name, ShaderStage stage, uint32_t maxCount);

        // Bindless texture support using separate samplers and image arrays
        void addSampler(std::string name, ShaderStage stage);
        void addSampledImageArray(std::string name, ShaderStage stage, uint32_t maxCount);

        size_t indexForBinding(const std::string_view name) const;

        struct DescriptorBinding
        {
            std::string name;
            DescriptorType type;
            ShaderStage stage;
            std::optional<StructureLayout> structure = std::nullopt;
            ImageLayoutFormat imageFormat = ImageLayoutFormat::RGBA8;
            uint32_t textureArrayCount = 0; // For SampledTextureArray
        };

        struct PayloadBinding
        {
            std::string name;
            ShaderStage stage;
            StructureLayout structure;
        };

        const std::vector<DescriptorBinding> &bindings() const { return m_bindings; }
        std::vector<DescriptorBinding> bindingsForStage(ShaderStage stage) const;
        const std::vector<PayloadBinding> &payloads() const { return m_payloads; }
        std::vector<PayloadBinding> payloadsForStage(ShaderStage stage) const;

    private:
        std::vector<DescriptorBinding> m_bindings;
        std::vector<PayloadBinding> m_payloads;
    };
}