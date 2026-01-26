#pragma once

#include <volk.h>
#include <cstdint>
#include <vector>

namespace tracey
{
    class VulkanContext;
    class VulkanImage2D;

    /// Configuration for presentation
    struct PresenterConfig
    {
        uint32_t width = 1280;
        uint32_t height = 720;

        // Preferred surface format (may fallback if unsupported)
        VkFormat preferredFormat = VK_FORMAT_B8G8R8A8_SRGB;
        VkColorSpaceKHR preferredColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

        // Present mode (FIFO is guaranteed, others optional)
        VkPresentModeKHR preferredPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;

        // Swapchain image count (2 = double buffer, 3 = triple buffer)
        uint32_t desiredImageCount = 3;

        // Enable HDR (requires compatible display)
        bool enableHDR = false;
    };

    /// Manages swapchain and presentation for a window surface
    /// Handles format conversion, synchronization, and resize
    class VulkanPresenter
    {
    public:
        /// Create presenter for a window surface
        /// @param context Vulkan context (must outlive presenter)
        /// @param surface VkSurfaceKHR created from native window handle
        /// @param config Presentation configuration
        VulkanPresenter(VulkanContext &context, VkSurfaceKHR surface,
                        const PresenterConfig &config);
        ~VulkanPresenter();

        VulkanPresenter(const VulkanPresenter &) = delete;
        VulkanPresenter &operator=(const VulkanPresenter &) = delete;

        /// Present a rendered image to the window
        /// Handles format conversion if needed
        /// @param sourceImage Image to present (any format)
        /// @param waitForRender If true, waits for GPU rendering to complete
        /// @return true on success, false if swapchain needs recreation
        bool present(VulkanImage2D *sourceImage, bool waitForRender = true);

        /// Present a rendered image to a specific region of the swapchain
        /// Used for viewport-based rendering with full-window swapchain
        /// @param sourceImage Image to present (any format)
        /// @param dstX X position in swapchain
        /// @param dstY Y position in swapchain
        /// @param dstWidth Width in swapchain
        /// @param dstHeight Height in swapchain
        /// @param waitForRender If true, waits for GPU rendering to complete
        /// @return true on success, false if swapchain needs recreation
        bool presentToRegion(VulkanImage2D *sourceImage,
                           int32_t dstX, int32_t dstY,
                           uint32_t dstWidth, uint32_t dstHeight,
                           bool waitForRender = true);

        /// Resize the swapchain (call on window resize)
        /// @param newWidth New width in pixels
        /// @param newHeight New height in pixels
        void resize(uint32_t newWidth, uint32_t newHeight);

        /// Check if swapchain needs recreation (after present fails)
        bool needsRecreation() const { return m_needsRecreation; }

        /// Recreate swapchain (after resize or present failure)
        void recreateSwapchain();

        /// Wait for all presentation operations to complete
        void waitIdle();

        // Accessors
        uint32_t width() const { return m_config.width; }
        uint32_t height() const { return m_config.height; }
        VkFormat swapchainFormat() const { return m_surfaceFormat.format; }
        VkColorSpaceKHR colorSpace() const { return m_surfaceFormat.colorSpace; }
        bool isHDR() const { return m_isHDR; }

    private:
        // Swapchain lifecycle
        void createSwapchain();
        void destroySwapchain();
        void createSwapchainImageViews();
        void destroySwapchainImageViews();

        // Surface capability queries
        void querySurfaceCapabilities();
        VkSurfaceFormatKHR chooseSurfaceFormat(
            const std::vector<VkSurfaceFormatKHR> &availableFormats);
        VkPresentModeKHR choosePresentMode(
            const std::vector<VkPresentModeKHR> &availableModes);
        VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities);

        // Synchronization
        void createSyncObjects();
        void destroySyncObjects();

        // Format conversion
        void blitToSwapchain(VulkanImage2D *sourceImage, uint32_t swapchainImageIndex,
                             VkCommandBuffer cmd);

        // Region-based blitting for viewport rendering
        void blitToSwapchainRegion(VulkanImage2D *sourceImage, uint32_t swapchainImageIndex,
                                   int32_t dstX, int32_t dstY,
                                   uint32_t dstWidth, uint32_t dstHeight,
                                   VkCommandBuffer cmd);

        bool needsFormatConversion(VkFormat sourceFormat) const;

        // Command buffer management
        VkCommandBuffer beginFrame();
        void endFrame(VkCommandBuffer cmd);

        VulkanContext &m_context;
        VkSurfaceKHR m_surface; // Not owned
        PresenterConfig m_config;

        // Swapchain
        VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
        VkSwapchainKHR m_oldSwapchain = VK_NULL_HANDLE; // For recreation
        std::vector<VkImage> m_swapchainImages;
        std::vector<VkImageView> m_swapchainImageViews;
        VkSurfaceFormatKHR m_surfaceFormat;
        VkPresentModeKHR m_presentMode;
        bool m_isHDR = false;
        bool m_needsRecreation = false;

        // Synchronization (per-frame resources)
        struct FrameSync
        {
            VkSemaphore imageAvailableSemaphore = VK_NULL_HANDLE;
            VkSemaphore renderFinishedSemaphore = VK_NULL_HANDLE;
            VkFence inFlightFence = VK_NULL_HANDLE;
        };
        std::vector<FrameSync> m_frameSync;
        uint32_t m_currentFrame = 0;

        // Command pool for blit operations
        VkCommandPool m_commandPool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> m_commandBuffers;

        // Cached surface capabilities
        VkSurfaceCapabilitiesKHR m_surfaceCapabilities;
    };
} // namespace tracey
