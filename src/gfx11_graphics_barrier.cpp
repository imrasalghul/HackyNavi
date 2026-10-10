// SPDX-License-Identifier: MIT
// Source-derived packet sequence: Mesa35b085c4c067f198ce0b6437d94678e47b9c645f,
// ac_barrier.c (CB+DB+INV_L2+PFP_SYNC_ME), ac_cmdbuf_cp.c PWS release/acquire.
// Copyright 2024 Advanced Micro Devices, Inc.
// Copyright 2026 Valve Corporation
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
#include "hackynavi/gfx11_graphics_barrier.hpp"
namespace hackynavi { namespace gfx11graphicsbarrier { namespace {
// RELEASE_MEM and ACQUIRE_MEM have seven payload DWORDs each. RELEASE's
// GCR field encoding differs from ACQUIRE's; no GLI operation is requested.
constexpr uint32_t sequence[kDwords]={
    0xc0064900U,
    0x80703514U, // PWS|SEQ_FORWARD|GL2_WB|GL2_INV|GLM_INV|GLM_WB|index5|event0x14
    0,0,0,0,0,0, // discard data; no address, interrupt or CPU-visible fence
    0xc0065800U,
    0x00022000U, // PWS_ENA2|CP_PFP; TS counter, distance0
    0xffffffffU,0x01ffffffU, // full GCR_SIZE; PWS layout is not plain ACQUIRE
    0,0,0x80000000U,0 // base0, PWS_ENA; remaining GCR operations none
};
bool pointer(const void* p) {
    const uintptr_t address=reinterpret_cast<uintptr_t>(p);
    return address && !(address&(alignof(uint32_t)-1)) &&
        kDwords*sizeof(uint32_t)-1<=UINTPTR_MAX-address;
}
} // namespace
Status encode(uint32_t* output,size_t capacityWords) {
    if(!pointer(output))return Status::InvalidArgument;
    if(capacityWords<kDwords)return Status::BufferTooSmall;
    for(size_t n=0;n<kDwords;++n)output[n]=sequence[n];
    return Status::Ok;
}
Status validate(const uint32_t* commands,size_t exactWords) {
    if(!pointer(commands))return Status::InvalidArgument;
    if(exactWords!=kDwords)return Status::InvalidStream;
    for(size_t n=0;n<kDwords;++n)if(commands[n]!=sequence[n])return Status::InvalidStream;
    return Status::Ok;
}
const char* statusString(Status status) {
    switch(status) {
    case Status::Ok:return "Ok";
    case Status::InvalidArgument:return "InvalidArgument";
    case Status::BufferTooSmall:return "BufferTooSmall";
    case Status::InvalidStream:return "InvalidStream";
    }
    return "Unknown";
}
} }
