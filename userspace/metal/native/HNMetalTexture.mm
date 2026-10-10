// SPDX-License-Identifier: MIT
#import "HNMetalTexture.hpp"
#include "../../runtime/texture_region.hpp"
#include <mach/kern_return.h>
#include <mutex>
#include <new>
#include <vector>

namespace {
using namespace hackynavi;
NSError* textureError(NSString* description,compute::Result result) {
    return [NSError errorWithDomain:@"org.hackynavi.Metal.Texture" code:NSInteger(result.status)
        userInfo:@{NSLocalizedDescriptionKey:description,@"IOReturn":@(result.iokit)}];
}
[[noreturn]] void invalid(NSString* description) {
    @throw [NSException exceptionWithName:NSInvalidArgumentException reason:description userInfo:nil];
}
[[noreturn]] void nativeFailure(NSString* description,compute::Result result) {
    @throw [NSException exceptionWithName:NSInternalInconsistencyException reason:description
        userInfo:@{@"error":textureError(description,result)}];
}
texture::Region region(MTLRegion r) {
    return {r.origin.x,r.origin.y,r.origin.z,r.size.width,r.size.height,r.size.depth};
}
}

@implementation HNMetalTexture {
    id<HNMetalContextOwner> _device;
    std::shared_ptr<hackynavi::compute::Context> _context;
    std::shared_ptr<hackynavi::compute::Buffer> _buffer;
    HNColorLayout _layout;
    MTLStorageMode _storageMode;
    BOOL _allowGPUOptimizedContents;
    std::vector<uint8_t> _managed;
    std::mutex _mutex;
    NSString* _label;
}
- (instancetype)init { return nil; }
- (nullable instancetype)initWithDevice:(id<HNMetalContextOwner>)device
                               context:(std::shared_ptr<hackynavi::compute::Context>)context
                            descriptor:(MTLTextureDescriptor*)descriptor
                                 error:(NSError * _Nullable __autoreleasing * _Nullable)error {
    if(error)*error=nil;
    self=[super init];if(!self)return nil;
    using namespace hackynavi;
    if(!device||!context||!descriptor||
        ![device respondsToSelector:@selector(nativeComputeContext)]||[device nativeComputeContext]!=context) {
        if(error)*error=textureError(@"The texture requires its device's exact live native context.",{compute::Status::Invalid,0});
        return nil;
    }
    descriptor=[descriptor copy];
    if(!descriptor) {
        if(error)*error=textureError(@"Unable to snapshot the texture descriptor.",{compute::Status::OutOfMemory,0});
        return nil;
    }
    // Capture descriptor scalars once. No mutable descriptor is retained or
    // consulted by command execution after construction.
    const NSUInteger width=descriptor.width,height=descriptor.height;
    const auto storage=descriptor.storageMode;
    const auto hazards=descriptor.hazardTrackingMode;
    const auto swizzle=descriptor.swizzle;
    const auto options=descriptor.resourceOptions;
    const BOOL allowOptimized=descriptor.allowGPUOptimizedContents;
    BOOL placementSparse=NO;
    if(@available(macOS 26.0,*))placementSparse=descriptor.placementSparsePageSize!=0;
    if(descriptor.textureType!=MTLTextureType2D||descriptor.pixelFormat!=MTLPixelFormatRGBA8Unorm||
        !width||!height||width>16384||height>16384||descriptor.depth!=1||descriptor.mipmapLevelCount!=1||
        descriptor.arrayLength!=1||descriptor.sampleCount!=1||descriptor.usage!=MTLTextureUsageRenderTarget||
        (storage!=MTLStorageModeManaged&&storage!=MTLStorageModePrivate)||
        descriptor.cpuCacheMode!=MTLCPUCacheModeDefaultCache||
        descriptor.compressionType!=MTLTextureCompressionTypeLossless||placementSparse||
        swizzle.red!=MTLTextureSwizzleRed||swizzle.green!=MTLTextureSwizzleGreen||
        swizzle.blue!=MTLTextureSwizzleBlue||swizzle.alpha!=MTLTextureSwizzleAlpha||
        options!=((MTLResourceOptions(storage)<<MTLResourceStorageModeShift)|
                  (MTLResourceOptions(hazards)<<MTLResourceHazardTrackingModeShift))||
        (hazards!=MTLHazardTrackingModeDefault&&hazards!=MTLHazardTrackingModeTracked)) {
        if(error)*error=textureError(@"This texture requires one RGBA8Unorm 2D render-target layer, mip and sample, Private or Managed storage, default CPU cache and tracked hazards.",{compute::Status::Unsupported,0});
        return nil;
    }
    HNColorTopology topology{};HNColorLayout layout{};
    auto result=context->graphicsInformation(topology);
    const HNColorRequest dimensions={uint32_t(width),uint32_t(height)};
    if(result&&(hn_color_target_calculate(&topology,&dimensions,&layout)!=HN_COLOR_OK||
        hn_color_target_validate(&topology,&dimensions,&layout)!=HN_COLOR_OK||
        !layout.total_bytes||layout.total_bytes>nativeabi::kMaxGraphicsTargetBytes||layout.total_bytes>SIZE_MAX))
        result={compute::Status::Unsupported,0};
    if(!result) {
        if(error)*error=textureError(@"The actual native graphics topology and Mesa color layout could not be admitted.",result);
        return nil;
    }
    try {if(storage==MTLStorageModeManaged)_managed.resize(size_t(layout.total_bytes),0);}
    catch(const std::bad_alloc&) {
        if(error)*error=textureError(@"Unable to allocate the managed texture CPU copy.",{compute::Status::OutOfMemory,0});
        return nil;
    }
    result=context->allocate(layout.total_bytes,_buffer);
    if(result&&(_buffer->length()!=layout.total_bytes||
        _buffer->allocatedSize()!=((layout.total_bytes+4095)&~UINT64_C(4095)))) {
        result={compute::Status::Protocol,0};(void)context->close();
    }
    if(!result) {
        if(error)*error=textureError(@"The real native texture allocation could not be created.",result);
        return nil;
    }
    // HackyNaviDMABuffer::allocate zeroes the complete actual native DMA
    // allocation before it can be published. The CPU cache above is distinct.
    _device=device;_context=std::move(context);_layout=layout;_storageMode=storage;
    _allowGPUOptimizedContents=allowOptimized;
    return self;
}
- (id<MTLDevice>)device { return _device; }
- (NSUInteger)allocatedSize { return NSUInteger(_buffer->allocatedSize()); }
- (MTLCPUCacheMode)cpuCacheMode { return MTLCPUCacheModeDefaultCache; }
- (MTLStorageMode)storageMode { return _storageMode; }
- (MTLHazardTrackingMode)hazardTrackingMode { return MTLHazardTrackingModeTracked; }
- (MTLResourceOptions)resourceOptions {
    return (_storageMode==MTLStorageModeManaged?MTLResourceStorageModeManaged:MTLResourceStorageModePrivate)|MTLResourceHazardTrackingModeTracked;
}
- (nullable NSString*)label { std::lock_guard<std::mutex> lock(_mutex);return _label; }
- (void)setLabel:(nullable NSString*)value {
    NSString* copy=[value copy];std::lock_guard<std::mutex> lock(_mutex);_label=copy;
}
- (MTLTextureType)textureType { return MTLTextureType2D; }
- (MTLPixelFormat)pixelFormat { return MTLPixelFormatRGBA8Unorm; }
- (NSUInteger)width { return _layout.width; }
- (NSUInteger)height { return _layout.height; }
- (NSUInteger)depth { return 1; }
- (NSUInteger)mipmapLevelCount { return 1; }
- (NSUInteger)sampleCount { return 1; }
- (NSUInteger)arrayLength { return 1; }
- (MTLTextureUsage)usage { return MTLTextureUsageRenderTarget; }
- (BOOL)isFramebufferOnly { return NO; }
- (BOOL)isShareable { return NO; }
- (BOOL)isSparse { return NO; }
- (BOOL)allowGPUOptimizedContents { return _allowGPUOptimizedContents; }
- (MTLTextureCompressionType)compressionType { return MTLTextureCompressionTypeLossless; }
- (MTLTextureSwizzleChannels)swizzle { return MTLTextureSwizzleChannelsDefault; }
- (MTLResourceID)gpuResourceID { invalid(@"Texture argument-buffer resource IDs are not implemented."); }
- (nullable id<MTLResource>)rootResource { return nil; }
- (nullable id<MTLTexture>)parentTexture { return nil; }
- (NSUInteger)parentRelativeLevel { return 0; }
- (NSUInteger)parentRelativeSlice { return 0; }
- (nullable id<MTLBuffer>)buffer { return nil; }
- (NSUInteger)bufferOffset { return 0; }
- (NSUInteger)bufferBytesPerRow { return 0; }
- (nullable IOSurfaceRef)iosurface { return nullptr; }
- (NSUInteger)iosurfacePlane { return 0; }
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {
    if(state!=MTLPurgeableStateKeepCurrent&&state!=MTLPurgeableStateNonVolatile)
        invalid(@"Purgeable native texture storage is not implemented.");
    return MTLPurgeableStateNonVolatile;
}
- (nullable id<MTLHeap>)heap { return nil; }
- (NSUInteger)heapOffset { return 0; }
- (BOOL)isAliasable { return NO; }
- (void)makeAliasable { invalid(@"A standalone texture cannot become a heap alias."); }
- (kern_return_t)setOwnerWithIdentity:(task_id_token_t)identity { (void)identity;return KERN_NOT_SUPPORTED; }

