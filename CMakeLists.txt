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
    src/core/parallel.hpp
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
    src/shading/material_program/opcodes.hpp
    src/shading/material_program/material_program.hpp
    src/shading/material_program/material_program.cpp
    src/shading/material_program/cpu_evaluator.hpp
    src/shading/material_program/cpu_evaluator.cpp

    src/geometry/attribute_class.hpp
    src/geometry/attribute.hpp
    src/geometry/attribute.cpp
    src/geometry/attribute_allocator.hpp
    src/geometry/attribute_allocator.cpp
    src/geometry/attribute_table.hpp
    src/geometry/attribute_table.cpp
    src/geometry/geometry.hpp
    src/geometry/geometry.cpp
    src/geometry/geometry_converter.hpp
    src/geometry/geometry_converter.cpp

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
    src/scene/blas_cache.hpp
    src/scene/blas_cache.cpp
    src/scene/gltf_loader.hpp
    src/scene/gltf_loader.cpp
    src/scene/camera.hpp
    src/scene/camera.cpp

    src/rendering/data_structure.hpp
    src/rendering/data_structure.cpp
    src/rendering/glsl_layout.hpp
    src/rendering/glsl_layout.cpp
    src/rendering/post_processing.hpp
    src/rendering/post_processing.cpp
    src/rendering/rasterizer.hpp
    src/rendering/rasterizer.cpp
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

    src/sops/parameter.hpp
    src/sops/parameter.cpp
    src/sops/sop_node.hpp
    src/sops/sop_node.cpp
    src/sops/sop_registry.hpp
    src/sops/sop_registry.cpp
    src/sops/sop_graph.hpp
    src/sops/sop_graph.cpp
    src/sops/cook_cache.hpp
    src/sops/cook_cache.cpp
    src/sops/serialization.hpp
    src/sops/serialization.cpp
    src/sops/register_builtins.cpp
    src/sops/nodes/primitive_sops.cpp
    src/sops/nodes/transform_sop.cpp
    src/sops/nodes/merge_sop.cpp
    src/sops/nodes/object_output_sop.cpp
    src/sops/nodes/gltf_import_sop.cpp
    src/sops/nodes/subnet_sop.cpp
    src/sops/nodes/attribute_vop_sop.cpp
    src/sops/nodes/points_grid_sop.cpp
    src/sops/nodes/copy_to_points_sop.cpp
    src/sops/nodes/instance_sop.cpp
    src/sops/nodes/instance_vop_sop.cpp
    src/sops/nodes/instance_vop_sop.hpp
    src/sops/nodes/scatter_sop.cpp
    src/sops/nodes/light_sop.cpp
    src/sops/nodes/normal_sop.cpp
    src/sops/nodes/dop_import_sop.cpp
    src/sops/nodes/dop_import_sop.hpp
    src/sops/nodes/delete_sop.cpp
    src/sops/nodes/switch_sop.cpp
    src/sops/nodes/bound_sop.cpp
    src/sops/nodes/sort_sop.cpp
    src/sops/codegen/copy_to_points_compute.hpp
    src/sops/codegen/copy_to_points_compute.cpp
    src/sops/codegen/transform_compute.hpp
    src/sops/codegen/transform_compute.cpp
    src/sops/codegen/merge_compute.hpp
    src/sops/codegen/merge_compute.cpp
    src/sops/mograph/orient_util.hpp
    src/sops/mograph/falloff.hpp
    src/sops/mograph/effector_base.hpp
    src/sops/mograph/curve_frame.hpp
    src/sops/nodes/plain_effector_sop.cpp
    src/sops/nodes/random_effector_sop.cpp
    src/sops/nodes/noise_effector_sop.cpp
    src/sops/nodes/step_effector_sop.cpp

    # ── MoGraph Phase 2: curves / sweep / text ──
    src/sops/nodes/line_sop.cpp
    src/sops/nodes/circle_sop.cpp
    src/sops/nodes/spiral_sop.cpp

    # ── VOPs (per-point attribute graph hosted inside attribute_vop SOP) ──
    src/vops/parameter.hpp
    src/vops/eval_context.hpp
    src/vops/vop_node.hpp
    src/vops/vop_node.cpp
    src/vops/vop_graph.hpp
    src/vops/vop_graph.cpp
    src/vops/typing.hpp
    src/vops/typing.cpp
    src/vops/vop_registry.hpp
    src/vops/vop_registry.cpp
    src/vops/register_builtins.hpp
    src/vops/register_builtins.cpp
    src/vops/serialization.hpp
    src/vops/serialization.cpp
    src/vops/nodes/geo_io_vops.cpp
    src/vops/nodes/math_vops.cpp
    src/vops/nodes/noise_vops.cpp
    src/vops/nodes/displacement_vops.cpp
    src/vops/codegen/glsl_emit.hpp
    src/vops/codegen/glsl_emit.cpp
    src/vops/codegen/compute_dispatch.hpp
    src/vops/codegen/compute_dispatch.cpp

    src/dops/parameter.hpp
    src/dops/sim_state.hpp
    src/dops/eval_context.hpp
    src/dops/dop_node.hpp
    src/dops/dop_node.cpp
    src/dops/dop_graph.hpp
    src/dops/dop_graph.cpp
    src/dops/dop_registry.hpp
    src/dops/dop_registry.cpp
    src/dops/register_builtins.hpp
    src/dops/register_builtins.cpp
    src/dops/serialization.hpp
    src/dops/serialization.cpp
    src/dops/nodes/pop_source.cpp
    src/dops/nodes/pop_gravity.cpp
    src/dops/nodes/pop_solver.cpp
    src/dops/nodes/pop_force.cpp
    src/dops/nodes/pop_force.hpp
    src/dops/nodes/pop_drag.cpp
    src/dops/nodes/pop_wind.cpp
    src/dops/nodes/pop_attract.cpp
    src/dops/nodes/pop_speed_limit.cpp
    src/dops/nodes/pop_kill.cpp

    src/graph/graphs/shader_graph/shader_graph.hpp
    src/graph/graphs/shader_graph/shader_graph.cpp
    src/graph/graphs/shader_graph/shader_graph_node.hpp
    src/graph/graphs/shader_graph/shader_graph_node.cpp
    src/graph/graphs/shader_graph/nodes.hpp
    src/graph/graphs/shader_graph/compiler.hpp
    src/graph/graphs/shader_graph/compiler.cpp
    src/graph/graphs/shader_graph/serialization.hpp
    src/graph/graphs/shader_graph/serialization.cpp

    src/gpu/vulkan_context.hpp
    src/gpu/vulkan_context.cpp
    src/gpu/vulkan_presenter.hpp
    src/gpu/vulkan_presenter.cpp
    src/gpu/vulkan_queue_sync.hpp
    src/gpu/vulkan_queue_sync.cpp
    src/gpu/vulkan_surface_factory.hpp
    src/gpu/vulkan_surface_factory.cpp
    src/gpu/shader_compiler.hpp
    src/gpu/shader_compiler.cpp

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

    target_link_libraries(tracey PRIVATE glm::glm Vulkan::Vulkan PkgConfig::SHADERC volk::volk)
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

    target_link_libraries(tracey PRIVATE glm::glm Vulkan::Vulkan volk::volk)
    target_link_libraries(tracey PRIVATE ${SHADERC_LIBRARY})
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

