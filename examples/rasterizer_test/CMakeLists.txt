cmake_minimum_required(VERSION 3.16)

add_executable(rasterizer_test
    rasterizer_test.cpp
)

target_link_libraries(rasterizer_test
    PRIVATE tracey
    PRIVATE volk
    PRIVATE glm::glm-header-only
)

# Set C++ standard and rpath
set_target_properties(rasterizer_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    MACOSX_RPATH ON
    BUILD_RPATH "/usr/local/lib"
    INSTALL_RPATH "/usr/local/lib"
)
