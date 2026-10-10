// SPDX-License-Identifier: MIT
#pragma once
#include "../transport/iokit_client.hpp"
#include <array>
#include <memory>

namespace hackynavi { namespace compute {
class Context;
// Immutable copy of real NGG/fragment machine bytes. capture() copies both
// input spans before independently decoding their actual instructions. A
// compiler's JSON, register metadata or prior Report is never accepted.
// This CPU admission does not confer native allocation/submission authority.
class GraphicsPipeline final {
public:
    static transport::Result capture(const void* vertex, size_t vertexBytes,
        const void* fragment, size_t fragmentBytes, std::shared_ptr<const GraphicsPipeline>&);
    uint32_t vertexBytes() const { return vertexBytes_; }
    uint32_t fragmentBytes() const { return fragmentBytes_; }
    transport::Result copyImage(void* output, size_t exactBytes) const;
private:
    friend class Context;
    GraphicsPipeline() = default;
    std::array<uint8_t, nativeabi::kGraphicsImageBytes> image_{};
    uint32_t vertexBytes_ = 0, fragmentBytes_ = 0;
};

// Pure bounds/layout check used before actual mapping. The supplied topology
// is numeric data only; Context uses its retained native query and the kernel
// rechecks its private owner. Output stays unchanged on failure. The logical
// image must fit application storage; only its rounded page footprint may use
// allocation padding. No client-selected register fields bypass validation.
transport::Result graphicsTargetFootprint(const HNColorTopology&, const HNColorLayout&,
    uint64_t logicalBytes, uint64_t allocationBytes, uint64_t offset, uint64_t& footprint);
} }
