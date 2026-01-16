cmake_minimum_required(VERSION 3.16)
project(Raytracer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT TARGET glm::glm-header-only)
    add_subdirectory(deps/glm)
endif()

if(NOT TARGET volk)
    add_subdirectory(deps/volk)
endif()

add_library(tracey 
    src/core/types.hpp
    src/core/ray.hpp
    src/core/hit.hpp
    src/core/blas.hpp
    src/core/blas.cpp
    src/core/tlas.hpp
    src/core/tlas.cpp
    src/core/bvh_node.hpp
    src/core/intersect.hpp
    src/core/intersect.cpp

    src/ray_tracing/trace.hpp
    src/ray_tracing/trace.cpp

    src/shading/random/sampling.hpp
    src/shading/random/sampling.cpp
    src/shading/bsdf/pbr/pbr.hpp
    src/shading/bsdf/pbr/pbr_bsdf.hpp
    src/shading/bsdf/pbr/pbr_bsdf.cpp

    src/scene/actor.hpp
    src/scene/actor.cpp
    src/scene/scene.hpp
    src/scene/scene.cpp

    src/graph/graph.hpp
    src/graph/graph.cpp
    src/graph/node.hpp
    src/graph/node.cpp
    src/graph/io_nodes/output_node.hpp
    src/graph/io_nodes/output_node.cpp
    src/graph/connection.hpp
    src/graph/data_type.hpp
    src/graph/port_info.hpp
    src/graph/template.hpp
    src/graph/prototype.hpp

    src/graph/graphs/shader_graph/shader_graph.hpp
    src/graph/graphs/shader_graph/shader_graph.cpp
    src/graph/graphs/shader_graph/shader_graph_node.hpp
    src/graph/graphs/shader_graph/shader_graph_node.cpp

    src/gpu/vulkan_context.hpp
    src/gpu/vulkan_context.cpp
    # src/gpu/shader_compiler.hpp
    # src/gpu/shader_compiler.cpp

    src/device/device.hpp
    src/device/device.cpp

    src/device/gpu/vulkan_compute_device.hpp
    src/device/gpu/vulkan_compute_device.cpp
    src/device/gpu/vulkan_buffer.hpp
    src/device/gpu/vulkan_buffer.cpp
    src/device/gpu/vulkan_image_2d.hpp
    src/device/gpu/vulkan_image_2d.cpp
    src/device/gpu/vulkan_compute_bottom_level_accelerations_structure.hpp
    src/device/gpu/vulkan_compute_bottom_level_accelerations_structure.cpp
    src/device/gpu/vulkan_compute_top_level_acceleration_structure.hpp
    src/device/gpu/vulkan_compute_top_level_acceleration_structure.cpp

    src/device/cpu/cpu_compute_device.hpp
    src/device/cpu/cpu_compute_device.cpp
    src/device/cpu/cpu_buffer.hpp
    src/device/cpu/cpu_buffer.cpp
    src/device/buffer.hpp
    src/device/image_2d.hpp
    src/device/cpu/cpu_image_2d.hpp
    src/device/cpu/cpu_image_2d.cpp
    src/device/cpu/cpu_bottom_level_acceleration_structure.hpp
    src/device/cpu/cpu_bottom_level_acceleration_structure.cpp
    src/device/cpu/cpu_top_level_acceleration_structure.hpp
    src/device/cpu/cpu_top_level_acceleration_structure.cpp

    src/ray_tracing/ray_tracing_pipeline/ray_tracing_pipeline.hpp
    src/ray_tracing/ray_tracing_pipeline/ray_tracing_pipeline_layout.hpp
    src/ray_tracing/ray_tracing_pipeline/ray_tracing_pipeline_layout.cpp

    src/ray_tracing/ray_tracing_pipeline/shader_binding_table.hpp
    src/ray_tracing/ray_tracing_pipeline/data_structure.hpp
    src/ray_tracing/ray_tracing_pipeline/data_structure.cpp

    src/ray_tracing/ray_tracing_pipeline/cpu/cpu_shader_binding_table.hpp
    src/ray_tracing/ray_tracing_pipeline/cpu/cpu_shader_binding_table.cpp
    src/ray_tracing/ray_tracing_pipeline/cpu/cpu_ray_tracing_pipeline.hpp
    src/ray_tracing/ray_tracing_pipeline/cpu/cpu_ray_tracing_pipeline.cpp
    src/ray_tracing/ray_tracing_pipeline/descriptor_set.hpp
    src/ray_tracing/ray_tracing_pipeline/cpu/cpu_descriptor_set.hpp
    src/ray_tracing/ray_tracing_pipeline/cpu/cpu_descriptor_set.cpp
    src/ray_tracing/ray_tracing_pipeline/cpu/cpu_pipeline_compiler.hpp
    src/ray_tracing/ray_tracing_pipeline/cpu/cpu_pipeline_compiler.cpp
    src/ray_tracing/ray_tracing_pipeline/cpu/trace_rays_ext.hpp
    src/ray_tracing/ray_tracing_pipeline/cpu/trace_rays_ext.cpp

    src/ray_tracing/ray_tracing_pipeline/gpu/vulkan_compute_raytracing_pipeline.hpp
    src/ray_tracing/ray_tracing_pipeline/gpu/vulkan_compute_raytracing_pipeline.cpp
    src/ray_tracing/ray_tracing_pipeline/gpu/vulkan_compute_raytracing_descriptor_set.hpp
    src/ray_tracing/ray_tracing_pipeline/gpu/vulkan_compute_raytracing_descriptor_set.cpp
    src/ray_tracing/ray_tracing_pipeline/gpu/vulkan_compute_pipeline_compiler.hpp
    src/ray_tracing/ray_tracing_pipeline/gpu/vulkan_compute_pipeline_compiler.cpp
    src/ray_tracing/ray_tracing_pipeline/gpu/wavefront/vulkan_wavefront_pipeline.hpp
    src/ray_tracing/ray_tracing_pipeline/gpu/wavefront/vulkan_wavefront_pipeline.cpp

    src/ray_tracing/ray_tracing_command_buffer/ray_tracing_command_buffer.hpp
    src/ray_tracing/ray_tracing_command_buffer/cpu/cpu_ray_tracing_command_buffer.hpp
    src/ray_tracing/ray_tracing_command_buffer/cpu/cpu_ray_tracing_command_buffer.cpp
    src/ray_tracing/ray_tracing_command_buffer/gpu/vulkan_compute_ray_tracing_command_buffer.hpp
    src/ray_tracing/ray_tracing_command_buffer/gpu/vulkan_compute_ray_tracing_command_buffer.cpp

    src/ray_tracing/shader_module/shader_module.hpp
    src/ray_tracing/shader_module/cpu/cpu_shader_module.hpp
    src/ray_tracing/shader_module/cpu/cpu_shader_module.cpp

    src/ray_tracing/isf/isf_parser.hpp
    src/ray_tracing/isf/isf_parser.cpp
    src/ray_tracing/isf/isf_pipeline_builder.hpp
    src/ray_tracing/isf/isf_pipeline_builder.cpp
    src/ray_tracing/isf/shader_inputs_buffer.hpp
    src/ray_tracing/isf/shader_inputs_buffer.cpp
)

