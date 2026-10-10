// SPDX-License-Identifier: MIT
#include "graphics_buffer_snapshot.hpp"
#include "hackynavi/gfx11_graphics_product.hpp"
#include <cstring>
#include <new>

namespace hackynavi { namespace compute { namespace {
transport::Result fail(transport::Status status) { return {status, 0}; }
bool span(const void* address, size_t bytes) {
    return address && bytes && bytes <= UINTPTR_MAX - reinterpret_cast<uintptr_t>(address);
}
transport::Result captureVertexImage(const void* vertex,size_t vertexBytes,const void* fragment,size_t fragmentBytes,
    bool fragmentBuffer,std::array<uint8_t,nativeabi::kGraphicsImageBytes>& image) {
    if(!span(vertex,vertexBytes)||!span(fragment,fragmentBytes))return fail(transport::Status::Invalid);
    gfx11graphics::Layout layout{};
    if(gfx11graphics::layout(vertexBytes,fragmentBytes,layout)!=gfx11graphics::Status::Ok||layout.imageBytes!=image.size())
        return fail(transport::Status::Invalid);
    std::memcpy(image.data(),vertex,vertexBytes);
    std::memcpy(image.data()+layout.fragmentOffset,fragment,fragmentBytes);
    const gfx11graphics::Request request={image.data(),image.size(),vertexBytes,fragmentBytes};
    gfx11graphics::Report report{};
    const auto status=fragmentBuffer?gfx11graphics::validateVertexFragmentBuffers(request,report):
        gfx11graphics::validateVertexBuffer(request,report);
    return status==gfx11graphics::Status::Ok?transport::Result{transport::Status::Ok,0}:fail(transport::Status::Unsupported);
}
transport::Result copyVertexImage(const void* object,size_t objectBytes,
    const std::array<uint8_t,nativeabi::kGraphicsImageBytes>& image,void* output,size_t bytes) {
    if(bytes!=image.size()||!span(output,bytes))return fail(transport::Status::Invalid);
    const uintptr_t destination=reinterpret_cast<uintptr_t>(output),source=reinterpret_cast<uintptr_t>(object);
    if(destination<source+objectBytes&&source<destination+bytes)return fail(transport::Status::Invalid);
    std::memcpy(output,image.data(),bytes);return {transport::Status::Ok,0};
}
}
transport::Result FragmentBufferGraphicsPipeline::capture(const void* vertex, size_t vertexBytes,
    const void* fragment, size_t fragmentBytes, std::shared_ptr<const FragmentBufferGraphicsPipeline>& out) {
    if (out || !span(vertex, vertexBytes) || !span(fragment, fragmentBytes))
        return fail(transport::Status::Invalid);
    gfx11graphics::Layout layout{};
    if (gfx11graphics::layout(vertexBytes, fragmentBytes, layout) != gfx11graphics::Status::Ok ||
        layout.imageBytes != nativeabi::kGraphicsImageBytes)
        return fail(transport::Status::Invalid);
    try {
        auto pipeline = std::shared_ptr<FragmentBufferGraphicsPipeline>(new FragmentBufferGraphicsPipeline);
        std::memcpy(pipeline->image_.data(), vertex, vertexBytes);
        std::memcpy(pipeline->image_.data() + layout.fragmentOffset, fragment, fragmentBytes);
        const gfx11graphics::Request request{pipeline->image_.data(), pipeline->image_.size(), vertexBytes, fragmentBytes};
        gfx11graphics::Report report{};
        if (gfx11graphics::validateFragmentBuffer(request, report) != gfx11graphics::Status::Ok)
            return fail(transport::Status::Unsupported);
        pipeline->vertexBytes_ = uint32_t(vertexBytes);
        pipeline->fragmentBytes_ = uint32_t(fragmentBytes);
        out = std::move(pipeline);
        return {transport::Status::Ok, 0};
    } catch (const std::bad_alloc&) { return fail(transport::Status::OutOfMemory); }
}
transport::Result FragmentBufferGraphicsPipeline::copyImage(void* output, size_t bytes) const {
    if (bytes != image_.size() || !span(output, bytes)) return fail(transport::Status::Invalid);
    const uintptr_t destination = reinterpret_cast<uintptr_t>(output), object = reinterpret_cast<uintptr_t>(this);
    if (destination < object + sizeof(*this) && object < destination + bytes)
        return fail(transport::Status::Invalid);
    std::memcpy(output, image_.data(), bytes);
    return {transport::Status::Ok, 0};
}
transport::Result VertexBufferGraphicsPipeline::capture(const void* vertex,size_t vertexBytes,
    const void* fragment,size_t fragmentBytes,std::shared_ptr<const VertexBufferGraphicsPipeline>& out) {
    if(out)return fail(transport::Status::Invalid);
    try {
        auto pipeline=std::shared_ptr<VertexBufferGraphicsPipeline>(new VertexBufferGraphicsPipeline);
        const auto result=captureVertexImage(vertex,vertexBytes,fragment,fragmentBytes,false,pipeline->image_);
        if(!result)return result;
        pipeline->vertexBytes_=uint32_t(vertexBytes);pipeline->fragmentBytes_=uint32_t(fragmentBytes);
        out=std::move(pipeline);return {transport::Status::Ok,0};
    } catch(const std::bad_alloc&) {return fail(transport::Status::OutOfMemory);}
}
transport::Result VertexBufferGraphicsPipeline::copyImage(void* output,size_t bytes) const {
    return copyVertexImage(this,sizeof(*this),image_,output,bytes);
}
transport::Result VertexFragmentBufferGraphicsPipeline::capture(const void* vertex,size_t vertexBytes,
    const void* fragment,size_t fragmentBytes,std::shared_ptr<const VertexFragmentBufferGraphicsPipeline>& out) {
    if(out)return fail(transport::Status::Invalid);
    try {
        auto pipeline=std::shared_ptr<VertexFragmentBufferGraphicsPipeline>(new VertexFragmentBufferGraphicsPipeline);
        const auto result=captureVertexImage(vertex,vertexBytes,fragment,fragmentBytes,true,pipeline->image_);
        if(!result)return result;
        pipeline->vertexBytes_=uint32_t(vertexBytes);pipeline->fragmentBytes_=uint32_t(fragmentBytes);
        out=std::move(pipeline);return {transport::Status::Ok,0};
    } catch(const std::bad_alloc&) {return fail(transport::Status::OutOfMemory);}
}
transport::Result VertexFragmentBufferGraphicsPipeline::copyImage(void* output,size_t bytes) const {
    return copyVertexImage(this,sizeof(*this),image_,output,bytes);
}
} }
