#!/bin/bash

# Compile PBR shaders to SPIR-V
echo "Compiling rasterizer PBR shaders..."

glslc pbr.vert -o pbr.vert.spv
if [ $? -ne 0 ]; then
    echo "ERROR: Failed to compile pbr.vert"
    exit 1
fi

glslc pbr.frag -o pbr.frag.spv
if [ $? -ne 0 ]; then
    echo "ERROR: Failed to compile pbr.frag"
    exit 1
fi

echo "✓ PBR shaders compiled successfully"
ls -lh *.spv