# Define DEBUG for Debug builds, NDEBUG for non-Debug
add_compile_definitions(
    $<$<CONFIG:Debug>:DEBUG>
    $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
)

if(MSVC)
    target_compile_options(tracey PRIVATE /W4 /permissive-)
else()
    target_compile_options(tracey PRIVATE -Wall -Wextra -Wpedantic)
endif()

find_package(Vulkan REQUIRED)

find_package(PkgConfig REQUIRED)

pkg_check_modules(SHADERC REQUIRED IMPORTED_TARGET shaderc)

# Optional: shaderc_combined (not commonly available on macOS)
pkg_check_modules(SHADERC_COMBINED QUIET IMPORTED_TARGET shaderc_combined)
target_include_directories(tracey PRIVATE
    ${SHADERC_INCLUDE_DIRS}
)

# IMPORTANT: static deps (glslang + spirv-tools) are only in the --static output
execute_process(
    COMMAND pkg-config --libs --static shaderc
    OUTPUT_VARIABLE SHADERC_STATIC_LIBS
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

target_link_libraries(tracey PRIVATE glm::glm Vulkan::Vulkan PkgConfig::SHADERC volk::volk)
target_include_directories(tracey
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(tracey PRIVATE
    shaderc
    shaderc_util
    glslang
    SPIRV-Tools
    SPIRV-Tools-opt
)
add_subdirectory(./examples) 
# After you define your executable target
set_target_properties(device_api PROPERTIES
    MACOSX_RPATH ON
    BUILD_RPATH "/usr/local/lib"
    INSTALL_RPATH "/usr/local/lib"
)