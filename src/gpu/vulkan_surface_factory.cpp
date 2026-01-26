#include "vulkan_surface_factory.hpp"

#ifdef __APPLE__
#include <vulkan/vulkan_metal.h>
#endif

#ifdef _WIN32
#include <vulkan/vulkan_win32.h>
#include <windows.h>
#endif

#ifdef __linux__
#include <vulkan/vulkan_xlib.h>
#include <vulkan/vulkan_wayland.h>
#endif

namespace tracey {

std::string VulkanSurfaceFactory::createSurface(
    VkInstance instance,
    void* nativeHandle,
    void* nativeDisplay,
    VkSurfaceKHR* surface
) {
    if (instance == VK_NULL_HANDLE) {
        return "Vulkan instance is null";
    }

    if (nativeHandle == nullptr) {
        return "Native window handle is null";
    }

    if (surface == nullptr) {
        return "Surface output pointer is null";
    }

#ifdef __APPLE__
    // macOS: Use Metal surface (nativeDisplay not needed)
    return createMetalSurface(instance, nativeHandle, surface);
#elif defined(_WIN32)
    // Windows: Use Win32 surface
    if (nativeDisplay == nullptr) {
        return "HINSTANCE (nativeDisplay) is null";
    }
    return createWin32Surface(instance, nativeHandle, nativeDisplay, surface);
#elif defined(__linux__)
    // Linux: Try Wayland first (modern), then X11 (legacy)
    // Check if we have Wayland handles
    if (nativeDisplay != nullptr) {
        // Try Wayland
        std::string result = createWaylandSurface(instance, nativeDisplay, nativeHandle, surface);
        if (result.empty()) {
            return result; // Success
        }
        // Wayland failed, try X11
        return createXlibSurface(instance, nativeDisplay, nativeHandle, surface);
    } else {
        return "Native display handle is null (X11 Display* or wl_display* required)";
    }
#else
    return "Unsupported platform for Vulkan surface creation";
#endif
}

void VulkanSurfaceFactory::destroySurface(VkInstance instance, VkSurfaceKHR surface) {
    if (instance != VK_NULL_HANDLE && surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance, surface, nullptr);
    }
}

// Platform-specific implementations

#ifdef __APPLE__
std::string VulkanSurfaceFactory::createMetalSurface(
    VkInstance instance,
    void* metalLayer,
    VkSurfaceKHR* surface
) {
    VkMetalSurfaceCreateInfoEXT createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
    createInfo.pNext = nullptr;
    createInfo.flags = 0;
    createInfo.pLayer = metalLayer; // CAMetalLayer*

    VkResult result = vkCreateMetalSurfaceEXT(instance, &createInfo, nullptr, surface);

    if (result != VK_SUCCESS) {
        switch (result) {
            case VK_ERROR_OUT_OF_HOST_MEMORY:
                return "Failed to create Metal surface: out of host memory";
            case VK_ERROR_OUT_OF_DEVICE_MEMORY:
                return "Failed to create Metal surface: out of device memory";
            case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
                return "Failed to create Metal surface: native window already in use";
            default:
                return "Failed to create Metal surface: error code " + std::to_string(result);
        }
    }

    return ""; // Success
}
#endif

#ifdef _WIN32
std::string VulkanSurfaceFactory::createWin32Surface(
    VkInstance instance,
    void* hwnd,
    void* hinstance,
    VkSurfaceKHR* surface
) {
    VkWin32SurfaceCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    createInfo.pNext = nullptr;
    createInfo.flags = 0;
    createInfo.hinstance = static_cast<HINSTANCE>(hinstance);
    createInfo.hwnd = static_cast<HWND>(hwnd);

    VkResult result = vkCreateWin32SurfaceKHR(instance, &createInfo, nullptr, surface);

    if (result != VK_SUCCESS) {
        switch (result) {
            case VK_ERROR_OUT_OF_HOST_MEMORY:
                return "Failed to create Win32 surface: out of host memory";
            case VK_ERROR_OUT_OF_DEVICE_MEMORY:
                return "Failed to create Win32 surface: out of device memory";
            case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
                return "Failed to create Win32 surface: native window already in use";
            default:
                return "Failed to create Win32 surface: error code " + std::to_string(result);
        }
    }

    return ""; // Success
}
#endif

#ifdef __linux__
std::string VulkanSurfaceFactory::createXlibSurface(
    VkInstance instance,
    void* display,
    void* window,
    VkSurfaceKHR* surface
) {
    VkXlibSurfaceCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    createInfo.pNext = nullptr;
    createInfo.flags = 0;
    createInfo.dpy = static_cast<Display*>(display);
    createInfo.window = reinterpret_cast<Window>(window);

    VkResult result = vkCreateXlibSurfaceKHR(instance, &createInfo, nullptr, surface);

    if (result != VK_SUCCESS) {
        switch (result) {
            case VK_ERROR_OUT_OF_HOST_MEMORY:
                return "Failed to create Xlib surface: out of host memory";
            case VK_ERROR_OUT_OF_DEVICE_MEMORY:
                return "Failed to create Xlib surface: out of device memory";
            case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
                return "Failed to create Xlib surface: native window already in use";
            default:
                return "Failed to create Xlib surface: error code " + std::to_string(result);
        }
    }

    return ""; // Success
}

std::string VulkanSurfaceFactory::createWaylandSurface(
    VkInstance instance,
    void* display,
    void* surface_handle,
    VkSurfaceKHR* surface
) {
    VkWaylandSurfaceCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    createInfo.pNext = nullptr;
    createInfo.flags = 0;
    createInfo.display = static_cast<wl_display*>(display);
    createInfo.surface = static_cast<wl_surface*>(surface_handle);

    VkResult result = vkCreateWaylandSurfaceKHR(instance, &createInfo, nullptr, surface);

    if (result != VK_SUCCESS) {
        switch (result) {
            case VK_ERROR_OUT_OF_HOST_MEMORY:
                return "Failed to create Wayland surface: out of host memory";
            case VK_ERROR_OUT_OF_DEVICE_MEMORY:
                return "Failed to create Wayland surface: out of device memory";
            case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
                return "Failed to create Wayland surface: native window already in use";
            default:
                return "Failed to create Wayland surface: error code " + std::to_string(result);
        }
    }

    return ""; // Success
}
#endif

} // namespace tracey
