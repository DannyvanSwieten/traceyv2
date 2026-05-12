#pragma once

#include "gpu/vulkan_presenter.hpp"

#include <volk.h>

#include <cstdint>
#include <memory>

namespace tracey {
class Device;
class VulkanContext;
class VulkanImage2D;
class Image2D;
}  // namespace tracey

namespace tracey_editor {

// Owns the VkSurface + VulkanPresenter that draws the path-tracer output into
// the native viewport (CAMetalLayer on macOS). The Device passed in must be a
// present-capable VulkanComputeDevice — i.e. created with
// tracey::createDevice(..., enablePresentation=true).
class ViewportRenderer {
public:
    ViewportRenderer(tracey::Device& device, void* native_surface_handle,
                     void* native_display_handle, uint32_t initial_pixel_w,
                     uint32_t initial_pixel_h);
    ~ViewportRenderer();

    ViewportRenderer(const ViewportRenderer&) = delete;
    ViewportRenderer& operator=(const ViewportRenderer&) = delete;

    // Resize the swapchain to match the new viewport pixel size.
    void resize(uint32_t pixel_w, uint32_t pixel_h);

    // Present the given image to the swapchain (auto-scales).
    // Returns false if the swapchain needs recreation (caller can call resize()).
    bool present(tracey::Image2D* source);

    // Present a fullscreen image with a second image overlaid in a sub-region
    // (picture-in-picture) in a single submit. `inset` may be null to fall back
    // to a fullscreen-only present.
    bool present_composite(tracey::Image2D* fullscreen,
                           tracey::Image2D* inset,
                           int32_t inset_x, int32_t inset_y,
                           uint32_t inset_w, uint32_t inset_h);

private:
    tracey::VulkanContext* m_context = nullptr;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    std::unique_ptr<tracey::VulkanPresenter> m_presenter;
    uint32_t m_pixel_w = 0;
    uint32_t m_pixel_h = 0;
};

}  // namespace tracey_editor
