// SPDX-License-Identifier: MIT
#pragma once
#include "hackynavi/gfx11_color_target.h"
#include <cstddef>
#include <cstdint>

namespace hackynavi { namespace texture {
struct Region { uint64_t x, y, z, width, height, depth; };
// CPU byte-copy geometry only. This never calculates a GPU surface, allocates
// storage, or attests topology. The texture owner first obtains a real Mesa
// layout and validates it against its native context's observations.
class Transfer final {
public:
    uint64_t textureOffset() const { return offset_; }
    size_t rowBytes() const { return row_; }
    size_t rows() const { return rows_; }
    size_t texturePitch() const { return texturePitch_; }
    size_t hostPitch() const { return hostPitch_; }
    size_t hostSpan() const { return hostSpan_; }
    size_t packedBytes() const { return packed_; }
    size_t textureBytes() const { return textureBytes_; }
private:
    friend bool plan(const HNColorLayout&, Region, uint64_t, Transfer&);
    uint64_t offset_ = 0;
    size_t row_ = 0, rows_ = 0, texturePitch_ = 0, hostPitch_ = 0;
    size_t hostSpan_ = 0, packed_ = 0, textureBytes_ = 0;
};
// Nonempty 2D region, mip/slice zero are enforced by the Metal owner. Failure
// leaves output unchanged. Zero row stride denotes one packed source row;
// the API owner applies its read/write-specific stride limits. Arithmetic,
// layout extent and SIZE_MAX are checked.
bool plan(const HNColorLayout&, Region, uint64_t bytesPerRow, Transfer&);
// Metal CPU I/O constraints for this one-image profile. Direction-specific
// stride limits follow Apple's getBytes/replaceRegion documentation.
bool planRead(const HNColorLayout&, Region, uint64_t bytesPerRow, uint64_t bytesPerImage,
              uint64_t level, uint64_t slice, Transfer&);
bool planWrite(const HNColorLayout&, Region, uint64_t bytesPerRow, uint64_t bytesPerImage,
               uint64_t level, uint64_t slice, Transfer&);
// All copies validate complete spans and reject overlap before the first
// write. Only pixel rows are touched: host/texture row padding stays intact.
bool pack(const Transfer&, const void* host, size_t hostBytes, void* packed, size_t packedBytes);
bool commit(const Transfer&, const void* packed, size_t packedBytes, void* texture, size_t textureBytes);
bool read(const Transfer&, const void* texture, size_t textureBytes, void* host, size_t hostBytes);
} }
