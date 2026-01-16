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