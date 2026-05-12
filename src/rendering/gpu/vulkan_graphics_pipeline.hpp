#pragma once

#include "../graphics_pipeline.hpp"
#include "../../gpu/vulkan_context.hpp"
#include "../../device/image_2d.hpp"
#include <volk.h>
#include <memory>

namespace tracey
{
    class VulkanComputeDevice;

    /// Vulkan implementation of graphics pipeline
    /// Manages render pass, pipeline, framebuffer, and render targets
    class VulkanGraphicsPipeline : public GraphicsPipeline
    {
    public:
        VulkanGraphicsPipeline(VulkanComputeDevice& device,
                              const GraphicsPipelineConfig& config,
                              const GraphicsPipelineLayout& layout);
        ~VulkanGraphicsPipeline() override;

        // GraphicsPipeline interface
        void bind() override;
        const GraphicsPipelineLayout& layout() const override { return m_layout; }
        Image2D* colorTarget() const override { return m_colorImage.get(); }
        Image2D* depthTarget() const override { return m_depthImage.get(); }
        const GraphicsPipelineConfig& config() const override { return m_config; }

        // Vulkan-specific accessors
        VkPipeline vkPipeline() const { return m_pipeline; }
        VkPipeline vkPointsPipeline() const { return m_pointsPipeline; }
        VkPipeline vkLinesPipeline() const { return m_linesPipeline; }
        VkPipeline vkGroundPipeline() const { return m_groundPipeline; }
        bool hasPointsPipeline() const { return m_pointsPipeline != VK_NULL_HANDLE; }
        bool hasLinesPipeline() const { return m_linesPipeline != VK_NULL_HANDLE; }
        bool hasGroundPipeline() const { return m_groundPipeline != VK_NULL_HANDLE; }
        VkPipelineLayout vkPipelineLayout() const { return m_pipelineLayout; }
        VkRenderPass vkRenderPass() const { return m_renderPass; }
        VkFramebuffer vkFramebuffer() const { return m_framebuffer; }

    private:
        void createRenderPass();
        void createRenderTargets();
        void createFramebuffer();
        void createDescriptorSetLayout();
        void createPipelineLayout();
        void createPipeline();
        // Optional sibling pipeline drawing point sprites in the same render
        // pass (alpha-blended, depth test on, depth write off).
        void createPointsPipeline();
        // Optional sibling pipeline drawing triangle edges (POLYGON_MODE_LINE,
        // depth-test on, depth-write off, slight depth bias).
        void createLinesPipeline();
        // Optional sibling pipeline drawing a reference ground grid on y=0
        // (procedural quad in the vertex shader, alpha-blended, depth-test on,
        // depth-write off).
        void createGroundPipeline();

        VulkanComputeDevice& m_device;
        GraphicsPipelineConfig m_config;
        GraphicsPipelineLayout m_layout;

        // Vulkan objects
        VkRenderPass m_renderPass = VK_NULL_HANDLE;
        VkPipeline m_pipeline = VK_NULL_HANDLE;
        VkPipeline m_pointsPipeline = VK_NULL_HANDLE;
        VkPipeline m_linesPipeline = VK_NULL_HANDLE;
        VkPipeline m_groundPipeline = VK_NULL_HANDLE;
        VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
        VkFramebuffer m_framebuffer = VK_NULL_HANDLE;

        // Shader modules (destroyed after pipeline creation)
        VkShaderModule m_vertexShaderModule = VK_NULL_HANDLE;
        VkShaderModule m_fragmentShaderModule = VK_NULL_HANDLE;

        // Render targets (owned)
        std::unique_ptr<Image2D> m_colorImage;
        std::unique_ptr<Image2D> m_depthImage;  // unused; depth is raw below
        VkImageView m_colorImageView = VK_NULL_HANDLE;
        VkImageView m_depthImageView = VK_NULL_HANDLE;
        // Depth attachment is depth-format (D32_SFLOAT) and depth-aspect, which
        // the abstract Image2D factory doesn't support. Manage it as raw
        // Vulkan resources owned by this pipeline.
        VkImage m_depthVkImage = VK_NULL_HANDLE;
        VkDeviceMemory m_depthVkMemory = VK_NULL_HANDLE;
    };
}
