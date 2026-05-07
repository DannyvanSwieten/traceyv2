#pragma once

#include <vulkan/vulkan.h>
#include <string>

namespace tracey {

/// Factory for creating Vulkan surfaces from native window handles
/// Handles platform-specific surface creation (Metal/Win32/X11/Wayland)
class VulkanSurfaceFactory {
public:
    /// Create Vulkan surface from native window handle
    ///
    /// @param instance Vulkan instance (must have surface extensions enabled)
    /// @param nativeHandle Platform-specific handle:
    ///   - macOS: CAMetalLayer* (from NSView's layer)
    ///   - Windows: HWND
    ///   - Linux X11: xcb_window_t or Window
    ///   - Linux Wayland: wl_surface*
    /// @param nativeDisplay Platform-specific display handle:
    ///   - macOS: nullptr (not needed for Metal)
    ///   - Windows: HINSTANCE (from GetModuleHandle)
    ///   - Linux X11: Display* or xcb_connection_t*
    ///   - Linux Wayland: wl_display*
    /// @param surface Output VkSurfaceKHR handle
    /// @return Empty string on success, error message on failure
    static std::string createSurface(
        VkInstance instance,
        void* nativeHandle,
        void* nativeDisplay,
        VkSurfaceKHR* surface
    );

    /// Destroy Vulkan surface
    ///
    /// @param instance Vulkan instance that created the surface
    /// @param surface Surface to destroy
    static void destroySurface(VkInstance instance, VkSurfaceKHR surface);

private:
    // Platform-specific surface creation
#ifdef __APPLE__
    static std::string createMetalSurface(
        VkInstance instance,
        void* metalLayer,
        VkSurfaceKHR* surface
    );
#endif

#ifdef _WIN32
    static std::string createWin32Surface(
        VkInstance instance,
        void* hwnd,
        void* hinstance,
        VkSurfaceKHR* surface
    );
#endif

#ifdef __linux__
    static std::string createXlibSurface(
        VkInstance instance,
        void* display,
        void* window,
        VkSurfaceKHR* surface
    );

    static std::string createWaylandSurface(
        VkInstance instance,
        void* display,
        void* surface_handle,
        VkSurfaceKHR* surface
    );
#endif
};

} // namespace tracey
