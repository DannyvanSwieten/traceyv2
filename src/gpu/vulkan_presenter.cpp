#include "vulkan_presenter.hpp"
#include "vulkan_context.hpp"
#include "vulkan_queue_sync.hpp"
#include "../device/gpu/vulkan_image_2d.hpp"
#include <algorithm>
#include <stdexcept>

namespace tracey
{
    constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3; // Triple buffering

    VulkanPresenter::VulkanPresenter(VulkanContext &context, VkSurfaceKHR surface,
                                     const PresenterConfig &config)
        : m_context(context), m_surface(surface), m_config(config)
    {
        if (m_surface == VK_NULL_HANDLE)
        {
            throw std::runtime_error("Invalid surface handle");
        }

        // Query surface capabilities
        querySurfaceCapabilities();

        // Create swapchain
        createSwapchain();
        createSwapchainImageViews();

        // Create synchronization objects
        createSyncObjects();

        // Create command pool for blit operations
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = m_context.graphicsQueueFamilyIndex();
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        if (vkCreateCommandPool(m_context.device(), &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create command pool");
        }

        // Allocate command buffers (one per frame in flight)
        m_commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = MAX_FRAMES_IN_FLIGHT;

        if (vkAllocateCommandBuffers(m_context.device(), &allocInfo, m_commandBuffers.data()) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to allocate command buffers");
        }
    }

    VulkanPresenter::~VulkanPresenter()
    {
        waitIdle();

        // Destroy command buffers
        if (m_commandPool != VK_NULL_HANDLE)
        {
            vkFreeCommandBuffers(m_context.device(), m_commandPool,
                               static_cast<uint32_t>(m_commandBuffers.size()),
                               m_commandBuffers.data());
            vkDestroyCommandPool(m_context.device(), m_commandPool, nullptr);
        }

        // Destroy synchronization objects
        destroySyncObjects();

        // Destroy swapchain
        destroySwapchainImageViews();
        destroySwapchain();

        // Note: Surface is not owned by presenter, caller must destroy it
    }

    void VulkanPresenter::querySurfaceCapabilities()
    {
        VkPhysicalDevice physicalDevice = m_context.physicalDevice();

        // Get surface capabilities
        VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, m_surface, &m_surfaceCapabilities);
        if (result != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to get surface capabilities: error code " + std::to_string(result));
        }

        // Get surface formats
        uint32_t formatCount;
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, m_surface, &formatCount, nullptr);
        if (result != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to get surface format count: error code " + std::to_string(result));
        }
        if (formatCount == 0)
        {
            throw std::runtime_error("No surface formats available");
        }

        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, m_surface, &formatCount, formats.data());
        if (result != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to get surface formats: error code " + std::to_string(result));
        }

        m_surfaceFormat = chooseSurfaceFormat(formats);

        // Get present modes
        uint32_t presentModeCount;
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, m_surface, &presentModeCount, nullptr);
        if (result != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to get present mode count: error code " + std::to_string(result));
        }
        if (presentModeCount == 0)
        {
            throw std::runtime_error("No present modes available");
        }

        std::vector<VkPresentModeKHR> presentModes(presentModeCount);
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, m_surface, &presentModeCount, presentModes.data());
        if (result != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to get present modes: error code " + std::to_string(result));
        }

        m_presentMode = choosePresentMode(presentModes);
    }

    VkSurfaceFormatKHR VulkanPresenter::chooseSurfaceFormat(
        const std::vector<VkSurfaceFormatKHR> &availableFormats)
    {
        // Check for HDR support
        if (m_config.enableHDR)
        {
            for (const auto &format : availableFormats)
            {
                if (format.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
                    format.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT)
                {
                    m_isHDR = true;
                    return format;
                }
            }
        }

        // Fallback to preferred LDR format
        for (const auto &format : availableFormats)
        {
            if (format.format == m_config.preferredFormat &&
                format.colorSpace == m_config.preferredColorSpace)
            {
                return format;
            }
        }

        // If preferred not available, return first available
        return availableFormats[0];
    }

    VkPresentModeKHR VulkanPresenter::choosePresentMode(
        const std::vector<VkPresentModeKHR> &availableModes)
    {
        // Try to find preferred mode
        for (const auto &mode : availableModes)
        {
            if (mode == m_config.preferredPresentMode)
            {
                return mode;
            }
        }

        // FIFO is guaranteed to be available
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D VulkanPresenter::chooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities)
    {
        if (capabilities.currentExtent.width != UINT32_MAX)
        {
            return capabilities.currentExtent;
        }

        VkExtent2D actualExtent = {m_config.width, m_config.height};

        actualExtent.width = std::max(capabilities.minImageExtent.width,
                                     std::min(capabilities.maxImageExtent.width, actualExtent.width));
        actualExtent.height = std::max(capabilities.minImageExtent.height,
                                      std::min(capabilities.maxImageExtent.height, actualExtent.height));

        return actualExtent;
    }

    void VulkanPresenter::createSwapchain()
    {
        // Validate surface
        if (m_surface == VK_NULL_HANDLE)
        {
            throw std::runtime_error("Surface is null");
        }

        // Validate device
        if (m_context.device() == VK_NULL_HANDLE)
        {
            throw std::runtime_error("Device is null");
        }

        VkExtent2D extent = chooseSwapExtent(m_surfaceCapabilities);

        // Validate extent
        if (extent.width == 0 || extent.height == 0)
        {
            throw std::runtime_error("Invalid swapchain extent: " + std::to_string(extent.width) + "x" + std::to_string(extent.height));
        }

        uint32_t imageCount = m_config.desiredImageCount;
        if (m_surfaceCapabilities.maxImageCount > 0 && imageCount > m_surfaceCapabilities.maxImageCount)
        {
            imageCount = m_surfaceCapabilities.maxImageCount;
        }
        if (imageCount < m_surfaceCapabilities.minImageCount)
        {
            imageCount = m_surfaceCapabilities.minImageCount;
        }

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.pNext = nullptr;
        createInfo.surface = m_surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = m_surfaceFormat.format;
        createInfo.imageColorSpace = m_surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        // Assume graphics queue supports presentation
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        createInfo.queueFamilyIndexCount = 0;
        createInfo.pQueueFamilyIndices = nullptr;

        createInfo.preTransform = m_surfaceCapabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = m_presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = m_oldSwapchain;

        if (vkCreateSwapchainKHR(m_context.device(), &createInfo, nullptr, &m_swapchain) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create swapchain");
        }

        // Destroy old swapchain if it exists
        if (m_oldSwapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(m_context.device(), m_oldSwapchain, nullptr);
            m_oldSwapchain = VK_NULL_HANDLE;
        }

        // Get swapchain images
        uint32_t swapchainImageCount;
        vkGetSwapchainImagesKHR(m_context.device(), m_swapchain, &swapchainImageCount, nullptr);
        m_swapchainImages.resize(swapchainImageCount);
        vkGetSwapchainImagesKHR(m_context.device(), m_swapchain, &swapchainImageCount, m_swapchainImages.data());

        // Update config with actual extent
        m_config.width = extent.width;
        m_config.height = extent.height;
    }

    void VulkanPresenter::destroySwapchain()
    {
        if (m_swapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(m_context.device(), m_swapchain, nullptr);
            m_swapchain = VK_NULL_HANDLE;
        }
    }

    void VulkanPresenter::createSwapchainImageViews()
    {
        m_swapchainImageViews.resize(m_swapchainImages.size());

        for (size_t i = 0; i < m_swapchainImages.size(); i++)
        {
            VkImageViewCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            createInfo.image = m_swapchainImages[i];
            createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            createInfo.format = m_surfaceFormat.format;
            createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            createInfo.subresourceRange.baseMipLevel = 0;
            createInfo.subresourceRange.levelCount = 1;
            createInfo.subresourceRange.baseArrayLayer = 0;
            createInfo.subresourceRange.layerCount = 1;

            if (vkCreateImageView(m_context.device(), &createInfo, nullptr, &m_swapchainImageViews[i]) != VK_SUCCESS)
            {
                throw std::runtime_error("Failed to create image view");
            }
        }
    }

    void VulkanPresenter::destroySwapchainImageViews()
    {
        for (auto imageView : m_swapchainImageViews)
        {
            vkDestroyImageView(m_context.device(), imageView, nullptr);
        }
        m_swapchainImageViews.clear();
    }

    void VulkanPresenter::createSyncObjects()
    {
        m_frameSync.resize(MAX_FRAMES_IN_FLIGHT);

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // Start signaled

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            if (vkCreateSemaphore(m_context.device(), &semaphoreInfo, nullptr, &m_frameSync[i].imageAvailableSemaphore) != VK_SUCCESS ||
                vkCreateSemaphore(m_context.device(), &semaphoreInfo, nullptr, &m_frameSync[i].renderFinishedSemaphore) != VK_SUCCESS ||
                vkCreateFence(m_context.device(), &fenceInfo, nullptr, &m_frameSync[i].inFlightFence) != VK_SUCCESS)
            {
                throw std::runtime_error("Failed to create synchronization objects");
            }
        }
    }

    void VulkanPresenter::destroySyncObjects()
    {
        for (auto &sync : m_frameSync)
        {
            if (sync.imageAvailableSemaphore != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(m_context.device(), sync.imageAvailableSemaphore, nullptr);
            }
            if (sync.renderFinishedSemaphore != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(m_context.device(), sync.renderFinishedSemaphore, nullptr);
            }
            if (sync.inFlightFence != VK_NULL_HANDLE)
            {
                vkDestroyFence(m_context.device(), sync.inFlightFence, nullptr);
            }
        }
        m_frameSync.clear();
    }

    bool VulkanPresenter::present(VulkanImage2D *sourceImage, bool /*waitForRender*/)
    {
        // Whole-present lock — covers swapchain acquire, command-pool
        // alloc, record, queue submit + present. See vulkan_queue_sync.hpp.
        std::lock_guard<std::mutex> gpuLock(vulkanQueueMutex());

        if (m_needsRecreation)
        {
            // recreateSwapchain() owns the flag now: it clears it on success
            // and KEEPS it set when it defers on a transient 0×0 surface, so
            // we retry next present instead of acquiring from a stale/missing
            // swapchain.
            recreateSwapchain();
        }

        // Wait for frame N-3 to complete
        vkWaitForFences(m_context.device(), 1, &m_frameSync[m_currentFrame].inFlightFence, VK_TRUE, UINT64_MAX);

        // Acquire next swapchain image
        uint32_t imageIndex;
        VkResult result = vkAcquireNextImageKHR(m_context.device(), m_swapchain, UINT64_MAX,
                                                m_frameSync[m_currentFrame].imageAvailableSemaphore,
                                                VK_NULL_HANDLE, &imageIndex);

        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            m_needsRecreation = true;
            return false;
        }
        else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            throw std::runtime_error("Failed to acquire swapchain image");
        }

        // Reset fence for this frame
        vkResetFences(m_context.device(), 1, &m_frameSync[m_currentFrame].inFlightFence);

        // Record command buffer
        VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        vkBeginCommandBuffer(cmd, &beginInfo);

        // Blit source image to swapchain image
        blitToSwapchain(sourceImage, imageIndex, cmd);

        vkEndCommandBuffer(cmd);

        // Submit command buffer
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

        VkSemaphore waitSemaphores[] = {m_frameSync[m_currentFrame].imageAvailableSemaphore};
        VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;

        VkSemaphore signalSemaphores[] = {m_frameSync[m_currentFrame].renderFinishedSemaphore};
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        // Present
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;

        VkSwapchainKHR swapchains[] = {m_swapchain};
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapchains;
        presentInfo.pImageIndices = &imageIndex;
        presentInfo.pResults = nullptr;

        // Lock held since function entry — see top of present().
        if (vkQueueSubmit(m_context.graphicsQueue(), 1, &submitInfo, m_frameSync[m_currentFrame].inFlightFence) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to submit command buffer");
        }
        result = vkQueuePresentKHR(m_context.graphicsQueue(), &presentInfo);

        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        {
            m_needsRecreation = true;
            return false;
        }
        else if (result != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to present swapchain image");
        }

        // Advance to next frame
        m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;

        return true;
    }

    bool VulkanPresenter::presentToRegion(VulkanImage2D *sourceImage,
                                         int32_t dstX, int32_t dstY,
                                         uint32_t dstWidth, uint32_t dstHeight,
                                         bool /*waitForRender*/)
    {
        // Whole-present lock — see vulkan_queue_sync.hpp.
        std::lock_guard<std::mutex> gpuLock(vulkanQueueMutex());

        if (m_needsRecreation)
        {
            // recreateSwapchain() owns the flag now: it clears it on success
            // and KEEPS it set when it defers on a transient 0×0 surface, so
            // we retry next present instead of acquiring from a stale/missing
            // swapchain.
            recreateSwapchain();
        }

        // Wait for frame N-3 to complete
        vkWaitForFences(m_context.device(), 1, &m_frameSync[m_currentFrame].inFlightFence, VK_TRUE, UINT64_MAX);

        // Acquire next swapchain image
        uint32_t imageIndex;
        VkResult result = vkAcquireNextImageKHR(m_context.device(), m_swapchain, UINT64_MAX,
                                                m_frameSync[m_currentFrame].imageAvailableSemaphore,
                                                VK_NULL_HANDLE, &imageIndex);

        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            m_needsRecreation = true;
            return false;
        }
        else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            throw std::runtime_error("Failed to acquire swapchain image");
        }

        // Reset fence for this frame
        vkResetFences(m_context.device(), 1, &m_frameSync[m_currentFrame].inFlightFence);

        // Record command buffer
        VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        vkBeginCommandBuffer(cmd, &beginInfo);

        // Blit source image to specific region of swapchain image
        blitToSwapchainRegion(sourceImage, imageIndex, dstX, dstY, dstWidth, dstHeight, cmd);

        vkEndCommandBuffer(cmd);

        // Submit command buffer
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

        VkSemaphore waitSemaphores[] = {m_frameSync[m_currentFrame].imageAvailableSemaphore};
        VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;

        VkSemaphore signalSemaphores[] = {m_frameSync[m_currentFrame].renderFinishedSemaphore};
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        // Present
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;

        VkSwapchainKHR swapchains[] = {m_swapchain};
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapchains;
        presentInfo.pImageIndices = &imageIndex;
        presentInfo.pResults = nullptr;

        // Lock held since function entry — see top of present().
        if (vkQueueSubmit(m_context.graphicsQueue(), 1, &submitInfo, m_frameSync[m_currentFrame].inFlightFence) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to submit command buffer");
        }
        result = vkQueuePresentKHR(m_context.graphicsQueue(), &presentInfo);

        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        {
            m_needsRecreation = true;
            return false;
        }
        else if (result != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to present swapchain image");
        }

        // Advance to next frame
        m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;

        return true;
    }

    bool VulkanPresenter::presentComposite(VulkanImage2D *fullscreenSrc,
                                          VulkanImage2D *insetSrc,
                                          int32_t insetX, int32_t insetY,
                                          uint32_t insetWidth, uint32_t insetHeight,
                                          bool /*waitForRender*/)
    {
        // Whole-present lock — see vulkan_queue_sync.hpp.
        std::lock_guard<std::mutex> gpuLock(vulkanQueueMutex());

        if (m_needsRecreation)
        {
            // recreateSwapchain() owns the flag now: it clears it on success
            // and KEEPS it set when it defers on a transient 0×0 surface, so
            // we retry next present instead of acquiring from a stale/missing
            // swapchain.
            recreateSwapchain();
        }

        vkWaitForFences(m_context.device(), 1, &m_frameSync[m_currentFrame].inFlightFence, VK_TRUE, UINT64_MAX);

        uint32_t imageIndex;
        VkResult result = vkAcquireNextImageKHR(m_context.device(), m_swapchain, UINT64_MAX,
                                                m_frameSync[m_currentFrame].imageAvailableSemaphore,
                                                VK_NULL_HANDLE, &imageIndex);

        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            m_needsRecreation = true;
            return false;
        }
        else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            throw std::runtime_error("Failed to acquire swapchain image");
        }

        vkResetFences(m_context.device(), 1, &m_frameSync[m_currentFrame].inFlightFence);

        VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        VkImage dstImage = m_swapchainImages[imageIndex];
        VkImage fullSrc = fullscreenSrc->vkImage();
        VkImage insetSrcImg = insetSrc ? insetSrc->vkImage() : VK_NULL_HANDLE;

        // Helper: barrier transition
        auto transition = [&](VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                              VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                              VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
            VkImageMemoryBarrier b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout = oldLayout;
            b.newLayout = newLayout;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            b.srcAccessMask = srcAccess;
            b.dstAccessMask = dstAccess;
            vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
        };

        // Sources: GENERAL → TRANSFER_SRC.
        transition(fullSrc, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        if (insetSrcImg)
        {
            transition(insetSrcImg, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        }
        // Swapchain image: UNDEFINED → TRANSFER_DST.
        transition(dstImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   0, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        // Blit fullscreen → entire swapchain extent.
        {
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[0] = {0, 0, 0};
            blit.srcOffsets[1] = {static_cast<int32_t>(fullscreenSrc->width()),
                                  static_cast<int32_t>(fullscreenSrc->height()), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[0] = {0, 0, 0};
            blit.dstOffsets[1] = {static_cast<int32_t>(m_config.width),
                                  static_cast<int32_t>(m_config.height), 1};
            vkCmdBlitImage(cmd, fullSrc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &blit, VK_FILTER_LINEAR);
        }

        // Barrier: make the fullscreen blit visible to the inset blit reading
        // the same swapchain image.
        if (insetSrcImg)
        {
            VkMemoryBarrier mem{};
            mem.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            mem.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mem.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 1, &mem, 0, nullptr, 0, nullptr);

            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[0] = {0, 0, 0};
            blit.srcOffsets[1] = {static_cast<int32_t>(insetSrc->width()),
                                  static_cast<int32_t>(insetSrc->height()), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[0] = {insetX, insetY, 0};
            blit.dstOffsets[1] = {insetX + static_cast<int32_t>(insetWidth),
                                  insetY + static_cast<int32_t>(insetHeight), 1};
            vkCmdBlitImage(cmd, insetSrcImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &blit, VK_FILTER_LINEAR);
        }

        // Final layouts.
        transition(dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                   VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        transition(fullSrc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        if (insetSrcImg)
        {
            transition(insetSrcImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                       VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        }

        vkEndCommandBuffer(cmd);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        VkSemaphore waitSemaphores[] = {m_frameSync[m_currentFrame].imageAvailableSemaphore};
        VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        VkSemaphore signalSemaphores[] = {m_frameSync[m_currentFrame].renderFinishedSemaphore};
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;
        VkSwapchainKHR swapchains[] = {m_swapchain};
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapchains;
        presentInfo.pImageIndices = &imageIndex;

        // Lock held since function entry — see top of presentComposite().
        if (vkQueueSubmit(m_context.graphicsQueue(), 1, &submitInfo, m_frameSync[m_currentFrame].inFlightFence) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to submit command buffer");
        }
        result = vkQueuePresentKHR(m_context.graphicsQueue(), &presentInfo);

        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        {
            m_needsRecreation = true;
            return false;
        }
        else if (result != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to present swapchain image");
        }

        m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
        return true;
    }

    void VulkanPresenter::blitToSwapchain(VulkanImage2D *sourceImage, uint32_t swapchainImageIndex,
                                          VkCommandBuffer cmd)
    {
        VkImage srcImage = sourceImage->vkImage();
        VkImage dstImage = m_swapchainImages[swapchainImageIndex];

        // Transition source image to TRANSFER_SRC
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = srcImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Transition dst image to TRANSFER_DST
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.image = dstImage;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Blit
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {static_cast<int32_t>(sourceImage->width()),
                              static_cast<int32_t>(sourceImage->height()), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {static_cast<int32_t>(m_config.width),
                              static_cast<int32_t>(m_config.height), 1};

        vkCmdBlitImage(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     1, &blit, VK_FILTER_LINEAR);

        // Transition dst image to PRESENT
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.image = dstImage;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = 0;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Transition source back to GENERAL
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.image = srcImage;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    void VulkanPresenter::resize(uint32_t newWidth, uint32_t newHeight)
    {
        if (newWidth == m_config.width && newHeight == m_config.height)
        {
            return;
        }

        m_config.width = newWidth;
        m_config.height = newHeight;
        m_needsRecreation = true;
    }

    void VulkanPresenter::recreateSwapchain()
    {
        waitIdle();

        // Query new surface capabilities
        querySurfaceCapabilities();

        // Transient zero-size surface — e.g. switching to the Render tab / PT
        // preview before the window layout settles, where the CAMetalLayer
        // momentarily reports a 0×0 drawable. Creating a swapchain at that
        // extent throws, and because this runs on the present path that throw
        // was uncaught and crashed the app ("can't create a swapchain"). Defer
        // instead: keep the current swapchain intact and leave the recreation
        // flag set so the next present retries once a real size arrives.
        const VkExtent2D extent = chooseSwapExtent(m_surfaceCapabilities);
        if (extent.width == 0 || extent.height == 0)
        {
            m_needsRecreation = true;
            return;
        }

        // Store old swapchain
        m_oldSwapchain = m_swapchain;

        // Destroy old resources
        destroySwapchainImageViews();

        // Create new swapchain
        createSwapchain();
        createSwapchainImageViews();

        // Success — clear the flag here (callers no longer do, so a deferred
        // recreate above can keep it set).
        m_needsRecreation = false;
    }

    void VulkanPresenter::waitIdle()
    {
        vkDeviceWaitIdle(m_context.device());
    }

    void VulkanPresenter::blitToSwapchainRegion(VulkanImage2D *sourceImage, uint32_t swapchainImageIndex,
                                                int32_t dstX, int32_t dstY,
                                                uint32_t dstWidth, uint32_t dstHeight,
                                                VkCommandBuffer cmd)
    {
        VkImage srcImage = sourceImage->vkImage();
        VkImage dstImage = m_swapchainImages[swapchainImageIndex];

        // Transition source image to TRANSFER_SRC
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = srcImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Transition dst image to TRANSFER_DST
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.image = dstImage;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Clear only the viewport region to transparent (not the entire swapchain)
        VkClearColorValue clearColor = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkClearRect clearRect{};
        clearRect.rect.offset = {dstX, dstY};
        clearRect.rect.extent = {dstWidth, dstHeight};
        clearRect.baseArrayLayer = 0;
        clearRect.layerCount = 1;

        VkClearAttachment clearAttachment{};
        clearAttachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clearAttachment.colorAttachment = 0;
        clearAttachment.clearValue.color = clearColor;

        // Note: vkCmdClearAttachments requires a framebuffer/renderpass
        // So instead, we'll just skip clearing and rely on the blit to overwrite
        // The swapchain starts in UNDEFINED layout anyway, so undefined data outside viewport is fine

        // Blit to specific region
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {static_cast<int32_t>(sourceImage->width()),
                              static_cast<int32_t>(sourceImage->height()), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        // Blit to specific region of swapchain (viewport position)
        blit.dstOffsets[0] = {dstX, dstY, 0};
        blit.dstOffsets[1] = {dstX + static_cast<int32_t>(dstWidth),
                              dstY + static_cast<int32_t>(dstHeight), 1};

        vkCmdBlitImage(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     1, &blit, VK_FILTER_LINEAR);

        // Transition dst image to PRESENT
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.image = dstImage;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = 0;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Transition source back to GENERAL
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.image = srcImage;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

} // namespace tracey
