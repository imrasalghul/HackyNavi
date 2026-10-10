// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_GFX11_GRAPHICS_BARRIER_HPP
#define HACKYNAVI_GFX11_GRAPHICS_BARRIER_HPP
#include <stddef.h>
#include <stdint.h>
namespace hackynavi { namespace gfx11graphicsbarrier {
constexpr size_t kDwords=16;
enum class Status { Ok,InvalidArgument,BufferTooSmall,InvalidStream };
// Fixed GFX11 graphics-PFP/ME profile, never a compute/MEC queue: full CB/DB
// TS flush, then forward L2/metadata writeback+invalidation, followed immediately
// by PWS acquire at PFP for the last TS event (distance0). No addresses, interrupt,
// memory fence value, predication, caller-selected packet fields or allocation.
// The pair MUST remain adjacent in one immutable trusted IB, with no competing
// queue/process able to insert a PWS TS event between release and acquire.
// Does not invalidate instruction/scalar/vector/L1 caches for future shader
// reads. A later shader consumer requires its own corresponding invalidation.
// Success only encodes/checks bytes. The native owner must establish actual
// gfx11 residency, state/resources, producer exclusion, final GPU-written fence,
// consumed RPTR, CPU visibility and retention/reset before releasing storage.
// Output is unchanged on failure; only the first kDwords are written on success.
// Actual readable/writable CPU objects must remain stable for the whole call.
Status encode(uint32_t* output,size_t capacityWords);
// Exact canonical slice only. It is NOT a validator for an arbitrary draw IB.
Status validate(const uint32_t* commands,size_t exactWords);
const char* statusString(Status);
} }
#endif
