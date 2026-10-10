// SPDX-License-Identifier: MIT
#include "graphics_snapshot.hpp"
#include "hackynavi/gfx11_graphics_product.hpp"
#include <cstring>
#include <new>

namespace hackynavi { namespace compute { namespace {
transport::Result fail(transport::Status s) { return {s, 0}; }
bool span(const void* p, size_t bytes) {
    return p && bytes && bytes <= UINTPTR_MAX - reinterpret_cast<uintptr_t>(p);
}
}
transport::Result GraphicsPipeline::capture(const void* vertex, size_t vertexBytes,
    const void* fragment, size_t fragmentBytes, std::shared_ptr<const GraphicsPipeline>& out) {
    if (out || !span(vertex, vertexBytes) || !span(fragment, fragmentBytes))
        return fail(transport::Status::Invalid);
    gfx11graphics::Layout layout{};
    if (gfx11graphics::layout(vertexBytes, fragmentBytes, layout) != gfx11graphics::Status::Ok ||
        layout.imageBytes != nativeabi::kGraphicsImageBytes) return fail(transport::Status::Invalid);
    try {
        auto pipeline = std::shared_ptr<GraphicsPipeline>(new GraphicsPipeline);
        std::memcpy(pipeline->image_.data(), vertex, vertexBytes);
        std::memcpy(pipeline->image_.data() + layout.fragmentOffset, fragment, fragmentBytes);
        gfx11graphics::Report report{};
        const gfx11graphics::Request request = {pipeline->image_.data(), pipeline->image_.size(),
                                               vertexBytes, fragmentBytes};
        if (gfx11graphics::validate(request, report) != gfx11graphics::Status::Ok)
            return fail(transport::Status::Unsupported);
        pipeline->vertexBytes_ = uint32_t(vertexBytes);
        pipeline->fragmentBytes_ = uint32_t(fragmentBytes);
        out = std::move(pipeline); return {transport::Status::Ok, 0};
    } catch (const std::bad_alloc&) { return fail(transport::Status::OutOfMemory); }
}
transport::Result GraphicsPipeline::copyImage(void* output, size_t bytes) const {
    if (bytes != image_.size() || !span(output, bytes)) return fail(transport::Status::Invalid);
    std::memcpy(output, image_.data(), bytes); return {transport::Status::Ok, 0};
}
transport::Result graphicsTargetFootprint(const HNColorTopology& topology, const HNColorLayout& layout,
    uint64_t logical, uint64_t allocation, uint64_t offset, uint64_t& footprint) {
    const HNColorRequest request = {layout.width, layout.height};
    if (hn_color_target_validate(&topology, &request, &layout) != HN_COLOR_OK ||
        !logical || logical > allocation || (allocation & 4095) || (offset & 4095) ||
        !layout.total_bytes || layout.total_bytes > nativeabi::kMaxGraphicsTargetBytes ||
        offset > logical || layout.total_bytes > logical - offset)
        return fail(transport::Status::Invalid);
    const uint64_t rounded = (layout.total_bytes + 4095) & ~uint64_t(4095);
    if (offset > allocation || rounded > allocation - offset) return fail(transport::Status::Invalid);
    footprint = rounded; return {transport::Status::Ok, 0};
}
} }
