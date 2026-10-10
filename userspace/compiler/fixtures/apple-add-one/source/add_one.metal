// Authored HackyNavi shader fixture, MIT license.
#include <metal_stdlib>
using namespace metal;
kernel void add_one(device const uint *input [[buffer(0)]],
                    device uint *output [[buffer(1)]],
                    uint tid [[thread_position_in_grid]]) {
    output[tid] = input[tid] + 1u;
}