- (void)getBytes:(void*)bytes bytesPerRow:(NSUInteger)bytesPerRow bytesPerImage:(NSUInteger)bytesPerImage
      fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice {
    using namespace hackynavi;
    texture::Transfer transfer;
    if(_storageMode!=MTLStorageModeManaged||
        !texture::planRead(_layout,region(r),bytesPerRow,bytesPerImage,level,slice,transfer)||
        !bytes)
        invalid(@"Texture CPU reads require Managed storage and an in-bounds level-zero, slice-zero 2D region.");
    std::lock_guard<std::mutex> lock(_mutex);
    if(!texture::read(transfer,_managed.data(),_managed.size(),bytes,transfer.hostSpan()))
        invalid(@"The destination texture byte span is invalid or overlaps owned storage.");
}
- (void)getBytes:(void*)bytes bytesPerRow:(NSUInteger)bytesPerRow fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level {
    [self getBytes:bytes bytesPerRow:bytesPerRow bytesPerImage:0 fromRegion:r mipmapLevel:level slice:0];
}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice
            withBytes:(const void*)bytes bytesPerRow:(NSUInteger)bytesPerRow bytesPerImage:(NSUInteger)bytesPerImage {
    using namespace hackynavi;
    texture::Transfer transfer;
    if(_storageMode!=MTLStorageModeManaged||
        !texture::planWrite(_layout,region(r),bytesPerRow,bytesPerImage,level,slice,transfer)||
        !bytes)
        invalid(@"Texture CPU writes require Managed storage and an in-bounds level-zero, slice-zero 2D region.");
    try {
        std::vector<uint8_t> packed(transfer.packedBytes());
        if(!texture::pack(transfer,bytes,transfer.hostSpan(),packed.data(),packed.size()))
            invalid(@"The source texture byte span is invalid or overlaps the owned snapshot.");
        std::lock_guard<std::mutex> lock(_mutex);
        if([_device nativeComputeContext]!=_context)invalid(@"The texture's native context identity changed.");
        for(size_t row=0;row<transfer.rows();++row) {
            // Exact pixel rows only. Uploading the whole intervening span
            // would overwrite neighboring GPU pixels with stale CPU padding.
            const auto result=_buffer->upload(transfer.textureOffset()+row*transfer.texturePitch(),
                packed.data()+row*transfer.rowBytes(),transfer.rowBytes());
            if(!result) {
                (void)_context->close();
                nativeFailure(@"The native texture upload failed; the context is closed and GPU rows may be partially changed.",result);
            }
        }
        if(!texture::commit(transfer,packed.data(),packed.size(),_managed.data(),_managed.size())) {
            (void)_context->close();nativeFailure(@"The managed CPU texture commit failed after native upload.",{compute::Status::Protocol,0});
        }
    } catch(const std::bad_alloc&) {nativeFailure(@"Unable to snapshot the texture upload.",{compute::Status::OutOfMemory,0});}
}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level withBytes:(const void*)bytes bytesPerRow:(NSUInteger)bytesPerRow {
    [self replaceRegion:r mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:bytesPerRow bytesPerImage:0];
}
- (std::shared_ptr<hackynavi::compute::Context>)nativeContext { return _context; }
- (HNColorLayout)nativeLayout { return _layout; }
- (hackynavi::compute::Result)drawWithPipeline:(std::shared_ptr<const hackynavi::compute::GraphicsPipeline>)pipeline
                                  completion:(hackynavi::compute::Completion&)completion {
    std::lock_guard<std::mutex> lock(_mutex);
    if([_device nativeComputeContext]!=_context)return {hackynavi::compute::Status::Invalid,0};
    return _context->draw(std::move(pipeline),_buffer,0,_layout,completion);
}
- (hackynavi::compute::Result)drawWithBufferPipeline:(std::shared_ptr<const hackynavi::compute::FragmentBufferGraphicsPipeline>)pipeline
                                           buffer:(std::shared_ptr<hackynavi::compute::Buffer>)buffer
                                           offset:(uint64_t)offset
                                       completion:(hackynavi::compute::Completion&)completion {
    std::lock_guard<std::mutex> lock(_mutex);
    if([_device nativeComputeContext]!=_context)return {hackynavi::compute::Status::Invalid,0};
    return _context->draw(std::move(pipeline),_buffer,0,_layout,std::move(buffer),offset,completion);
}
- (hackynavi::compute::Result)synchronizeManagedContents {
    std::lock_guard<std::mutex> lock(_mutex);
    if(_storageMode!=MTLStorageModeManaged)return {hackynavi::compute::Status::Unsupported,0};
    if([_device nativeComputeContext]!=_context)return {hackynavi::compute::Status::Invalid,0};
    // Buffer.download commits the CPU destination only after all actual
    // native reads succeed. No cached or manufactured GPU result is returned.
    return _buffer->download(0,_managed.data(),_managed.size());
}
- (hackynavi::compute::Result)drawWithVertexBufferPipeline:(std::shared_ptr<const hackynavi::compute::VertexBufferGraphicsPipeline>)pipeline
                                                 buffer:(std::shared_ptr<hackynavi::compute::Buffer>)buffer
                                                 offset:(uint64_t)offset
                                             completion:(hackynavi::compute::Completion&)completion {
    std::lock_guard<std::mutex> lock(_mutex);
    if([_device nativeComputeContext]!=_context)return {hackynavi::compute::Status::Invalid,0};
    return _context->draw(std::move(pipeline),_buffer,0,_layout,std::move(buffer),offset,completion);
}
- (hackynavi::compute::Result)drawWithVertexFragmentBufferPipeline:(std::shared_ptr<const hackynavi::compute::VertexFragmentBufferGraphicsPipeline>)pipeline
                                                         vertex:(std::shared_ptr<hackynavi::compute::Buffer>)vertex
                                                   vertexOffset:(uint64_t)vertexOffset
                                                       fragment:(std::shared_ptr<hackynavi::compute::Buffer>)fragment
                                                 fragmentOffset:(uint64_t)fragmentOffset
                                                     completion:(hackynavi::compute::Completion&)completion {
    std::lock_guard<std::mutex> lock(_mutex);
    if([_device nativeComputeContext]!=_context)return {hackynavi::compute::Status::Invalid,0};
    return _context->draw(std::move(pipeline),_buffer,0,_layout,std::move(vertex),vertexOffset,
        std::move(fragment),fragmentOffset,completion);
}
- (nullable id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format {
    (void)format;return nil;
}
- (nullable id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format textureType:(MTLTextureType)type
                                               levels:(NSRange)levels slices:(NSRange)slices {
    (void)format;(void)type;(void)levels;(void)slices;return nil;
}
- (nullable id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format textureType:(MTLTextureType)type
                                               levels:(NSRange)levels slices:(NSRange)slices swizzle:(MTLTextureSwizzleChannels)swizzle {
    (void)format;(void)type;(void)levels;(void)slices;(void)swizzle;return nil;
}
- (nullable MTLSharedTextureHandle*)newSharedTextureHandle { return nil; }
- (nullable id<MTLTexture>)remoteStorageTexture { return nil; }
- (nullable id<MTLTexture>)newRemoteTextureViewForDevice:(id<MTLDevice>)device { (void)device;return nil; }
- (nullable id<MTLTexture>)newTextureViewWithDescriptor:(MTLTextureViewDescriptor*)descriptor API_AVAILABLE(macos(26.0),ios(26.0)) {
    (void)descriptor;return nil;
}
- (MTLTextureSparseTier)sparseTextureTier API_AVAILABLE(macos(26.0),ios(26.0)) { return MTLTextureSparseTierNone; }
@end
