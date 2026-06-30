cmake_minimum_required(VERSION 3.16)

set(TRACEY_EDITOR_FRONTEND_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../frontend")
# During Phase 1 the frontend still lives at editor/ (root), not editor/frontend/.
# We probe both — frontend/ takes precedence once Phase 5 reorganizes the tree.
if(NOT EXISTS "${TRACEY_EDITOR_FRONTEND_DIR}/package.json")
    set(TRACEY_EDITOR_FRONTEND_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()

# nlohmann::json single-header is bundled inside deps/tinygltf — reuse it
# instead of pulling another copy.
add_library(tracey_editor_json INTERFACE)
target_include_directories(tracey_editor_json INTERFACE
    "${CMAKE_SOURCE_DIR}/deps/tinygltf")

# ─── Editor executable ──────────────────────────────────────────────────────
set(TRACEY_EDITOR_SRCS
    main.cpp
    editor_server.cpp
    editor_server_cmds_scene.cpp
    editor_server_cmds_render.cpp
    editor_server_cmds_material.cpp
    editor_server_cmds_graph.cpp
    editor_server_cmds_timeline.cpp
    editor_server_cmds_io.cpp
    editor_server_cmds_shot.cpp
    render_engine.cpp
    scene_state.cpp
    viewport_renderer.cpp)

if(APPLE)
    add_executable(tracey_editor
        ${TRACEY_EDITOR_SRCS}
        platform/platform_macos.mm
        video_exporter.mm)

    set_source_files_properties(platform/platform_macos.mm
        PROPERTIES COMPILE_FLAGS "-x objective-c++")
    # video_exporter.mm holds long-lived Objective-C pointers in a C++ struct
    # (the AVAssetWriter / input / adaptor across many appendFrame calls). MRR
    # would let the autoreleased convenience-constructor objects deallocate
    # before the next frame; ARC retains via the implicit __strong members so
    # they survive the export run.
    set_source_files_properties(video_exporter.mm
        PROPERTIES COMPILE_FLAGS "-x objective-c++ -fobjc-arc")

    target_link_libraries(tracey_editor PRIVATE
        tracey
        tracey_pathtracer
        tracey_editor_json
        glm::glm
        volk::volk
        Vulkan::Vulkan
        "-framework Cocoa"
        "-framework WebKit"
        "-framework QuartzCore"
        "-framework Metal"
        "-framework CoreVideo"
        "-framework CoreMedia"
        "-framework AVFoundation"
    )

    # OpenUSD import: the editor is the (only) app that links tracey_usd. The
    # core renderer + C API stay USD-free; this is the one consumer that pulls
    # the heavy dep in. TRACEY_HAS_USD gates the editor's USD code paths
    # (peek_usd IPC, usd_import SOP registration, USD material re-resolution).
    if(TRACEY_WITH_USD)
        target_link_libraries(tracey_editor PRIVATE tracey_usd)
        target_compile_definitions(tracey_editor PRIVATE TRACEY_HAS_USD=1)
    endif()

    set(TRACEY_EDITOR_RPATH "/usr/local/lib")
    if(TRACEY_WITH_OIDN)
        list(APPEND TRACEY_EDITOR_RPATH "${TRACEY_OIDN_LIBDIR}")
    endif()
    # USD ships shared libs (libusd_*.dylib + libtbb.dylib) under deps/usd/lib.
    if(TRACEY_WITH_USD)
        list(APPEND TRACEY_EDITOR_RPATH "${TRACEY_USD_LIBDIR}")
    endif()
    set_target_properties(tracey_editor PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Tracey Editor"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.tracey.editor"
        MACOSX_RPATH ON
        BUILD_RPATH "${TRACEY_EDITOR_RPATH}"
        INSTALL_RPATH "${TRACEY_EDITOR_RPATH}"
        # Use only the explicit rpath; skip CMake's automatic link-dir rpath.
        # tracey links shaderc via pkg-config, which (statically) propagates
        # -L/opt/homebrew/lib to the editor — and that dir holds conflicting
        # copies of libOpenImageDenoise + libtbb vs deps/oidn/lib, which CMake
        # can't order without a cycle. The explicit rpath already covers every
        # dylib the editor loads (Vulkan+shaderc in /usr/local/lib, oidn+usd+tbb
        # in deps). Same fix as the examples. See examples/CMakeLists.txt.
        BUILD_WITH_INSTALL_RPATH TRUE)

    if(EXISTS "${TRACEY_EDITOR_FRONTEND_DIR}/package.json")
        add_custom_command(TARGET tracey_editor POST_BUILD
            COMMAND npm install --silent
            COMMAND npm run build
            COMMAND ${CMAKE_COMMAND} -E rm -rf
                "$<TARGET_BUNDLE_CONTENT_DIR:tracey_editor>/Resources/editor-dist"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${TRACEY_EDITOR_FRONTEND_DIR}/dist"
                "$<TARGET_BUNDLE_CONTENT_DIR:tracey_editor>/Resources/editor-dist"
            COMMAND codesign --force --sign -
                --entitlements "${CMAKE_CURRENT_SOURCE_DIR}/EditorEntitlements.plist"
                "$<TARGET_BUNDLE_DIR:tracey_editor>"
            COMMAND /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
                -f "$<TARGET_BUNDLE_DIR:tracey_editor>"
            WORKING_DIRECTORY "${TRACEY_EDITOR_FRONTEND_DIR}"
            COMMENT "Building frontend, bundling, and signing Tracey Editor.app")
    endif()

elseif(WIN32)
    add_executable(tracey_editor WIN32
        ${TRACEY_EDITOR_SRCS}
        platform/platform_windows.cpp)

    target_link_libraries(tracey_editor PRIVATE
        tracey
        tracey_editor_json
        glm::glm
        # WebView2Loader, etc. — to be added when the Windows impl is ported.
    )

    if(EXISTS "${TRACEY_EDITOR_FRONTEND_DIR}/package.json")
        add_custom_command(TARGET tracey_editor POST_BUILD
            COMMAND npm install --silent
            COMMAND npm run build
            COMMAND ${CMAKE_COMMAND} -E rm -rf
                "$<TARGET_FILE_DIR:tracey_editor>/editor-dist"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${TRACEY_EDITOR_FRONTEND_DIR}/dist"
                "$<TARGET_FILE_DIR:tracey_editor>/editor-dist"
            WORKING_DIRECTORY "${TRACEY_EDITOR_FRONTEND_DIR}"
            COMMENT "Building frontend and copying alongside tracey_editor.exe")
    endif()

else()
    message(WARNING
        "tracey_editor: no native webview implementation for this platform yet.")
endif()
