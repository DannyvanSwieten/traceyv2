#pragma once
#include <vector>
#include <string>
#include "../../device/device.hpp"
namespace tracey
{
    class RayTracingPipelineLayout
    {
    public:
        enum class DescriptorType
        {
            Image2D,
            Buffer,
            AccelerationStructure
        };

        void addImage2D(std::string name, uint32_t index, ShaderStage stage);
        void addBuffer(std::string name, uint32_t index, ShaderStage stage);
        void addAccelerationStructure(std::string name, uint32_t index, ShaderStage stage);

        struct DescriptorBinding
        {
            std::string name;
            uint32_t index;
            DescriptorType type;
            ShaderStage stage;
        };

        const std::vector<DescriptorBinding> &bindings() const { return m_bindings; }
        std::vector<DescriptorBinding> bindingsForStage(ShaderStage stage) const;

    private:
        std::vector<DescriptorBinding> m_bindings;
    };
}