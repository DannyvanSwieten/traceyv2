#pragma once
#include <vector>
#include <string>
#include "../../device/device.hpp"
#include "data_structure.hpp"
namespace tracey
{
    class RayTracingPipelineLayoutDescriptor
    {
    public:
        enum class DescriptorType
        {
            Image2D,
            StorageBuffer,
            UniformBuffer,
            RayPayload,
            AccelerationStructure
        };

        void addImage2D(std::string name, ShaderStage stage);
        void addStorageBuffer(std::string name, ShaderStage stage, const StructureLayout &structure);
        void addUniformBuffer(std::string name, ShaderStage stage, const StructureLayout &structure);
        void addAccelerationStructure(std::string name, ShaderStage stage);
        void addPayload(std::string name, ShaderStage stage, const StructureLayout &structure);

        size_t indexForBinding(const std::string_view name) const;

        struct DescriptorBinding
        {
            std::string name;
            DescriptorType type;
            ShaderStage stage;
            std::optional<StructureLayout> structure = std::nullopt;
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