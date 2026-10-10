// SPDX-License-Identifier: MIT
#include "compute_context.hpp"
#include <cstring>
#include <new>

namespace hackynavi { namespace compute {
namespace {
Result fail(Status status) { return {status, 0}; }
bool same3(const uint32_t* a, const uint32_t* b) { return !std::memcmp(a, b, 3 * sizeof(uint32_t)); }
}
Result Pipeline::variantInfo(uint32_t index, HNPipelineVariant& out) const {
    if (index >= variantCount_) return fail(Status::Invalid);
    out = variants_[index].metadata; return {Status::Ok, 0};
}
Result Pipeline::copyCode(uint32_t index, void* output, size_t bytes) const {
    if (index >= variantCount_ || !output || bytes != variants_[index].code.size() ||
        bytes > UINTPTR_MAX - reinterpret_cast<uintptr_t>(output)) return fail(Status::Invalid);
    std::memcpy(output, variants_[index].code.data(), bytes); return {Status::Ok, 0};
}
Result Pipeline::capture(const HNPipelineResult* source, const uint32_t grid[3],
                         const uint32_t local[3], std::shared_ptr<const Pipeline>& out) {
    if (out || !source || !grid || !local) return fail(Status::Invalid);
    if (hn_pipeline_abi_version() != 1) return fail(Status::Unsupported);
    const char* error = hn_pipeline_error(source);
    if (!error || *error) return fail(Status::Invalid);
    uint64_t threads = 1, gridThreads = 1;
    for (unsigned d = 0; d < 3; ++d) {
        if (!grid[d] || !local[d] || local[d] > nativeabi::kMaxProductLocalThreads ||
            gridThreads > UINT64_MAX / grid[d]) return fail(Status::Invalid);
        threads *= local[d]; gridThreads *= grid[d];
    }
    if (threads > nativeabi::kMaxProductLocalThreads) return fail(Status::Unsupported);
    HNPipelineInfo info{};
    if (hn_pipeline_info(source, &info, sizeof(info)) != 1 || !info.variant_count ||
        info.variant_count > nativeabi::kMaxProductVariants || !info.region_count || info.region_count > 8 ||
        !info.resource_count || info.resource_count > nativeabi::kMaxProductResources ||
        info.target_gfx != 1100 || info.descriptor_set || info.descriptor_stride_bytes != 16 ||
        info.push_constant_bytes != 48 || !info.descriptor_table_bytes || info.descriptor_table_bytes > 512)
        return fail(Status::Unsupported);
    try {
        auto result = std::shared_ptr<Pipeline>(new Pipeline);
        result->variantCount_ = info.variant_count;
        std::memcpy(result->grid_, grid, sizeof(result->grid_));
        std::memcpy(result->local_, local, sizeof(result->local_));
        uint32_t reads = 0, writes = 0;
        for (uint32_t n = 0; n < info.variant_count; ++n) {
            auto& variant = result->variants_[n]; auto& v = variant.metadata;
            if (hn_pipeline_variant(source, n, &v, sizeof(v)) != 1 ||
                !v.code_bytes || v.code_bytes > nativeabi::kMaxProductCodeBytes || (v.code_bytes & 3) ||
                !v.executable_bytes || v.executable_bytes > v.code_bytes || (v.executable_bytes & 3) ||
                !v.code_alignment || v.code_alignment > 4096 || (v.code_alignment & (v.code_alignment - 1)) ||
                v.wave_size != 32 || v.num_sgprs < 8 || v.num_sgprs > 128 || !v.num_vgprs || v.num_vgprs > 256 ||
                v.descriptor_binding_mask != (v.descriptor_read_mask | v.descriptor_write_mask) ||
                v.descriptor_table_bytes != info.descriptor_table_bytes || v.push_constant_bytes != 48)
                return fail(Status::Unsupported);
            uint64_t actualThreads = 1;
            for (unsigned d = 0; d < 3; ++d) {
                if (!v.local_size[d] || v.local_size[d] > local[d]) return fail(Status::Invalid);
                actualThreads *= v.local_size[d];
            }
            if (actualThreads > 1024) return fail(Status::Unsupported);
            for (uint32_t p = 0; p < n; ++p)
                if (same3(v.local_size, result->variants_[p].metadata.local_size)) return fail(Status::Invalid);
            if (hn_pipeline_blob_size(source, HN_PIPELINE_CODE, n) != v.code_bytes) return fail(Status::Invalid);
            variant.code.resize(v.code_bytes);
            if (hn_pipeline_copy_blob(source, HN_PIPELINE_CODE, n, variant.code.data(), variant.code.size()) != 1)
                return fail(Status::Invalid);
            reads |= v.descriptor_read_mask; writes |= v.descriptor_write_mask;
        }
        uint32_t resourceMask = 0, metalMask = 0;
        for (uint32_t n = 0; n < info.resource_count; ++n) {
            HNPipelineResource resource{};
            if (hn_pipeline_resource(source, n, &resource, sizeof(resource)) != 1 || resource.descriptor_set ||
                resource.binding >= 32 || resource.metal_index >= 32 || (resource.access & ~3U) ||
                (resource.reflected_access & ~3U) || (resourceMask & (1U << resource.binding)) ||
                (metalMask & (1U << resource.metal_index))) return fail(Status::Invalid);
            const uint32_t bit = 1U << resource.binding;
            const uint32_t access = ((reads & bit) ? 1U : 0U) | ((writes & bit) ? 2U : 0U);
            if (resource.access != access || resource.binding * 16 + 16 > info.descriptor_table_bytes)
                return fail(Status::Invalid);
            resourceMask |= bit; metalMask |= 1U << resource.metal_index;
            if (access) result->resources_.push_back(resource);
        }
        if (!writes || ((reads | writes) & ~resourceMask)) return fail(Status::Unsupported);
        // Compare every retained compiler region with the canonical exact-thread
        // split. No caller-supplied push constants are sent to the kernel.
        bool seen[8]{}; uint32_t usedVariants = 0, expectedRegions = 0;
        for (unsigned mask = 0; mask < 8; ++mask) {
            HNShaderDispatchRegion expected{}; bool nonempty = true;
            for (unsigned d = 0; d < 3; ++d) {
                const uint32_t full = grid[d] / local[d], tail = grid[d] % local[d];
                const bool last = (mask & (1U << d)) != 0;
                expected.local_size[d] = last ? tail : local[d];
                expected.group_count[d] = last ? 1U : full;
                expected.thread_base[d] = last ? full * local[d] : 0;
                expected.threadgroup_base[d] = last ? full : 0;
                expected.local_size_spec_ids[d] = d;
                expected.push_constants[d] = grid[d];
                expected.push_constants[d + 3] = expected.thread_base[d];
                expected.push_constants[d + 6] = expected.threadgroup_base[d];
                expected.push_constants[d + 9] = full + (tail ? 1U : 0U);
                if ((last && !tail) || (!last && !full)) nonempty = false;
            }
            if (!nonempty) continue;
            ++expectedRegions; expected.push_constant_size = 48;
            uint32_t n = 0;
            for (; n < info.region_count; ++n) {
                HNPipelineRegion region{};
                if (hn_pipeline_region(source, n, &region, sizeof(region)) != 1 || region.variant_index >= info.variant_count)
                    return fail(Status::Invalid);
                if (!std::memcmp(&expected, &region.dispatch, sizeof(expected))) {
                    if (seen[n] || !same3(expected.local_size, result->variants_[region.variant_index].metadata.local_size))
                        return fail(Status::Invalid);
                    seen[n] = true; usedVariants |= 1U << region.variant_index; break;
                }
            }
            if (n == info.region_count) return fail(Status::Invalid);
        }
        if (expectedRegions != info.region_count || usedVariants != ((1U << info.variant_count) - 1))
            return fail(Status::Invalid);
        out = std::move(result); return {Status::Ok, 0};
    } catch (const std::bad_alloc&) { return fail(Status::OutOfMemory); }
}
} }
