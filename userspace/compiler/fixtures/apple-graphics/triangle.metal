// Authored HackyNavi graphics fixture, MIT license.
// Compiled offline by the actual Apple Metal toolchain. No resource bindings.
#include <metal_stdlib>
using namespace metal;

struct TriangleVertex {
    float4 position [[position]];
};

vertex TriangleVertex hn_triangle_vertex(uint vertexID [[vertex_id]]) {
    float4 position = select(float4(0.0f, 1.0f, 0.0f, 1.0f),
                             float4(1.0f, -1.0f, 0.0f, 1.0f),
                             bool4(vertexID == 1u));
    position = select(position, float4(-1.0f, -1.0f, 0.0f, 1.0f),
                      bool4(vertexID == 0u));
    return TriangleVertex{position};
}

fragment float4 hn_constant_fragment() {
    return float4(0.25f, 0.5f, 0.75f, 1.0f);
}
