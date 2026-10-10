// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace hackynavi { namespace graphicsmemory {
constexpr uint64_t kVaBegin=4096, kVaEnd=UINT64_C(1)<<47;
constexpr uint64_t kCodeBytes=4096, kAttributeAlignment=65536, kWindowBytes=UINT64_C(1)<<32;
constexpr size_t kMaxMappings=128;
enum class Status : uint32_t { Ok, InvalidArgument, InvalidMapping, NoSpace };
struct Mapping { uint64_t va, bytes; };
struct Plan { uint64_t code, indirect, attributes, indirectBytes, attributeBytes; };
// Pure placement arithmetic, not a mapping/residency/ownership token. Native
// caller holds its VM mutex and passes its complete actual mapping ledger.
// Selects a disjoint contiguous private group in one4GiB window: 64KiB-aligned
// attribute ring, code4KiB, then IB4/8KiB. No addresses are mapped here.
// All mappings must be nonempty/page-aligned/disjoint in the supported VM.
// Attribute bytes64KiB..16MiB, multiple64KiB; IB4/8KiB. Inputs/output distinct
// and stable throughout. Failure leaves output unchanged. No allocation.
Status place(const Mapping*,size_t count,uint64_t attributeBytes,uint64_t indirectBytes,Plan&);
} }
