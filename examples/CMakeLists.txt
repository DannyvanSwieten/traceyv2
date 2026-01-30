add_executable(blas
    bvh/blas.cpp
)

add_executable(tlas
    bvh/tlas.cpp
)

add_executable(ray_tracer
    ray_tracing/ray_tracer.cpp
)

add_executable(path_tracer
    ray_tracing/path_tracer.cpp
)

add_executable(device_api
    device_api/device_setup.cpp
)

add_executable(wavefront
    wavefront/wavefront.cpp
)

add_executable(wavefront_isf
    wavefront/wavefront_isf.cpp
)

add_executable(scene_renderer
    scene_renderer/main.cpp
)

add_executable(test_node_system
    test_node_system.cpp
)

add_executable(test_transform_and_render
    test_transform_and_render.cpp
)

target_link_libraries(blas
    PRIVATE
    tracey
    glm
)

target_link_libraries(tlas
    PRIVATE
    tracey
    glm
)

target_link_libraries(ray_tracer
    PRIVATE
    tracey
    glm
)

target_link_libraries(path_tracer
    PRIVATE
    tracey
    glm
)

target_link_libraries(device_api
    PRIVATE
    tracey
    glm
    Vulkan::Vulkan
    shaderc
)

target_link_libraries(wavefront
    PRIVATE
    tracey
    glm
    Vulkan::Vulkan
    shaderc
)

target_link_libraries(wavefront_isf
    PRIVATE
    tracey
    glm
    Vulkan::Vulkan
    shaderc
)

target_link_libraries(scene_renderer
    PRIVATE
    tracey
    glm
    Vulkan::Vulkan
    shaderc
)

target_link_libraries(test_node_system
    PRIVATE
    tracey
    glm
)

target_link_libraries(test_transform_and_render
    PRIVATE
    tracey
    glm
    Vulkan::Vulkan
    shaderc
)

# Graphics test subdirectory
add_subdirectory(graphics_test)

# Rasterizer test subdirectory
add_subdirectory(rasterizer_test)

# Set rpath for all executables that link against Vulkan and shaderc
set_target_properties(device_api wavefront wavefront_isf scene_renderer test_transform_and_render PROPERTIES
    MACOSX_RPATH ON
    BUILD_RPATH "/usr/local/lib"
    INSTALL_RPATH "/usr/local/lib"
)