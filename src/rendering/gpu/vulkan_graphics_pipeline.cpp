#include "vulkan_graphics_pipeline.hpp"
#include "../../device/gpu/vulkan_compute_device.hpp"
#include "../../device/gpu/vulkan_image_2d.hpp"
#include <array>
#include <stdexcept>
#include <fstream>
#include <vector>

namespace tracey
{
    // Helper function to read shader file
    static std::vector<char> readShaderFile(const std::filesystem::path& filepath)
    {
        std::ifstream file(filepath, std::ios::ate | std::ios::binary);
        if (!file.is_open())
        {
            throw std::runtime_error("Failed to open shader file: " + filepath.string());
        }

        size_t fileSize = static_cast<size_t>(file.tellg());
        std::vector<char> buffer(fileSize);
        file.seekg(0);
        file.read(buffer.data(), fileSize);
        file.close();

        return buffer;
    }

    // Helper function to create shader module
    static VkShaderModule createShaderModule(VkDevice device, const std::vector<char>& code)
    {
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

        VkShaderModule shaderModule;
        if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create shader module");
        }

        return shaderModule;
    }

    VulkanGraphicsPipeline::VulkanGraphicsPipeline(VulkanComputeDevice& device,
                                                   const GraphicsPipelineConfig& config,
                                                   const GraphicsPipelineLayout& layout)
        : m_device(device), m_config(config), m_layout(layout)
    {
        createRenderPass();
        createRenderTargets();
        createFramebuffer();
        createDescriptorSetLayout();
        createPipelineLayout();
        createPipeline();
        if (!m_config.pointsVertexShader.empty() && !m_config.pointsFragmentShader.empty()) {
            createPointsPipeline();
        }
        if (!m_config.linesVertexShader.empty() && !m_config.linesFragmentShader.empty()) {
            createLinesPipeline();
        }
        if (!m_config.groundVertexShader.empty() && !m_config.groundFragmentShader.empty()) {
            createGroundPipeline();
        }
        if (!m_config.gizmoVertexShader.empty() && !m_config.gizmoFragmentShader.empty()) {
            createGizmoPipeline();
        }
    }

    VulkanGraphicsPipeline::~VulkanGraphicsPipeline()
    {
        VkDevice vkDevice = m_device.vkDevice();

        if (m_pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(vkDevice, m_pipeline, nullptr);
        if (m_pointsPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(vkDevice, m_pointsPipeline, nullptr);
        if (m_linesPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(vkDevice, m_linesPipeline, nullptr);
        if (m_groundPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(vkDevice, m_groundPipeline, nullptr);
        if (m_gizmoPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(vkDevice, m_gizmoPipeline, nullptr);

        if (m_pipelineLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(vkDevice, m_pipelineLayout, nullptr);

        if (m_descriptorSetLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(vkDevice, m_descriptorSetLayout, nullptr);

        if (m_framebuffer != VK_NULL_HANDLE)
            vkDestroyFramebuffer(vkDevice, m_framebuffer, nullptr);

        // Color image view is owned by m_colorImage (VulkanImage2D); skip.
        // Depth resources are raw because Image2D doesn't model depth formats.
        if (m_depthImageView != VK_NULL_HANDLE)
            vkDestroyImageView(vkDevice, m_depthImageView, nullptr);
        if (m_depthVkImage != VK_NULL_HANDLE)
            vkDestroyImage(vkDevice, m_depthVkImage, nullptr);
        if (m_depthVkMemory != VK_NULL_HANDLE)
            vkFreeMemory(vkDevice, m_depthVkMemory, nullptr);

        if (m_renderPass != VK_NULL_HANDLE)
            vkDestroyRenderPass(vkDevice, m_renderPass, nullptr);
    }

    void VulkanGraphicsPipeline::bind()
    {
        // Binding is handled by command buffer
        // This method exists for interface consistency
    }

    void VulkanGraphicsPipeline::createRenderPass()
    {
        VkDevice vkDevice = m_device.vkDevice();

        // Color attachment
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = VK_FORMAT_R8G8B8A8_UNORM;  // TODO: use config.colorFormat
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // Leave in GENERAL after the pass so a subsequent blit (e.g. via
        // VulkanPresenter::presentComposite) can transition GENERAL→TRANSFER_SRC
        // without the contents being discarded.
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkAttachmentReference colorAttachmentRef{};
        colorAttachmentRef.attachment = 0;
        colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        // Depth attachment (optional)
        VkAttachmentDescription depthAttachment{};
        VkAttachmentReference depthAttachmentRef{};

        std::vector<VkAttachmentDescription> attachments = {colorAttachment};

        if (m_config.useDepthBuffer)
        {
            depthAttachment.format = VK_FORMAT_D32_SFLOAT;
            depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
            depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

            depthAttachmentRef.attachment = 1;
            depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

            attachments.push_back(depthAttachment);
        }

        // Subpass
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorAttachmentRef;
        if (m_config.useDepthBuffer)
        {
            subpass.pDepthStencilAttachment = &depthAttachmentRef;
        }

        // Subpass dependency for layout transitions
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        // Create render pass
        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        renderPassInfo.pAttachments = attachments.data();
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;

        if (vkCreateRenderPass(vkDevice, &renderPassInfo, nullptr, &m_renderPass) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create render pass");
        }
    }

    void VulkanGraphicsPipeline::createRenderTargets()
    {
        VkDevice vkDevice = m_device.vkDevice();

        // Create color image. createImage2D() now sets COLOR_ATTACHMENT_BIT on
        // its storage-image usage flags so the image is render-pass-compatible.
        m_colorImage.reset(m_device.createImage2D(m_config.width, m_config.height, m_config.colorFormat));
        auto* vkColorImage = static_cast<VulkanImage2D*>(m_colorImage.get());
        m_colorImageView = vkColorImage->vkImageView();

        // Depth attachment: D32_SFLOAT with DEPTH aspect. The Image2D factory
        // doesn't model depth-formatted images, so we own the raw Vulkan
        // handles directly.
        if (m_config.useDepthBuffer)
        {
            VkImageCreateInfo imgInfo{};
            imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imgInfo.imageType = VK_IMAGE_TYPE_2D;
            imgInfo.extent = {m_config.width, m_config.height, 1};
            imgInfo.mipLevels = 1;
            imgInfo.arrayLayers = 1;
            imgInfo.format = VK_FORMAT_D32_SFLOAT;
            imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            imgInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateImage(vkDevice, &imgInfo, nullptr, &m_depthVkImage) != VK_SUCCESS)
                throw std::runtime_error("Failed to create depth image");

            VkMemoryRequirements memReq;
            vkGetImageMemoryRequirements(vkDevice, m_depthVkImage, &memReq);
            VkMemoryAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize = memReq.size;
            allocInfo.memoryTypeIndex = m_device.findMemoryType(
                memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (vkAllocateMemory(vkDevice, &allocInfo, nullptr, &m_depthVkMemory) != VK_SUCCESS)
                throw std::runtime_error("Failed to allocate depth image memory");
            vkBindImageMemory(vkDevice, m_depthVkImage, m_depthVkMemory, 0);

            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = m_depthVkImage;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = VK_FORMAT_D32_SFLOAT;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;
            if (vkCreateImageView(vkDevice, &viewInfo, nullptr, &m_depthImageView) != VK_SUCCESS)
                throw std::runtime_error("Failed to create depth image view");
        }
    }

    void VulkanGraphicsPipeline::createFramebuffer()
    {
        VkDevice vkDevice = m_device.vkDevice();

        // TODO: Create framebuffer with color and depth image views
        // Requires image views to be created in createRenderTargets()
        // This is a placeholder that will be completed in full implementation

        std::vector<VkImageView> attachments;
        if (m_colorImageView != VK_NULL_HANDLE)
        {
            attachments.push_back(m_colorImageView);
        }
        if (m_config.useDepthBuffer && m_depthImageView != VK_NULL_HANDLE)
        {
            attachments.push_back(m_depthImageView);
        }

        // Only create framebuffer if we have valid image views
        if (!attachments.empty())
        {
            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = m_renderPass;
            framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
            framebufferInfo.pAttachments = attachments.data();
            framebufferInfo.width = m_config.width;
            framebufferInfo.height = m_config.height;
            framebufferInfo.layers = 1;

            if (vkCreateFramebuffer(vkDevice, &framebufferInfo, nullptr, &m_framebuffer) != VK_SUCCESS)
            {
                throw std::runtime_error("Failed to create framebuffer");
            }
        }
    }

    void VulkanGraphicsPipeline::createDescriptorSetLayout()
    {
        VkDevice vkDevice = m_device.vkDevice();

        // Binding 0: lights SSBO (std430). Shared with the path tracer's
        // CompiledScene::lightBuffer so both renderers source identical
        // light parameters. Marked optional in the shader (skipped when
        // pc.lightCount == 0) so a pipeline without a bound lights buffer
        // still issues — but the binding itself MUST exist in the layout
        // to keep the descriptor set valid; Rasterizer always binds the
        // engine's lightBuffer (always at least one dummy entry — see
        // SceneCompiler::compile).
        VkDescriptorSetLayoutBinding lightsBinding{};
        lightsBinding.binding         = 0;
        lightsBinding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        lightsBinding.descriptorCount = 1;
        lightsBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        lightsBinding.pImmutableSamplers = nullptr;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings    = &lightsBinding;

        if (vkCreateDescriptorSetLayout(vkDevice, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create descriptor set layout");
        }
    }

    void VulkanGraphicsPipeline::createPipelineLayout()
    {
        VkDevice vkDevice = m_device.vkDevice();

        // Push constant range layout (96 bytes — comfortably under
        // Vulkan's 128-byte minimum guarantee on every implementation
        // we ship to, including MoltenVK on Apple Silicon):
        //   offset  0..63   mat4 viewProj      (shared with vertex shaders)
        //   offset 64..79   vec4 viewPos       (camera world pos for PBR)
        //   offset 80..95   uvec4 misc         (.x = lightCount; rest pad)
        //
        // Other pipelines that share this layout (ground, points, lines,
        // gizmo, edges) reinterpret the first 80 bytes via their own
        // shader structs — none of them read the misc slot, so the
        // extension is backwards-compatible.
        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstantRange.offset = 0;
        pushConstantRange.size = 96;

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &m_descriptorSetLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

        if (vkCreatePipelineLayout(vkDevice, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create pipeline layout");
        }
    }

    void VulkanGraphicsPipeline::createPipeline()
    {
        VkDevice vkDevice = m_device.vkDevice();

        // Load shaders
        auto vertShaderCode = readShaderFile(m_config.vertexShader);
        auto fragShaderCode = readShaderFile(m_config.fragmentShader);

        m_vertexShaderModule = createShaderModule(vkDevice, vertShaderCode);
        m_fragmentShaderModule = createShaderModule(vkDevice, fragShaderCode);

        // Shader stages
        VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
        vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertShaderStageInfo.module = m_vertexShaderModule;
        vertShaderStageInfo.pName = "main";

        VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
        fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragShaderStageInfo.module = m_fragmentShaderModule;
        fragShaderStageInfo.pName = "main";

        VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};

        // Vertex input: three buffers — two per-vertex, one per-instance.
        //   binding 0 = position (vec3) — also fed to the path-tracer BLAS.
        //   binding 1 = vertex color Cd (vec3) — written by the VOP graph
        //               and consumed by position_only.vert; the SceneCompiler
        //               always allocates this buffer (defaults to white)
        //               so the pipeline's expectation always holds.
        //   binding 2 = per-instance (mat4 model + vec4 color), stride 80,
        //               INPUT_RATE_INSTANCE. The rasterizer fills this with
        //               one row per scene-instance and issues a single
        //               vkCmdDraw(instanceCount=N) per BLAS group — the
        //               batching path that scales past 10k particles
        //               without the CPU-side draw-call overhead.
        // Per-instance stride is now 7 vec4 = 112 bytes:
        //   loc 2..5: mat4 model
        //   loc 6:    vec4 albedo (rgb=albedo, a=opacity)
        //   loc 7:    vec4 mrx    (x=metallic, y=roughness, z=emission strength, w=unused)
        //   loc 8:    vec4 emissive (rgb=emission color, a=unused)
        // The rasterizer fragment shader runs a Cook-Torrance/GGX BRDF
        // off these, so per-particle material variation (tint, roughness
        // ramp, emissive sparks) shows up live in the preview viewport.
        std::array<VkVertexInputBindingDescription, 3> bindingDescriptions{};
        bindingDescriptions[0].binding = 0;
        bindingDescriptions[0].stride = sizeof(float) * 3;
        bindingDescriptions[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        bindingDescriptions[1].binding = 1;
        bindingDescriptions[1].stride = sizeof(float) * 3;
        bindingDescriptions[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        bindingDescriptions[2].binding = 2;
        bindingDescriptions[2].stride = sizeof(float) * 28;  // 7 vec4
        bindingDescriptions[2].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

        std::array<VkVertexInputAttributeDescription, 9> attributeDescriptions{};
        // Position (location 0, binding 0)
        attributeDescriptions[0].binding = 0;
        attributeDescriptions[0].location = 0;
        attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[0].offset = 0;
        // Per-vertex color (location 1, binding 1)
        attributeDescriptions[1].binding = 1;
        attributeDescriptions[1].location = 1;
        attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[1].offset = 0;
        // Per-instance model matrix columns (locations 2–5, binding 2).
        // GLSL mat4 must be split into four vec4 attributes because each
        // input location can only hold a vec4.
        for (uint32_t col = 0; col < 4; ++col) {
            attributeDescriptions[2 + col].binding  = 2;
            attributeDescriptions[2 + col].location = 2 + col;
            attributeDescriptions[2 + col].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
            attributeDescriptions[2 + col].offset   = col * sizeof(float) * 4;
        }
        // Per-instance albedo + opacity (location 6, binding 2)
        attributeDescriptions[6].binding  = 2;
        attributeDescriptions[6].location = 6;
        attributeDescriptions[6].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        attributeDescriptions[6].offset   = 16 * sizeof(float);
        // Per-instance metallic / roughness / emissive strength (location 7)
        attributeDescriptions[7].binding  = 2;
        attributeDescriptions[7].location = 7;
        attributeDescriptions[7].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        attributeDescriptions[7].offset   = 20 * sizeof(float);
        // Per-instance emissive colour (location 8)
        attributeDescriptions[8].binding  = 2;
        attributeDescriptions[8].location = 8;
        attributeDescriptions[8].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        attributeDescriptions[8].offset   = 24 * sizeof(float);

        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(bindingDescriptions.size());
        vertexInputInfo.pVertexBindingDescriptions = bindingDescriptions.data();
        vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
        vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

        // Input assembly
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology =
            (m_config.topology == PrimitiveTopology::PointList)
                ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssembly.primitiveRestartEnable = VK_FALSE;

        // Viewport and scissor
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(m_config.width);
        viewport.height = static_cast<float>(m_config.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = {m_config.width, m_config.height};

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;

        // Rasterization
        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = m_config.cullBackFaces ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.depthBiasEnable = VK_FALSE;

        // Multisampling
        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.sampleShadingEnable = VK_FALSE;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Depth stencil
        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = m_config.depthTestEnable ? VK_TRUE : VK_FALSE;
        depthStencil.depthWriteEnable = m_config.depthWriteEnable ? VK_TRUE : VK_FALSE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
        depthStencil.depthBoundsTestEnable = VK_FALSE;
        depthStencil.stencilTestEnable = VK_FALSE;

        // Color blending
        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = m_config.alphaBlending ? VK_TRUE : VK_FALSE;
        colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.logicOpEnable = VK_FALSE;
        colorBlending.logicOp = VK_LOGIC_OP_COPY;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;

        // Create graphics pipeline
        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shaderStages;
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = m_config.useDepthBuffer ? &depthStencil : nullptr;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.layout = m_pipelineLayout;
        pipelineInfo.renderPass = m_renderPass;
        pipelineInfo.subpass = 0;
        pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

        if (vkCreateGraphicsPipelines(vkDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create graphics pipeline");
        }

        // Destroy shader modules (no longer needed after pipeline creation)
        vkDestroyShaderModule(vkDevice, m_vertexShaderModule, nullptr);
        vkDestroyShaderModule(vkDevice, m_fragmentShaderModule, nullptr);
        m_vertexShaderModule = VK_NULL_HANDLE;
        m_fragmentShaderModule = VK_NULL_HANDLE;
    }

    void VulkanGraphicsPipeline::createPointsPipeline()
    {
        VkDevice vkDevice = m_device.vkDevice();

        auto vertCode = readShaderFile(m_config.pointsVertexShader);
        auto fragCode = readShaderFile(m_config.pointsFragmentShader);
        VkShaderModule vert = createShaderModule(vkDevice, vertCode);
        VkShaderModule frag = createShaderModule(vkDevice, fragCode);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";

        // Two parallel buffers, same shape as the triangle pipeline:
        //   binding 0 = position (vec3)
        //   binding 1 = Cd (vec3) — the per-vertex colour the VOP graph
        //               writes; multiplied with the push-constant
        //               baseColor in the vertex shader so an
        //               attribute-VOP-driven Cd actually lights up
        //               point sprites instead of being silently
        //               ignored. The SceneCompiler always allocates a
        //               white-default Cd buffer when Cd is missing on
        //               the geometry, so this binding is safe even when
        //               the user hasn't touched colour.
        std::array<VkVertexInputBindingDescription, 2> bindings{};
        bindings[0].binding = 0;
        bindings[0].stride = sizeof(float) * 3;
        bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        bindings[1].binding = 1;
        bindings[1].stride = sizeof(float) * 3;
        bindings[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        std::array<VkVertexInputAttributeDescription, 2> attrs{};
        attrs[0].binding = 0;
        attrs[0].location = 0;
        attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[0].offset = 0;
        attrs[1].binding = 1;
        attrs[1].location = 1;
        attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[1].offset = 0;
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
        vertexInput.pVertexBindingDescriptions = bindings.data();
        vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
        vertexInput.pVertexAttributeDescriptions = attrs.data();

        // POINT_LIST topology — every vertex becomes a point sprite.
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

        VkViewport viewport{};
        viewport.x = 0; viewport.y = 0;
        viewport.width = static_cast<float>(m_config.width);
        viewport.height = static_cast<float>(m_config.height);
        viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;
        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = {m_config.width, m_config.height};
        VkPipelineViewportStateCreateInfo vpState{};
        vpState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vpState.viewportCount = 1; vpState.pViewports = &viewport;
        vpState.scissorCount = 1; vpState.pScissors = &scissor;

        VkPipelineRasterizationStateCreateInfo rast{};
        rast.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rast.polygonMode = VK_POLYGON_MODE_FILL;
        rast.lineWidth = 1.0f;
        rast.cullMode = VK_CULL_MODE_NONE;
        rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Depth test on, depth write off — points sit on top without
        // occluding subsequent draws.
        VkPipelineDepthStencilStateCreateInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth.depthTestEnable = m_config.useDepthBuffer ? VK_TRUE : VK_FALSE;
        depth.depthWriteEnable = VK_FALSE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        // Standard alpha blending for the antialiased circle edge.
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &blend;

        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &ia;
        info.pViewportState = &vpState;
        info.pRasterizationState = &rast;
        info.pMultisampleState = &ms;
        info.pDepthStencilState = m_config.useDepthBuffer ? &depth : nullptr;
        info.pColorBlendState = &cb;
        info.layout = m_pipelineLayout;
        info.renderPass = m_renderPass;
        info.subpass = 0;

        if (vkCreateGraphicsPipelines(vkDevice, VK_NULL_HANDLE, 1, &info, nullptr, &m_pointsPipeline) != VK_SUCCESS)
        {
            vkDestroyShaderModule(vkDevice, vert, nullptr);
            vkDestroyShaderModule(vkDevice, frag, nullptr);
            throw std::runtime_error("Failed to create points pipeline");
        }
        vkDestroyShaderModule(vkDevice, vert, nullptr);
        vkDestroyShaderModule(vkDevice, frag, nullptr);
    }

    void VulkanGraphicsPipeline::createLinesPipeline()
    {
        VkDevice vkDevice = m_device.vkDevice();

        auto vertCode = readShaderFile(m_config.linesVertexShader);
        auto fragCode = readShaderFile(m_config.linesFragmentShader);
        VkShaderModule vert = createShaderModule(vkDevice, vertCode);
        VkShaderModule frag = createShaderModule(vkDevice, fragCode);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";

        // Same vertex input as the triangle pipeline (position-only vec3).
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(float) * 3;
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription attr{};
        attr.binding = 0;
        attr.location = 0;
        attr.format = VK_FORMAT_R32G32B32_SFLOAT;
        attr.offset = 0;
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 1;
        vertexInput.pVertexAttributeDescriptions = &attr;

        // Triangle topology + line polygon mode = wireframe.
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkViewport viewport{};
        viewport.width = static_cast<float>(m_config.width);
        viewport.height = static_cast<float>(m_config.height);
        viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;
        VkRect2D scissor{};
        scissor.extent = {m_config.width, m_config.height};
        VkPipelineViewportStateCreateInfo vpState{};
        vpState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vpState.viewportCount = 1; vpState.pViewports = &viewport;
        vpState.scissorCount = 1; vpState.pScissors = &scissor;

        VkPipelineRasterizationStateCreateInfo rast{};
        rast.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rast.polygonMode = VK_POLYGON_MODE_LINE;  // wireframe
        rast.lineWidth = 1.0f;
        rast.cullMode = VK_CULL_MODE_NONE;
        rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        // Slight depth bias toward camera so wireframe sits on top of fills.
        rast.depthBiasEnable = VK_TRUE;
        rast.depthBiasConstantFactor = -1.0f;
        rast.depthBiasSlopeFactor = -1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Depth test on, depth write off — edges layer over fills, don't
        // occlude further draws.
        VkPipelineDepthStencilStateCreateInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth.depthTestEnable = m_config.useDepthBuffer ? VK_TRUE : VK_FALSE;
        depth.depthWriteEnable = VK_FALSE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        // Opaque, no blending — the lines.frag dims the base color slightly
        // so edges read against the fill.
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blend.blendEnable = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &blend;

        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &ia;
        info.pViewportState = &vpState;
        info.pRasterizationState = &rast;
        info.pMultisampleState = &ms;
        info.pDepthStencilState = m_config.useDepthBuffer ? &depth : nullptr;
        info.pColorBlendState = &cb;
        info.layout = m_pipelineLayout;
        info.renderPass = m_renderPass;
        info.subpass = 0;

        if (vkCreateGraphicsPipelines(vkDevice, VK_NULL_HANDLE, 1, &info, nullptr, &m_linesPipeline) != VK_SUCCESS)
        {
            vkDestroyShaderModule(vkDevice, vert, nullptr);
            vkDestroyShaderModule(vkDevice, frag, nullptr);
            throw std::runtime_error("Failed to create lines pipeline");
        }
        vkDestroyShaderModule(vkDevice, vert, nullptr);
        vkDestroyShaderModule(vkDevice, frag, nullptr);
    }

    void VulkanGraphicsPipeline::createGroundPipeline()
    {
        VkDevice vkDevice = m_device.vkDevice();

        auto vertCode = readShaderFile(m_config.groundVertexShader);
        auto fragCode = readShaderFile(m_config.groundFragmentShader);
        VkShaderModule vert = createShaderModule(vkDevice, vertCode);
        VkShaderModule frag = createShaderModule(vkDevice, fragCode);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";

        // Empty vertex input — the vertex shader emits the 4 quad corners
        // procedurally from gl_VertexIndex.
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        // MoltenVK/Metal always has primitive restart on for strip topologies
        // and warns when Vulkan tries to disable it. Enabling here matches
        // Metal's behaviour and silences the validation noise; harmless for
        // a 4-vertex strip with no index buffer.
        ia.primitiveRestartEnable = VK_TRUE;

        VkViewport viewport{};
        viewport.width  = static_cast<float>(m_config.width);
        viewport.height = static_cast<float>(m_config.height);
        viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;
        VkRect2D scissor{};
        scissor.extent = {m_config.width, m_config.height};
        VkPipelineViewportStateCreateInfo vpState{};
        vpState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vpState.viewportCount = 1; vpState.pViewports = &viewport;
        vpState.scissorCount  = 1; vpState.pScissors  = &scissor;

        VkPipelineRasterizationStateCreateInfo rast{};
        rast.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rast.polygonMode = VK_POLYGON_MODE_FILL;
        rast.lineWidth = 1.0f;
        rast.cullMode = VK_CULL_MODE_NONE;  // both sides visible
        rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Depth-test on so geometry occludes the grid; depth-write off so the
        // transparent grid doesn't punch into the depth buffer.
        VkPipelineDepthStencilStateCreateInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth.depthTestEnable  = m_config.useDepthBuffer ? VK_TRUE : VK_FALSE;
        depth.depthWriteEnable = VK_FALSE;
        depth.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

        // Standard alpha blending so the grid composites over the cleared
        // background (and over any opaque triangles drawn earlier).
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blend.blendEnable         = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp        = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaBlendOp        = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &blend;

        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &ia;
        info.pViewportState = &vpState;
        info.pRasterizationState = &rast;
        info.pMultisampleState = &ms;
        info.pDepthStencilState = m_config.useDepthBuffer ? &depth : nullptr;
        info.pColorBlendState = &cb;
        info.layout = m_pipelineLayout;
        info.renderPass = m_renderPass;
        info.subpass = 0;

        if (vkCreateGraphicsPipelines(vkDevice, VK_NULL_HANDLE, 1, &info, nullptr, &m_groundPipeline) != VK_SUCCESS)
        {
            vkDestroyShaderModule(vkDevice, vert, nullptr);
            vkDestroyShaderModule(vkDevice, frag, nullptr);
            throw std::runtime_error("Failed to create ground pipeline");
        }
        vkDestroyShaderModule(vkDevice, vert, nullptr);
        vkDestroyShaderModule(vkDevice, frag, nullptr);
    }

    void VulkanGraphicsPipeline::createGizmoPipeline()
    {
        VkDevice vkDevice = m_device.vkDevice();

        auto vertCode = readShaderFile(m_config.gizmoVertexShader);
        auto fragCode = readShaderFile(m_config.gizmoFragmentShader);
        VkShaderModule vert = createShaderModule(vkDevice, vertCode);
        VkShaderModule frag = createShaderModule(vkDevice, fragCode);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName  = "main";

        // No vertex buffer — the vertex shader emits 6 verts (3 axis lines)
        // procedurally from gl_VertexIndex.
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

        VkViewport viewport{};
        viewport.width  = static_cast<float>(m_config.width);
        viewport.height = static_cast<float>(m_config.height);
        viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;
        VkRect2D scissor{};
        scissor.extent = {m_config.width, m_config.height};
        VkPipelineViewportStateCreateInfo vpState{};
        vpState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vpState.viewportCount = 1; vpState.pViewports = &viewport;
        vpState.scissorCount  = 1; vpState.pScissors  = &scissor;

        // lineWidth must be 1.0 unless the device's wideLines feature is
        // enabled (or LINE_WIDTH is part of dynamic state). MoltenVK +
        // validation flags any other value as an error; we don't pay for
        // the feature for one cosmetic gizmo. The translate gizmo is
        // still readable at 1px against the muted background.
        VkPipelineRasterizationStateCreateInfo rast{};
        rast.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rast.polygonMode = VK_POLYGON_MODE_LINE;
        rast.lineWidth = 1.0f;
        rast.cullMode = VK_CULL_MODE_NONE;
        rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Depth-test OFF so the gizmo always reads on top of geometry,
        // even when the anchor is behind a wall. Maya/Blender both do this.
        VkPipelineDepthStencilStateCreateInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth.depthTestEnable  = VK_FALSE;
        depth.depthWriteEnable = VK_FALSE;
        depth.depthCompareOp   = VK_COMPARE_OP_ALWAYS;

        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blend.blendEnable = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &blend;

        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &ia;
        info.pViewportState = &vpState;
        info.pRasterizationState = &rast;
        info.pMultisampleState = &ms;
        info.pDepthStencilState = m_config.useDepthBuffer ? &depth : nullptr;
        info.pColorBlendState = &cb;
        info.layout = m_pipelineLayout;
        info.renderPass = m_renderPass;
        info.subpass = 0;

        if (vkCreateGraphicsPipelines(vkDevice, VK_NULL_HANDLE, 1, &info, nullptr, &m_gizmoPipeline) != VK_SUCCESS)
        {
            vkDestroyShaderModule(vkDevice, vert, nullptr);
            vkDestroyShaderModule(vkDevice, frag, nullptr);
            throw std::runtime_error("Failed to create gizmo pipeline");
        }
        vkDestroyShaderModule(vkDevice, vert, nullptr);
        vkDestroyShaderModule(vkDevice, frag, nullptr);
    }
}
