cmake_minimum_required(VERSION 3.16)
project(Raytracer LANGUAGES C CXX)

# Default to an optimized build. Without this, a bare `cmake -S . -B build`
# (or an IDE that doesn't pass one) configures with an EMPTY/Debug build type
# = -O0, which makes the path tracer ~13x slower. Single-config generators
# only; multi-config (Xcode/VS) pick per-build. Override with -DCMAKE_BUILD_TYPE=Debug.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (Debug/Release/RelWithDebInfo)" FORCE)
    message(STATUS "No build type set; defaulting to Release (-O3 -DNDEBUG)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Native CPU tuning for optimized configs only (Debug stays portable + fast to
# build). Applies to every target below, so it must precede the add_library/
# add_executable calls. NOTE: ThinLTO was evaluated here and removed — it added
# per-launch relink time for no measurable runtime gain, because the hot
# intersect/shading code is already header-`inline` (nothing left to inline
# cross-TU) and Apple clang's arm64 default already targets the host CPU.
if(APPLE)
    add_compile_options(
        $<$<CONFIG:Release>:-mcpu=native> $<$<CONFIG:RelWithDebInfo>:-mcpu=native>)
endif()

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
    src/scene/skeleton.hpp
    src/scene/skeleton.cpp
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
    src/scene/materialx_loader.hpp
    src/scene/materialx_loader.cpp
    src/scene/scene_exporter.hpp
    src/scene/scene_exporter.cpp
    src/scene/camera.hpp
    src/scene/camera.cpp

    src/io/exr_writer.hpp
    src/io/exr_writer.cpp
    src/io/png_writer.hpp
    src/io/png_writer.cpp
    src/io/denoiser.hpp
    src/io/denoiser.cpp
    deps/tinyexr/miniz.c

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
    src/sops/nodes/resample_sop.cpp
    src/sops/nodes/sweep_sop.cpp
    src/sops/nodes/mo_text_sop.cpp

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
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/earcut
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/tinyexr
)

