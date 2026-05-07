# C API shared library for Rust FFI
add_library(tracey_c_api SHARED
    tracey_api.cpp
    tracey_api.h
    tracey_types.h
)

# Link against the main tracey library and its dependencies
# Use PUBLIC to propagate include directories and dependencies
target_link_libraries(tracey_c_api
    PUBLIC tracey
    PRIVATE glm::glm
)

# Export headers for FFI
target_include_directories(tracey_c_api
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

# Set output name (no 'lib' prefix on Unix for easier FFI)
set_target_properties(tracey_c_api PROPERTIES
    OUTPUT_NAME "tracey_c_api"
    VERSION 1.0.0
    SOVERSION 1
)

# Install to system directories (for external FFI consumers).
install(TARGETS tracey_c_api
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
    ARCHIVE DESTINATION lib
)

install(FILES
    tracey_api.h
    tracey_types.h
    DESTINATION include/tracey
)
