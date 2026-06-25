#version 450

// Composition guides: flat RGBA passthrough. Alpha is set per line in the
// vertex shader (thirds read subtler than the safe-area rectangles); this
// pipeline is alpha-blended so the guides sit over the image without hiding it.
layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vColor;
}
