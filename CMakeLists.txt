cmake_minimum_required(VERSION 3.16)
project(Raytracer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(deps/glm)

add_library(tracey 
    src/types.hpp
    src/ray.hpp
    src/hit.hpp
    src/blas.hpp
    src/blas.cpp
    src/tlas.hpp
    src/tlas.cpp
    src/bvh_node.hpp
    src/intersect.hpp
    src/intersect.cpp
)

target_link_libraries(tracey PRIVATE glm::glm)
add_subdirectory(./examples)