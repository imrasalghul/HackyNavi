// SPDX-License-Identifier: MIT
#pragma once
#include "hackynavi/isolated_compute.hpp"
namespace hackynavi { namespace gfxaco {
constexpr size_t kControlBytes=4096, kCodeBytes=104, kExecutableBytes=84;
constexpr size_t kParameterBytes=80, kPushOffset=32, kMaxCommandDwords=128;
constexpr uint32_t kThreads=4096, kGuardWords=64, kDataBytes=(kThreads+kGuardWords)*4;
enum class ShaderClass : uint32_t { AppleAddOneGfx1100V1=1 };
enum class Status { Ok,InvalidArgument,ForeignContext,InvalidBinding,Permission,
    AliasedStorage,Overlap,BufferTooSmall,InvalidShader,InvalidParameters,InvalidStream };
struct Request {
    uint64_t owner,epoch;
    uint8_t vmid;
    gfxisolated::Binding shader,source,destination,indirect,parameters;
    // Offsets within the actual retained mappings, not the backing BO.
    // Descriptors and result extent cover kDataBytes; only kThreads run.
    uint64_t sourceOffset,destinationOffset;
};
// One recorded compiler product only. Copies all104 exact ACO bytes and zeros
// the rest of its private4KiB page. This is not an arbitrary shader loader.
Status shaderImage(uint8_t*,size_t capacity);
// Builds two real GFX11 raw-byte descriptors and the actual48-byte translator
// dispatch payload; unused page bytes are zero. All failure outputs unchanged.
Status parameterImage(const Request&,uint8_t*,size_t capacity);
// One canonical wave32 dispatch:64 groups of64 threads, zero START, recorded
// RSRC1/2/3. Uses private parameter pointers in SGPR0..3; no raw caller PM4.
// Caller publishes all mappings/code/data/control pages through real DMA/HDP;
// queue must append and observe its own cache-release fence AND consumed RPTR.
Status encode(const Request&,uint32_t*,size_t capacityWords,size_t& words);
// Rechecks the complete private control pages and canonical stream before
// actual publication. These numeric bindings prove no residency by themselves:
// native owner must resolve exact generation slots/mappings and seal all edits.
// Each control is a private4096-byte BO with exactly one mapping; all five BO
// ids and mapping VA ranges must be disjoint. Source R, destination RW, code/IB
// RX, parameters R. Input and output storage must not alias and stay stable.
Status validate(const Request&,const uint8_t* shader,size_t shaderBytes,
    const uint8_t* parameters,size_t parameterBytes,const uint32_t* stream,size_t words);
const char* statusString(Status);
} }
