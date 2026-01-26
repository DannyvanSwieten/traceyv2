#!/bin/bash

# Compile GLSL shaders to SPIR-V
glslc simple.vert -o simple.vert.spv
glslc simple.frag -o simple.frag.spv

echo "Shaders compiled successfully"
