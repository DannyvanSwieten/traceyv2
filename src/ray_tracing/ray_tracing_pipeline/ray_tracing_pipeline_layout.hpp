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
            Buffer,
            RayPayload,
            AccelerationStructure
        };

        void addImage2D(std::string name, uint32_t index, ShaderStage stage);
        void addBuffer(std::string name, uint32_t index, ShaderStage stage, const StructureLayout &structure);
        void addAccelerationStructure(std::string name, uint32_t index, ShaderStage stage);
        void addPayload(std::string name, uint32_t index, ShaderStage stage, const StructureLayout &structure);

        struct DescriptorBinding
        {
            std::string name;
            uint32_t index;
            DescriptorType type;
            ShaderStage stage;
            std::optional<StructureLayout> structure = std::nullopt;
        };

        struct PayloadBinding
        {
            std::string name;
            uint32_t index;
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

    class WavefrontPipelineLayoutDescriptor : public RayTracingPipelineLayoutDescriptor
    {
    public:
        // Additional wavefront-specific layout methods can be added here
    };
}