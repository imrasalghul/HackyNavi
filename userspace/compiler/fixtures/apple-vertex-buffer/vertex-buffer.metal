// Authored HackyNavi vertex-buffer fixture, MIT license.
// Actual Apple compiler input. The three positions come from the caller's BO.
#include <metal_stdlib>
using namespace metal;

struct BufferVertex {
    float4 position [[position]];
};

vertex BufferVertex hn_buffer_vertex(constant float4* positions [[buffer(0)]],
                                     uint vertexID [[vertex_id]]) {
    return BufferVertex{positions[vertexID]};
}