# ─── Intel Open Image Denoise (optional, R2) ─────────────────────────────────
# Heavy prebuilt dependency (ships its own dylibs + a CMake config package).
# Defaults ON when the prebuilt is vendored under deps/oidn, OFF otherwise so
# the core build never hard-depends on it. The denoiser code in src/io/
# denoiser.cpp guards on TRACEY_HAS_OIDN and stubs out cleanly when absent.
set(TRACEY_OIDN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/oidn")
set(TRACEY_OIDN_LIBDIR "${TRACEY_OIDN_DIR}/lib" CACHE INTERNAL "OIDN dylib dir for rpath")
if(EXISTS "${TRACEY_OIDN_DIR}/include/OpenImageDenoise/oidn.h")
    set(_tracey_oidn_default ON)
else()
    set(_tracey_oidn_default OFF)
endif()
option(TRACEY_WITH_OIDN "Build with the Intel Open Image Denoise denoiser" ${_tracey_oidn_default})
if(TRACEY_WITH_OIDN)
    # Search the vendored prefix (deps/oidn); config-mode locates the versioned
    # lib/cmake/OpenImageDenoise-*/ dir, so the bootstrap script's pinned
    # version isn't duplicated here.
    find_package(OpenImageDenoise 2 CONFIG REQUIRED
        PATHS "${TRACEY_OIDN_DIR}" NO_DEFAULT_PATH)
    target_link_libraries(tracey PUBLIC OpenImageDenoise)
    target_compile_definitions(tracey PUBLIC TRACEY_HAS_OIDN=1)
    message(STATUS "OIDN denoiser: enabled (${TRACEY_OIDN_LIBDIR})")
endif()

# ─── MaterialX (optional, R3f) ───────────────────────────────────────────────
# Built from source (Core+Format+stdlib) by scripts/bootstrap_deps.sh into
# deps/materialx. We use only the document/XML API to parse .mtlx and compile
# node graphs onto our own material VM — not MaterialX's shader codegen.
# Defaults ON when vendored, OFF otherwise; src/scene/materialx_loader.cpp
# guards on TRACEY_HAS_MATERIALX and stubs out when absent.
set(TRACEY_MATERIALX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/materialx")
if(EXISTS "${TRACEY_MATERIALX_DIR}/include/MaterialXCore/Document.h")
    set(_tracey_mtlx_default ON)
else()
    set(_tracey_mtlx_default OFF)
endif()
option(TRACEY_WITH_MATERIALX "Build with MaterialX (.mtlx) material import" ${_tracey_mtlx_default})
if(TRACEY_WITH_MATERIALX)
    find_package(MaterialX CONFIG REQUIRED
        PATHS "${TRACEY_MATERIALX_DIR}" NO_DEFAULT_PATH)
    target_link_libraries(tracey PUBLIC MaterialXCore MaterialXFormat)
    target_compile_definitions(tracey PUBLIC
        TRACEY_HAS_MATERIALX=1
        # Runtime location of the standard data libraries (stdlib/pbrlib/bxdf),
        # needed to resolve standard_surface nodedefs + input defaults.
        TRACEY_MATERIALX_LIBRARIES_DIR="${TRACEY_MATERIALX_DIR}/libraries")
    message(STATUS "MaterialX import: enabled (${TRACEY_MATERIALX_DIR})")
endif()

# ─── OpenUSD (optional, U0 — gate for Track 2) ───────────────────────────────
# USD core (tf/sdf/usd/usdGeom) built from source by scripts/bootstrap_deps.sh
# into deps/usd. The gate for USD import/export + a future Hydra delegate.
# Defaults ON when vendored, OFF otherwise; USD-gated code (src/scene/usd_*)
# guards on TRACEY_HAS_USD and stubs out when absent, so the core build never
# hard-depends on this very heavy dependency.
set(TRACEY_USD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/usd")
if(EXISTS "${TRACEY_USD_DIR}/include/pxr/pxr.h")
    set(_tracey_usd_default ON)
else()
    set(_tracey_usd_default OFF)
endif()
option(TRACEY_WITH_USD "Build with OpenUSD (USD scene import/export)" ${_tracey_usd_default})
if(TRACEY_WITH_USD)
    # Make the pxr imported targets (usd / usdGeom / sdf / tf / …) available to
    # subdirectories. We deliberately do NOT link USD into the core `tracey`
    # lib: USD ships *shared* libs, so that would force a USD rpath onto every
    # tracey-linking binary. Instead, only the targets that need USD link it
    # (currently usd_smoke; a future tracey_usd module / the editor when the
    # UsdLoader lands). The flag + TRACEY_USD_LIBDIR are read in examples/.
    find_package(pxr CONFIG REQUIRED PATHS "${TRACEY_USD_DIR}" NO_DEFAULT_PATH)
    set(TRACEY_USD_LIBDIR "${TRACEY_USD_DIR}/lib" CACHE INTERNAL "USD dylib dir for rpath")

    # tracey_usd: the only library that links OpenUSD. The public header
    # (usd_loader.hpp) is USD-free (returns a tracey::Scene), so consumers
    # include it freely; only this .cpp pulls pxr. Layered tracey <- tracey_usd.
    add_library(tracey_usd
        src/scene/usd_loader.hpp
        src/scene/usd_loader.cpp
        src/sops/nodes/usd_import_sop.cpp)
    target_link_libraries(tracey_usd PUBLIC tracey)
    target_link_libraries(tracey_usd PRIVATE usd usdGeom usdShade usdLux glm::glm)
    target_compile_definitions(tracey_usd PRIVATE TRACEY_HAS_USD=1)
    message(STATUS "OpenUSD: enabled (${TRACEY_USD_DIR})")
endif()

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

# ── Tracey C embedding API ──────────────────────────────────────────────────
# Pure-C ABI boundary for embedding the path tracer in third-party apps.
# Links the engine + path tracer; the public header (tracey_c.h) exposes only
# opaque handles + POD structs — no STL/glm/USD/MaterialX in the surface. This
# is the deliberate embedding boundary the renderer commits to: heavy optional
# modules stay behind it, never in the ABI.
add_library(tracey_c
    src/c_api/tracey_c.h
    src/c_api/tracey_c.cpp
)
target_link_libraries(tracey_c PUBLIC tracey_pathtracer)
target_link_libraries(tracey_c PRIVATE glm::glm)
target_include_directories(tracey_c
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/c_api
)

# Native C++ editor
option(BUILD_EDITOR "Build the native C++ editor" ON)
if(BUILD_EDITOR)
    add_subdirectory(editor/native)
endif()

add_subdirectory(./examples)