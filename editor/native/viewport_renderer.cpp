#include "viewport_renderer.hpp"

#include "device/gpu/vulkan_compute_device.hpp"
#include "device/gpu/vulkan_image_2d.hpp"
#include "gpu/vulkan_context.hpp"
#include "gpu/vulkan_surface_factory.hpp"

#include <stdexcept>

namespace tracey_editor {

ViewportRenderer::ViewportRenderer(tracey::Device& device, void* native_surface_handle,
                                   void* native_display_handle, uint32_t initial_pixel_w,
                                   uint32_t initial_pixel_h)
    : m_pixel_w(initial_pixel_w), m_pixel_h(initial_pixel_h) {
    auto* vd = dynamic_cast<tracey::VulkanComputeDevice*>(&device);
    if (!vd)
        throw std::runtime_error(
            "ViewportRenderer requires a VulkanComputeDevice (GPU backend).");

    m_context = &vd->context();
    if (!m_context->config().enablePresentation)
        throw std::runtime_error(
            "ViewportRenderer requires a present-capable VulkanContext "
            "(create the device with enablePresentation=true).");

    std::string err = tracey::VulkanSurfaceFactory::createSurface(
        m_context->instance(), native_surface_handle, native_display_handle, &m_surface);
    if (!err.empty()) throw std::runtime_error("Surface creation failed: " + err);

    tracey::PresenterConfig cfg;
    cfg.width = initial_pixel_w;
    cfg.height = initial_pixel_h;
    // outputImage is already tonemapped + gamma-corrected by the resolve
    // shader, so a UNORM swapchain (no extra sRGB encode) gives the right
    // pixels on screen.
    cfg.preferredFormat = VK_FORMAT_B8G8R8A8_UNORM;
    cfg.preferredColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    cfg.preferredPresentMode = VK_PRESENT_MODE_FIFO_KHR;  // guaranteed
    cfg.desiredImageCount = 3;
    m_presenter = std::make_unique<tracey::VulkanPresenter>(*m_context, m_surface, cfg);
}

ViewportRenderer::~ViewportRenderer() {
    // Presenter waits for device-idle inside its own destructor, but the
    // validation layer's surface-bookkeeping on macOS/MoltenVK has been
    // observed to crash inside vkDestroySurfaceKHR if there's any in-flight
    // work referencing the swapchain at destroy time. A second device-idle
    // wait, *after* the presenter is gone but *before* the surface is, is
    // cheap and removes timing as a variable.
    m_presenter.reset();
    if (m_context && m_context->device() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_context->device());
    }
    if (m_surface != VK_NULL_HANDLE && m_context) {
        tracey::VulkanSurfaceFactory::destroySurface(m_context->instance(), m_surface);
        m_surface = VK_NULL_HANDLE;
    }
}

void ViewportRenderer::resize(uint32_t pixel_w, uint32_t pixel_h) {
    if (pixel_w == 0 || pixel_h == 0) return;
    if (pixel_w == m_pixel_w && pixel_h == m_pixel_h) return;
    m_pixel_w = pixel_w;
    m_pixel_h = pixel_h;
    if (m_presenter) m_presenter->resize(pixel_w, pixel_h);
}

bool ViewportRenderer::present(tracey::Image2D* source) {
    if (!source || !m_presenter) return false;
    auto* vimg = dynamic_cast<tracey::VulkanImage2D*>(source);
    if (!vimg) return false;
    return m_presenter->present(vimg, /*waitForRender=*/true);
}

bool ViewportRenderer::present_composite(tracey::Image2D* fullscreen,
                                         tracey::Image2D* inset,
                                         int32_t inset_x, int32_t inset_y,
                                         uint32_t inset_w, uint32_t inset_h) {
    if (!fullscreen || !m_presenter) return false;
    auto* fs = dynamic_cast<tracey::VulkanImage2D*>(fullscreen);
    if (!fs) return false;
    auto* ins = inset ? dynamic_cast<tracey::VulkanImage2D*>(inset) : nullptr;
    return m_presenter->presentComposite(fs, ins, inset_x, inset_y, inset_w, inset_h,
                                         /*waitForRender=*/true);
}

}  // namespace tracey_editor
