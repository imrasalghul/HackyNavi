// Authored HackyNavi fragment buffer fixture, MIT license.
// Real read-only GPU resource; color is not a source constant.
#include <metal_stdlib>
using namespace metal;

fragment float4 hn_buffer_fragment(constant float4& color [[buffer(0)]]) {
    return color;
}
