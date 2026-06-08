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

add_executable(scene_renderer
    scene_renderer/main.cpp
)

add_executable(material_program_test
    material_program_test/main.cpp
)

add_executable(sop_eval_test
    sop_eval_test/main.cpp
)

add_executable(attribute_vop_smoke
    attribute_vop_smoke/main.cpp
)

add_executable(cloners_smoke
    cloners_smoke/main.cpp
)

add_executable(instance_vop_smoke
    instance_vop_smoke/main.cpp
)

add_executable(dop_smoke
    dop_smoke/main.cpp
)

add_executable(vop_codegen_smoke
    vop_codegen_smoke/main.cpp
)

add_executable(attribute_gpu_smoke
    attribute_gpu_smoke/main.cpp
)

# Generator for shipped example .tracey project folders. The output lives
# under examples/projects/ and is checked into the repo; this target
# rebuilds those files from C++ when the schema (or the showcased graph)
# changes, so we never edit hand-authored JSON.
add_executable(example_projects
    example_projects/main.cpp
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

target_link_libraries(scene_renderer
    PRIVATE
    tracey
    glm
    Vulkan::Vulkan
    shaderc
)

target_link_libraries(material_program_test
    PRIVATE
    tracey
    glm
)

target_link_libraries(sop_eval_test
    PRIVATE
    tracey
    glm
)

target_link_libraries(attribute_vop_smoke
    PRIVATE
    tracey
    glm
)

target_link_libraries(cloners_smoke
    PRIVATE
    tracey
    glm
)

target_link_libraries(instance_vop_smoke
    PRIVATE
    tracey
    glm
)

target_link_libraries(dop_smoke
    PRIVATE
    tracey
    glm
)

target_link_libraries(vop_codegen_smoke
    PRIVATE
    tracey
    glm
    Vulkan::Vulkan
    shaderc
)

target_link_libraries(attribute_gpu_smoke
    PRIVATE
    tracey
    glm
    Vulkan::Vulkan
    shaderc
)

target_link_libraries(example_projects
    PRIVATE
    tracey
    glm
)

# Set rpath for all executables that link against Vulkan and shaderc.
# tracey itself transitively depends on libvulkan.1.dylib via the GPU device
# layer, so even the headless smoke tests need the rpath at runtime.
set_target_properties(
    device_api wavefront scene_renderer
    sop_eval_test attribute_vop_smoke cloners_smoke instance_vop_smoke dop_smoke vop_codegen_smoke attribute_gpu_smoke
    example_projects
    PROPERTIES
    MACOSX_RPATH ON
    BUILD_RPATH "/usr/local/lib"
    INSTALL_RPATH "/usr/local/lib"
)