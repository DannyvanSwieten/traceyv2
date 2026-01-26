#include "graphics_pipeline_layout.hpp"
#include <algorithm>

namespace tracey
{
    void GraphicsPipelineLayout::addUniformBuffer(const std::string& name, uint32_t binding, uint32_t set,
                                                  ShaderStage stages, const StructureLayout& structure)
    {
        DescriptorBinding desc;
        desc.name = name;
        desc.type = DescriptorType::UniformBuffer;
        desc.binding = binding;
        desc.set = set;
        desc.stages = stages;
        desc.structure = structure;
        desc.arrayCount = 1;
        m_bindings.push_back(desc);
    }

    void GraphicsPipelineLayout::addStorageBuffer(const std::string& name, uint32_t binding, uint32_t set,
                                                 ShaderStage stages, const StructureLayout& structure)
    {
        DescriptorBinding desc;
        desc.name = name;
        desc.type = DescriptorType::StorageBuffer;
        desc.binding = binding;
        desc.set = set;
        desc.stages = stages;
        desc.structure = structure;
        desc.arrayCount = 1;
        m_bindings.push_back(desc);
    }

    void GraphicsPipelineLayout::addCombinedImageSampler(const std::string& name, uint32_t binding, uint32_t set,
                                                        ShaderStage stages, uint32_t arrayCount)
    {
        DescriptorBinding desc;
        desc.name = name;
        desc.type = DescriptorType::CombinedImageSampler;
        desc.binding = binding;
        desc.set = set;
        desc.stages = stages;
        desc.arrayCount = arrayCount;
        m_bindings.push_back(desc);
    }

    void GraphicsPipelineLayout::addSampler(const std::string& name, uint32_t binding, uint32_t set,
                                           ShaderStage stages)
    {
        DescriptorBinding desc;
        desc.name = name;
        desc.type = DescriptorType::Sampler;
        desc.binding = binding;
        desc.set = set;
        desc.stages = stages;
        desc.arrayCount = 1;
        m_bindings.push_back(desc);
    }

    void GraphicsPipelineLayout::addSampledImageArray(const std::string& name, uint32_t binding, uint32_t set,
                                                      ShaderStage stages, uint32_t arrayCount)
    {
        DescriptorBinding desc;
        desc.name = name;
        desc.type = DescriptorType::SampledImageArray;
        desc.binding = binding;
        desc.set = set;
        desc.stages = stages;
        desc.arrayCount = arrayCount;
        m_bindings.push_back(desc);
    }

    std::vector<GraphicsPipelineLayout::DescriptorBinding>
    GraphicsPipelineLayout::bindingsForSet(uint32_t set) const
    {
        std::vector<DescriptorBinding> result;
        for (const auto& binding : m_bindings)
        {
            if (binding.set == set)
            {
                result.push_back(binding);
            }
        }
        return result;
    }

    const GraphicsPipelineLayout::DescriptorBinding*
    GraphicsPipelineLayout::findBinding(const std::string& name) const
    {
        auto it = std::find_if(m_bindings.begin(), m_bindings.end(),
                              [&name](const DescriptorBinding& b) { return b.name == name; });
        if (it != m_bindings.end())
        {
            return &(*it);
        }
        return nullptr;
    }
}
