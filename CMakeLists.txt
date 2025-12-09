cmake_minimum_required(VERSION 3.16)
project(Raytracer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT TARGET glm::glm-header-only)
    add_subdirectory(deps/glm)
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
)

find_package(Vulkan REQUIRED)

target_link_libraries(tracey PRIVATE glm::glm Vulkan::Vulkan)
target_include_directories(tracey
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
add_subdirectory(./examples)