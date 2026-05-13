#version 450

// Procedurally emit three colored axis lines centred at the anchor point
// in world space. No vertex buffer — we draw 6 vertices (3 lines × 2 verts)
// as a LINE_LIST. The MVP and anchor are pushed via the existing push-
// constant block (mat4 + vec4) that the rest of the rasterizer pipelines
// share, so we can reuse the same layout without inventing a new one.
//
// vertex 0/1 → X axis (origin to anchor+X*len),  red
// vertex 2/3 → Y axis (origin to anchor+Y*len),  green
// vertex 4/5 → Z axis (origin to anchor+Z*len),  blue

layout(location = 0) out vec3 vColor;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    // baseColor.xyz carries the anchor (actor world position); .w carries
    // the axis length in world units. Re-using the shared push-constant
    // shape (mat4 + vec4) keeps Vulkan happy without adding a new layout.
    vec4 baseColor;
} pc;

void main() {
    vec3 anchor = pc.baseColor.xyz;
    float len = pc.baseColor.w;
    int  axis  = gl_VertexIndex / 2;     // 0,0,1,1,2,2 → X,X,Y,Y,Z,Z
    int  end   = gl_VertexIndex & 1;     // 0 = origin end, 1 = tip end

    vec3 dir = vec3(0.0);
    vec3 col = vec3(0.0);
    if (axis == 0) { dir = vec3(1, 0, 0); col = vec3(0.95, 0.25, 0.25); }
    if (axis == 1) { dir = vec3(0, 1, 0); col = vec3(0.30, 0.95, 0.30); }
    if (axis == 2) { dir = vec3(0, 0, 1); col = vec3(0.30, 0.45, 1.00); }

    vec3 world = anchor + dir * len * float(end);
    gl_Position = pc.mvp * vec4(world, 1.0);
    vColor = col;
}
