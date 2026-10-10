// SPDX-License-Identifier: MIT
#pragma once
#include "hackynavi/gfx_compute.hpp"
namespace hackynavi { namespace gfxisolated {
enum Access : uint32_t { Read = 1, Write = 2, Execute = 4 };
enum class Status { Ok, InvalidArgument, ForeignContext, InvalidBinding,
                    Permission, AliasedControlStorage, Overlap, InvalidStream };
// A native owner constructs these descriptions from retained generation slots
// and actual page-tree mappings. They are NOT a userspace wire format or proof
// of residency merely because their numbers validate. owner is a stable native
// context identity; private shader/IB storage has no writable or CPU-user alias.
struct Binding {
    uint64_t owner, epoch;
    uint32_t id, generation;
    uint64_t gpu, bytes, bufferOffset, allocationBytes;
    uint32_t access;
};
struct Request {
    uint64_t owner, epoch;
    uint8_t vmid;
    Binding shader, source, destination, indirect;
    gfxcompute::Request dispatch;
};
// First admitted subset: exactly the existing AMD GFX11 clear/copy dispatch
// grammar and its immutable built-in shader image. Canonical comparison denies
// EVERY extra/changed packet, privileged register, nested IB, predication,
// indirect dispatch, memory/register copy or embedded user fence. The native
// queue appends its own drain/cache-release/fence sequence outside this IB.
// The native caller first captures CPU bytes privately, then calls validate;
// all resources and bytes remain immutable until actual GPU completion.
// This deliberately does not admit arbitrary ACO shaders or RADV command
// streams. A future shader class needs its own metadata/packet validation.
Status validate(const Request&, const uint32_t* captured, size_t words);
const char* statusString(Status);
} }
