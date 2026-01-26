cmake_minimum_required(VERSION 3.16)

add_executable(graphics_test
    graphics_test.cpp
)

target_link_libraries(graphics_test
    PRIVATE tracey
    PRIVATE volk
    PRIVATE glm::glm-header-only
)

# Set C++ standard and rpath
set_target_properties(graphics_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    MACOSX_RPATH ON
    BUILD_RPATH "/usr/local/lib"
    INSTALL_RPATH "/usr/local/lib"
)

# Copy shaders to build directory
add_custom_command(TARGET graphics_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/simple.vert.spv
        ${CMAKE_CURRENT_SOURCE_DIR}/simple.frag.spv
        $<TARGET_FILE_DIR:graphics_test>
)
