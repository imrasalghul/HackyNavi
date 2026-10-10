// SPDX-License-Identifier: MIT
#pragma once
#include "graphics_snapshot.hpp"

namespace hackynavi { namespace compute {
// A distinct CPU-only image type. It cannot be passed to the existing
// resource-free Context::draw. Actual stage bytes are copied, then independently
// decoded with validateFragmentBuffer; compiler reports are never authority.
class FragmentBufferGraphicsPipeline final {
public:
    static transport::Result capture(const void* vertex, size_t vertexBytes,
        const void* fragment, size_t fragmentBytes,
        std::shared_ptr<const FragmentBufferGraphicsPipeline>&);
    uint32_t vertexBytes() const { return vertexBytes_; }
    uint32_t fragmentBytes() const { return fragmentBytes_; }
    uint32_t fragmentBufferIndex() const { return 0; }
    uint32_t fragmentBufferBytes() const { return 16; }
    uint32_t fragmentBufferAlignment() const { return 16; }
    transport::Result copyImage(void* output, size_t exactBytes) const;
    FragmentBufferGraphicsPipeline(const FragmentBufferGraphicsPipeline&) = delete;
    FragmentBufferGraphicsPipeline& operator=(const FragmentBufferGraphicsPipeline&) = delete;
private:
    FragmentBufferGraphicsPipeline() = default;
    std::array<uint8_t, nativeabi::kGraphicsImageBytes> image_{};
    uint32_t vertexBytes_ = 0, fragmentBytes_ = 0;
};
// Vertex buffer0 supplies three float4 positions. These CPU snapshots bind
// actual machine bytes to distinct draw classes; they grant no GPU ownership.
class VertexBufferGraphicsPipeline final {
public:
    static transport::Result capture(const void* vertex,size_t vertexBytes,
        const void* fragment,size_t fragmentBytes,std::shared_ptr<const VertexBufferGraphicsPipeline>&);
    uint32_t vertexBytes() const {return vertexBytes_;}
    uint32_t fragmentBytes() const {return fragmentBytes_;}
    uint32_t vertexBufferIndex() const {return 0;}
    uint32_t vertexBufferBytes() const {return 48;}
    uint32_t vertexBufferAlignment() const {return 16;}
    transport::Result copyImage(void*,size_t exactBytes) const;
    VertexBufferGraphicsPipeline(const VertexBufferGraphicsPipeline&)=delete;
    VertexBufferGraphicsPipeline& operator=(const VertexBufferGraphicsPipeline&)=delete;
private:
    VertexBufferGraphicsPipeline()=default;
    std::array<uint8_t,nativeabi::kGraphicsImageBytes> image_{};
    uint32_t vertexBytes_=0,fragmentBytes_=0;
};
class VertexFragmentBufferGraphicsPipeline final {
public:
    static transport::Result capture(const void* vertex,size_t vertexBytes,
        const void* fragment,size_t fragmentBytes,std::shared_ptr<const VertexFragmentBufferGraphicsPipeline>&);
    uint32_t vertexBytes() const {return vertexBytes_;}
    uint32_t fragmentBytes() const {return fragmentBytes_;}
    uint32_t vertexBufferIndex() const {return 0;}
    uint32_t vertexBufferBytes() const {return 48;}
    uint32_t vertexBufferAlignment() const {return 16;}
    uint32_t fragmentBufferIndex() const {return 0;}
    uint32_t fragmentBufferBytes() const {return 16;}
    uint32_t fragmentBufferAlignment() const {return 16;}
    transport::Result copyImage(void*,size_t exactBytes) const;
    VertexFragmentBufferGraphicsPipeline(const VertexFragmentBufferGraphicsPipeline&)=delete;
    VertexFragmentBufferGraphicsPipeline& operator=(const VertexFragmentBufferGraphicsPipeline&)=delete;
private:
    VertexFragmentBufferGraphicsPipeline()=default;
    std::array<uint8_t,nativeabi::kGraphicsImageBytes> image_{};
    uint32_t vertexBytes_=0,fragmentBytes_=0;
};
} }
