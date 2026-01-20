#pragma once
#include <string_view>
#include <span>
namespace tracey
{
    class Image2D;
    class Buffer;
    class TopLevelAccelerationStructure;
    class DescriptorSet
    {
    public:
        virtual ~DescriptorSet() = default;
        virtual void setImage2D(const std::string_view name, Image2D *image) = 0;
        virtual void setBuffer(const std::string_view name, Buffer *buffer) = 0;
        virtual void setUniformBuffer(const std::string_view name, Buffer *buffer) = 0;
        virtual void setAccelerationStructure(const std::string_view name, const TopLevelAccelerationStructure *tlas) = 0;

        // Bind a sampled texture (combined image sampler) at an explicit binding index
        virtual void setSampledTexture(uint32_t bindingIndex, Image2D *image) = 0;

        // Bind an array of sampled textures starting at an explicit binding index
        virtual void setSampledTextureArray(uint32_t bindingIndex, std::span<Image2D *> images) = 0;

        // Bind an array of sampled textures by name (looks up binding index from layout)
        virtual void setSampledTextureArray(const std::string_view name, std::span<Image2D *> images) = 0;

        // Bind a storage buffer at an explicit binding index (for materials, UVs, etc.)
        virtual void setStorageBuffer(uint32_t bindingIndex, Buffer *buffer) = 0;

        // Bindless texture support: separate samplers and sampled image arrays
        virtual void setSampler(const std::string_view name, bool useLinearFiltering) = 0;
        virtual void setSampledImageArray(const std::string_view name, std::span<Image2D *> images) = 0;
    };
} // namespace tracey