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
    src/scene/scene_object.hpp
    src/scene/scene_object.cpp
    src/scene/material_instance.hpp
    src/scene/material_instance.cpp
    src/scene/scene_instance.hpp
    src/scene/scene_instance.cpp
    src/scene/scene_loader.hpp
    src/scene/scene_loader.cpp
    src/scene/scene_compiler.hpp
    src/scene/scene_compiler.cpp
    src/scene/gltf_loader.hpp
    src/scene/gltf_loader.cpp
    src/scene/camera.hpp
    src/scene/camera.cpp
    src/scene/attribute.hpp
    src/scene/attribute.cpp
    src/scene/geometry.hpp
    src/scene/geometry.cpp

    # Procedural node system
    src/scene/procedural/parameter_value.hpp
    src/scene/procedural/evaluation_context.hpp
    src/scene/procedural/parameter.hpp
    src/scene/procedural/parameter.cpp
    src/scene/procedural/node.hpp
    src/scene/procedural/node.cpp
    src/scene/procedural/node_graph.hpp
    src/scene/procedural/node_graph.cpp
    src/scene/procedural/nodes/actor_node.hpp
    src/scene/procedural/nodes/actor_node.cpp
    src/scene/procedural/nodes/primitive_node.hpp
    src/scene/procedural/nodes/primitive_node.cpp
    src/scene/procedural/nodes/merge_node.hpp
    src/scene/procedural/nodes/merge_node.cpp
    src/scene/procedural/nodes/transform_geo_node.hpp
    src/scene/procedural/nodes/transform_geo_node.cpp

    src/rendering/path_tracer.hpp
    src/rendering/path_tracer.cpp
    src/rendering/rasterizer.hpp
    src/rendering/rasterizer.cpp
    src/rendering/post_processing.hpp
    src/rendering/post_processing.cpp
    src/rendering/graphics_pipeline.hpp
    src/rendering/graphics_pipeline_layout.hpp
    src/rendering/graphics_pipeline_layout.cpp
    src/rendering/graphics_command_buffer.hpp
    src/rendering/gpu/vulkan_graphics_pipeline.hpp
    src/rendering/gpu/vulkan_graphics_pipeline.cpp
    src/rendering/gpu/vulkan_graphics_command_buffer.hpp
    src/rendering/gpu/vulkan_graphics_command_buffer.cpp

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
    src/gpu/vulkan_presenter.hpp
    src/gpu/vulkan_presenter.cpp
    src/gpu/vulkan_surface_factory.hpp
    src/gpu/vulkan_surface_factory.cpp
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
    target_compile_definitions(tracey PRIVATE NOMINMAX)
else()
    target_compile_options(tracey PRIVATE -Wall -Wextra -Wpedantic)
endif()

find_package(Vulkan REQUIRED)

# Try to find PkgConfig (available on Unix/Linux/macOS, not typically on Windows)
find_package(PkgConfig QUIET)

if(PKG_CONFIG_FOUND)
    # Unix/Linux/macOS: Use pkg-config to find shaderc
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

    target_link_libraries(tracey PRIVATE glm::glm Vulkan::Vulkan PkgConfig::SHADERC)
    target_link_libraries(tracey PUBLIC volk::volk)
else()
    # Windows: Manually find shaderc libraries
    message(STATUS "PkgConfig not found, using manual library search for Windows")

    # Find shaderc include directory (usually in Vulkan SDK)
    find_path(SHADERC_INCLUDE_DIR
        NAMES shaderc/shaderc.hpp
        PATHS
            ${Vulkan_INCLUDE_DIRS}
            $ENV{VULKAN_SDK}/Include
            $ENV{VK_SDK_PATH}/Include
        PATH_SUFFIXES shaderc
    )

    # Find shaderc libraries
    find_library(SHADERC_LIBRARY
        NAMES shaderc_combined shaderc_shared shaderc
        PATHS
            ${Vulkan_LIBRARY_DIRS}
            $ENV{VULKAN_SDK}/Lib
            $ENV{VK_SDK_PATH}/Lib
    )

    if(NOT SHADERC_INCLUDE_DIR OR NOT SHADERC_LIBRARY)
        message(FATAL_ERROR "Could not find shaderc. Please install the Vulkan SDK with shaderc support.")
    endif()

    message(STATUS "Found shaderc: ${SHADERC_LIBRARY}")
    message(STATUS "Found shaderc include: ${SHADERC_INCLUDE_DIR}")

    target_include_directories(tracey PRIVATE
        ${SHADERC_INCLUDE_DIR}
    )

    target_link_libraries(tracey PRIVATE glm::glm Vulkan::Vulkan ${SHADERC_LIBRARY})
    target_link_libraries(tracey PUBLIC volk::volk)
endif()

target_include_directories(tracey
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/tinygltf
)

# Additional libraries needed when using pkg-config on Unix/Linux/macOS
if(PKG_CONFIG_FOUND)
    target_link_libraries(tracey PRIVATE
        shaderc
        shaderc_util
        glslang
        SPIRV-Tools
        SPIRV-Tools-opt
    )
endif()

# Optional: Build C API for FFI (Rust/Tauri editor)
option(BUILD_C_API "Build C API shared library for FFI" ON)
if(BUILD_C_API)
    add_subdirectory(src/c_api)
endif()

add_subdirectory(./examples)