# ─── Path tracer module ──────────────────────────────────────────────────────
# Separate library with pluggable backends (wavefront compute today; Metal RT
# on macOS, Vulkan KHR RT and a native CPU fallback to come). The rasterizer
# and viewport compositor stay in the core `tracey` library.
add_library(tracey_pathtracer
    src/path_tracer/api/path_tracer.hpp
    src/path_tracer/api/path_tracer.cpp
    src/path_tracer/api/path_tracer_backend.hpp
    src/path_tracer/api/backend_registry.hpp
    src/path_tracer/api/backend_registry.cpp
    src/path_tracer/api/shader_inputs_buffer.hpp
    src/path_tracer/api/shader_inputs_buffer.cpp
    src/path_tracer/api/shader_inputs_view.hpp
    src/path_tracer/backends/cpu/cpu_path_tracer_backend.hpp
    src/path_tracer/backends/cpu/cpu_path_tracer_backend.cpp
    src/path_tracer/backends/cpu/cpu_texture.hpp
    src/path_tracer/backends/cpu/cpu_texture.cpp
)
target_link_libraries(tracey_pathtracer PUBLIC tracey)
target_link_libraries(tracey_pathtracer PRIVATE glm::glm Vulkan::Vulkan volk::volk)
target_include_directories(tracey_pathtracer
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(APPLE)
    target_sources(tracey_pathtracer PRIVATE
        src/path_tracer/backends/metal/metal_pathtracer_backend.hpp
        src/path_tracer/backends/metal/metal_pathtracer_backend.mm)
    set_source_files_properties(src/path_tracer/backends/metal/metal_pathtracer_backend.mm
        PROPERTIES COMPILE_FLAGS "-x objective-c++ -fobjc-arc")
    target_link_libraries(tracey_pathtracer PRIVATE
        "-framework Metal"
        "-framework Foundation"
        "-framework QuartzCore")
    target_compile_definitions(tracey_pathtracer PRIVATE TRACEY_PT_HAS_METAL=1)
endif()

# Native C++ editor
option(BUILD_EDITOR "Build the native C++ editor" ON)
if(BUILD_EDITOR)
    add_subdirectory(editor/native)
endif()

add_subdirectory(./examples)