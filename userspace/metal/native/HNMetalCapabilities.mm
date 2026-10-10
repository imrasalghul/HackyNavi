// SPDX-License-Identifier: MIT
#include "HNMetalCapabilities.hpp"

namespace hackynavi { namespace metal {
compute::Result Capabilities::capture(const std::shared_ptr<compute::Context>& context,
                                     uint64_t expectedOwner, Capabilities& output) {
    // An admitted immutable snapshot cannot silently change owner or epoch.
    if (output.valid() || !context || !expectedOwner) return {compute::Status::Invalid, 0};
    nativeabi::HelloReply information{};
    auto result = context->information(information);
    if (!result) return result;
    CapabilityData candidate{};
    result = deriveCapabilityData(information, expectedOwner, candidate);
    if (!result) return result;
    output.data_ = candidate;
    return {compute::Status::Ok, 0};
}
} }
