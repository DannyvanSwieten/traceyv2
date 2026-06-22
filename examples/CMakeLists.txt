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

add_executable(mograph_curves_smoke
    mograph_curves_smoke/main.cpp
)

add_executable(dop_geometry_source_smoke
    dop_geometry_source_smoke/main.cpp
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

add_executable(scene_export_smoke
    scene_export_smoke/main.cpp
)

add_executable(exr_roundtrip_smoke
    exr_roundtrip_smoke/main.cpp
)

add_executable(denoiser_smoke
    denoiser_smoke/main.cpp
)

add_executable(exr_inspect
    exr_inspect/main.cpp
)

# Generator for shipped example .tracey project folders. The output lives
# under examples/projects/ and is checked into the repo; this target
# rebuilds those files from C++ when the schema (or the showcased graph)
# changes, so we never edit hand-authored JSON.
add_executable(example_projects
    example_projects/main.cpp
)

add_executable(materialx_smoke
    materialx_smoke/main.cpp
)
target_link_libraries(materialx_smoke
    PRIVATE
    tracey
    glm
)

# U0 OpenUSD smoke — only when USD is vendored. Links the pxr imported targets
# directly (USD isn't linked into the core `tracey` lib). usd/usdGeom pull in
# their transitive deps (sdf/tf/vt/gf/arch/…).
if(TRACEY_WITH_USD)
    add_executable(usd_smoke usd_smoke/main.cpp)
    target_link_libraries(usd_smoke PRIVATE usd usdGeom)
endif()

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

target_link_libraries(scene_renderer
    PRIVATE
    tracey
    tracey_pathtracer
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

target_link_libraries(mograph_curves_smoke
    PRIVATE
    tracey
    glm
)

target_link_libraries(instance_vop_smoke
    PRIVATE
    tracey
    glm
)

target_link_libraries(dop_geometry_source_smoke
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

target_link_libraries(scene_export_smoke
    PRIVATE
    tracey
    glm
)

target_link_libraries(exr_roundtrip_smoke
    PRIVATE
    tracey
    glm
)

target_link_libraries(denoiser_smoke
    PRIVATE
    tracey
    glm
)

target_link_libraries(exr_inspect
    PRIVATE
    tracey
    glm
)

target_link_libraries(example_projects
    PRIVATE
    tracey
    glm
)

# Backend parity harness: renders one scene with two PathTracer backends
# and gates on PSNR. The acceptance test for new backends (Metal RT, CPU)
# against the wavefront reference.
add_executable(pt_backend_compare pt_backend_compare/main.cpp)
target_link_libraries(pt_backend_compare
    PRIVATE
    tracey
    tracey_pathtracer
    glm
    Vulkan::Vulkan
    shaderc
)
# Let the harness load .usd* via UsdLoader when USD is vendored.
if(TRACEY_WITH_USD)
    target_link_libraries(pt_backend_compare PRIVATE tracey_usd)
    target_compile_definitions(pt_backend_compare PRIVATE TRACEY_HAS_USD=1)
endif()

# Metal ↔ Vulkan interop spike for the Metal RT path tracer backend
# (VK_EXT_metal_objects MTLTexture export). macOS only.
if(APPLE)
    add_executable(metal_interop_spike metal_interop_spike/main.mm)
    set_source_files_properties(metal_interop_spike/main.mm
        PROPERTIES COMPILE_FLAGS "-x objective-c++ -fobjc-arc")
    target_link_libraries(metal_interop_spike
        PRIVATE
        tracey
        glm
        volk::volk
        Vulkan::Vulkan
        "-framework Metal"
        "-framework Foundation"
    )
endif()

# Set rpath for all executables that link against Vulkan and shaderc.
# tracey itself transitively depends on libvulkan.1.dylib via the GPU device
# layer, so even the headless smoke tests need the rpath at runtime.
set(TRACEY_EXAMPLE_RPATH_TARGETS
    scene_renderer pt_backend_compare
    sop_eval_test attribute_vop_smoke cloners_smoke mograph_curves_smoke instance_vop_smoke dop_smoke dop_geometry_source_smoke vop_codegen_smoke attribute_gpu_smoke scene_export_smoke exr_roundtrip_smoke denoiser_smoke exr_inspect materialx_smoke
    example_projects)
if(APPLE)
    list(APPEND TRACEY_EXAMPLE_RPATH_TARGETS metal_interop_spike)
endif()
# usd_smoke only exists when USD is vendored; it needs the USD dylib rpath.
if(TRACEY_WITH_USD)
    list(APPEND TRACEY_EXAMPLE_RPATH_TARGETS usd_smoke)
endif()
# The OIDN dylibs live in deps/oidn/lib; add it so the main libOpenImageDenoise
# (and the sibling core/device libs it loads via @loader_path) resolve.
set(TRACEY_EXAMPLE_RPATH "/usr/local/lib")
if(TRACEY_WITH_OIDN)
    list(APPEND TRACEY_EXAMPLE_RPATH "${TRACEY_OIDN_LIBDIR}")
endif()
# USD ships shared libs (libusd_*.dylib + libtbb.dylib) in deps/usd/lib.
if(TRACEY_WITH_USD)
    list(APPEND TRACEY_EXAMPLE_RPATH "${TRACEY_USD_LIBDIR}")
endif()
set_target_properties(
    ${TRACEY_EXAMPLE_RPATH_TARGETS}
    PROPERTIES
    MACOSX_RPATH ON
    BUILD_RPATH "${TRACEY_EXAMPLE_RPATH}"
    INSTALL_RPATH "${TRACEY_EXAMPLE_RPATH}"
)