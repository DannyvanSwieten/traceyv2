#version 450

// Procedurally emit composition-guide line segments directly in NDC. The
// rendered image fills clip space [-1,1]², so guides are camera-independent
// and overlay both the rasterizer geometry and the path-traced composite
// (this pipeline runs last, depth-test OFF, alpha-blended). No vertex buffer —
// a LINE_LIST drawn from gl_VertexIndex. The push-constant selects which guide
// to emit (baseColor.x): 1 = rule of thirds (4 lines → draw 8 verts),
// 2 = safe areas (action 90% + title 80% rects, 8 lines → draw 16 verts).
// Reuses the shared (mat4 + vec4) push-constant layout; mvp is ignored since
// the positions are already in NDC.

layout(location = 0) out vec4 vColor;

layout(push_constant) uniform PushConstants {
    mat4 mvp;        // unused for guides (positions are NDC)
    vec4 baseColor;  // .x = guide kind (1 = thirds, 2 = safe areas)
} pc;

void main() {
    int kind = int(pc.baseColor.x + 0.5);
    int line = gl_VertexIndex / 2;   // line-segment index
    int end  = gl_VertexIndex & 1;   // 0 = first endpoint, 1 = second

    vec2 a = vec2(0.0);
    vec2 b = vec2(0.0);
    vec4 col = vec4(0.9, 0.9, 0.9, 0.4);

    if (kind == 1) {
        // Rule of thirds: two vertical + two horizontal divider lines.
        const float t = 1.0 / 3.0;
        if (line == 0) { a = vec2(-t, -1.0); b = vec2(-t,  1.0); }
        if (line == 1) { a = vec2( t, -1.0); b = vec2( t,  1.0); }
        if (line == 2) { a = vec2(-1.0, -t); b = vec2( 1.0, -t); }
        if (line == 3) { a = vec2(-1.0,  t); b = vec2( 1.0,  t); }
        col = vec4(0.9, 0.9, 0.9, 0.40);
    } else {
        // Safe areas: action-safe (90%) then title-safe (80%) rectangles.
        // lines 0-3 = action rect, 4-7 = title rect.
        float e = (line < 4) ? 0.9 : 0.8;
        int s = line & 3;
        if (s == 0) { a = vec2(-e, -e); b = vec2( e, -e); } // bottom
        if (s == 1) { a = vec2( e, -e); b = vec2( e,  e); } // right
        if (s == 2) { a = vec2( e,  e); b = vec2(-e,  e); } // top
        if (s == 3) { a = vec2(-e,  e); b = vec2(-e, -e); } // left
        col = (line < 4) ? vec4(0.95, 0.78, 0.25, 0.55)    // action: amber
                         : vec4(0.95, 0.35, 0.35, 0.55);   // title: red
    }

    vec2 p = (end == 0) ? a : b;
    gl_Position = vec4(p, 0.0, 1.0);
    vColor = col;
}
