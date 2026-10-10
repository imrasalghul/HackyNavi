// SPDX-License-Identifier: MIT
#import "HNMetalBuffer.hpp"
#include <mach/kern_return.h>
#include <cstdlib>
#include <cstring>

namespace {
NSError* bufferError(NSString* description, hackynavi::compute::Result result) {
    return [NSError errorWithDomain:@"org.hackynavi.Metal.Buffer" code:NSInteger(result.status)
        userInfo:@{NSLocalizedDescriptionKey: description, @"IOReturn": @(result.iokit)}];
}
void invalid(NSString* reason) {
    @throw [NSException exceptionWithName:NSInvalidArgumentException reason:reason userInfo:nil];
}
struct FreeBytes { void operator()(uint8_t* bytes) const { std::free(bytes); } };
void nativeFailure(NSString* description, hackynavi::compute::Result result) {
    @throw [NSException exceptionWithName:NSInternalInconsistencyException
        reason:description userInfo:@{@"error": bufferError(description, result)}];
}
}
@implementation HNMetalBuffer {
    id<HNMetalContextOwner> _device;
    std::shared_ptr<hackynavi::compute::Context> _context;
    std::shared_ptr<hackynavi::compute::Buffer> _buffer;
    std::unique_ptr<uint8_t, FreeBytes> _managedBytes;
    MTLStorageMode _storageMode;
    NSString* _label;
    NSMutableArray<NSDictionary*>* _markers;
}
- (nullable instancetype)initWithDevice:(id<HNMetalContextOwner>)device
                               context:(std::shared_ptr<hackynavi::compute::Context>)context
                                length:(NSUInteger)length
                               options:(MTLResourceOptions)options
                                 error:(NSError * _Nullable __autoreleasing * _Nullable)error {
    if (error) *error = nil;
    self = [super init];
    if (!self) return nil;
    using namespace hackynavi::compute;
    const auto mode = options & MTLResourceStorageModeMask;
    const MTLResourceOptions allowed = MTLResourceStorageModeMask | MTLResourceHazardTrackingModeMask;
    if (!device || !context || !length || (options & ~allowed) ||
        (mode != MTLResourceStorageModePrivate && mode != MTLResourceStorageModeManaged) ||
        ((options & MTLResourceHazardTrackingModeMask) != MTLResourceHazardTrackingModeDefault &&
         (options & MTLResourceHazardTrackingModeMask) != MTLResourceHazardTrackingModeTracked)) {
        if (error) *error = bufferError(@"This buffer requires Private or Managed storage, default CPU caching, and tracked/default hazards.", {Status::Unsupported, 0});
        return nil;
    }
    hackynavi::nativeabi::HelloReply info{};
    auto result = context->information(info);
    // Public device.registryID names the accelerator nub, whereas info.registryId
    // names the native allocation owner. Bind to the device's exact retained
    // context instead; the device factory must validate the service relation.
    if (result && (![device respondsToSelector:@selector(nativeComputeContext)] ||
                   [device nativeComputeContext] != context)) result = {Status::Invalid, 0};
    if (result) result = context->allocate(length, _buffer);
    if (!result) {
        if (error) *error = bufferError(@"The matching native GPU allocation could not be created.", result);
        return nil;
    }
    _storageMode = mode == MTLResourceStorageModeManaged ? MTLStorageModeManaged : MTLStorageModePrivate;
    if (_storageMode == MTLStorageModeManaged) {
        // The real native allocation has already enforced its maximum size.
        // Own and clear complete pages before exposing a stable CPU pointer.
        const uint64_t allocated = _buffer->allocatedSize();
        void* host = nullptr;
        if (!allocated || allocated > SIZE_MAX || ::posix_memalign(&host, 4096, size_t(allocated)) || !host) {
            if (error) *error = bufferError(@"Unable to allocate the managed CPU copy.", {Status::OutOfMemory, 0});
            return nil;
        }
        _managedBytes.reset(static_cast<uint8_t*>(host));
        std::memset(host, 0, size_t(allocated));
    }
    _device = device; _context = std::move(context);
    _markers = [[NSMutableArray alloc] init];
    if (!_markers) {
        if (error) *error = bufferError(@"Unable to allocate buffer metadata.", {Status::OutOfMemory, 0});
        return nil;
    }
    return self;
}
- (nullable instancetype)initWithDevice:(id<HNMetalContextOwner>)device
                               context:(std::shared_ptr<hackynavi::compute::Context>)context
                                 bytes:(const void*)bytes
                                length:(NSUInteger)length
                               options:(MTLResourceOptions)options
                                 error:(NSError * _Nullable __autoreleasing * _Nullable)error {
    if (error) *error = nil;
    if (!bytes || !length || length > UINTPTR_MAX - reinterpret_cast<uintptr_t>(bytes) ||
        (options & MTLResourceStorageModeMask) != MTLResourceStorageModeManaged) {
        if (error) *error = bufferError(@"Initial buffer bytes require a valid nonempty span and Managed storage.",
                                        {hackynavi::compute::Status::Invalid, 0});
        return nil;
    }
    self = [self initWithDevice:device context:std::move(context) length:length options:options error:error];
    if (!self) return nil;
    std::memcpy(_managedBytes.get(), bytes, length);
    const auto result = _buffer->upload(0, _managedBytes.get(), length);
    if (!result) {
        if (error) *error = bufferError(@"Unable to upload the managed buffer's initial bytes.", result);
        return nil;
    }
    return self;
}
- (id<MTLDevice>)device { return _device; }
- (NSUInteger)length { return NSUInteger(_buffer->length()); }
- (NSUInteger)allocatedSize { return NSUInteger(_buffer->allocatedSize()); }
- (MTLGPUAddress)gpuAddress { return MTLGPUAddress(_buffer->gpuAddress()); }
- (void*)contents { return _managedBytes.get(); }
- (MTLStorageMode)storageMode { return _storageMode; }
- (MTLCPUCacheMode)cpuCacheMode { return MTLCPUCacheModeDefaultCache; }
- (MTLHazardTrackingMode)hazardTrackingMode { return MTLHazardTrackingModeTracked; }
- (MTLResourceOptions)resourceOptions {
    return (_storageMode == MTLStorageModeManaged ? MTLResourceStorageModeManaged : MTLResourceStorageModePrivate) |
        MTLResourceHazardTrackingModeTracked;
}
- (nullable NSString*)label { @synchronized(self) { return _label; } }
- (void)setLabel:(nullable NSString*)value { @synchronized(self) { _label = [value copy]; } }
- (void)didModifyRange:(NSRange)range {
    if (_storageMode != MTLStorageModeManaged || !_managedBytes)
        invalid(@"didModifyRange: requires Managed storage.");
    if (range.location > self.length || range.length > self.length - range.location)
        invalid(@"The modified range must be inside the managed buffer.");
    @synchronized(self) {
        const auto result = _buffer->upload(range.location, _managedBytes.get() + range.location, range.length);
        if (!result) nativeFailure(@"The managed CPU changes could not be uploaded to the native buffer.", result);
    }
}
- (hackynavi::compute::Result)synchronizeManagedContents {
    if (_storageMode != MTLStorageModeManaged || !_managedBytes)
        return {hackynavi::compute::Status::Unsupported, 0};
    @synchronized(self) {
        // The command worker calls this in queue order. The real Context locks
        // against outstanding native work; download copies through an owned
        // temporary and commits only after every actual read succeeds.
        return _buffer->download(0, _managedBytes.get(), size_t(_buffer->length()));
    }
}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {
    if (state != MTLPurgeableStateKeepCurrent && state != MTLPurgeableStateNonVolatile)
        invalid(@"Purgeable storage is not implemented for native HackyNavi allocations.");
    return MTLPurgeableStateNonVolatile;
}
- (nullable id<MTLHeap>)heap { return nil; }
- (NSUInteger)heapOffset { return 0; }
- (BOOL)isAliasable { return NO; }
- (void)makeAliasable { invalid(@"A standalone buffer is not a heap allocation and cannot be made aliasable."); }
- (kern_return_t)setOwnerWithIdentity:(task_id_token_t)identity {
    (void)identity; return KERN_NOT_SUPPORTED;
}
- (nullable id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor*)descriptor
                                            offset:(NSUInteger)offset bytesPerRow:(NSUInteger)bytesPerRow {
    (void)descriptor; (void)offset; (void)bytesPerRow;
    return nil; // No texture object or buffer-texture view is implemented yet.
}
- (nullable id<MTLBuffer>)remoteStorageBuffer { return nil; }
- (nullable id<MTLBuffer>)newRemoteBufferViewForDevice:(id<MTLDevice>)device {
    (void)device; return nil; // No peer-group sharing capability is advertised.
}
- (void)addDebugMarker:(NSString*)marker range:(NSRange)range {
    if (!marker || range.location > self.length || range.length > self.length - range.location)
        invalid(@"The debug marker range must be inside the buffer.");
    @synchronized(self) { [_markers addObject:@{@"label": [marker copy], @"range": [NSValue valueWithRange:range]}]; }
}
- (void)removeAllDebugMarkers { @synchronized(self) { [_markers removeAllObjects]; } }
- (std::shared_ptr<hackynavi::compute::Buffer>)nativeBuffer { return _buffer; }
- (std::shared_ptr<hackynavi::compute::Context>)nativeContext { return _context; }
// SDK26 declares these requirements. Their implementations explicitly reject
// the unavailable features and do not call newer system APIs on macOS15.
- (MTLBufferSparseTier)sparseBufferTier API_AVAILABLE(macos(26.0)) { return MTLBufferSparseTierNone; }
- (nullable id<MTLTensor>)newTensorWithDescriptor:(MTLTensorDescriptor*)descriptor
                                          offset:(NSUInteger)offset
                                           error:(NSError * _Nullable __autoreleasing * _Nullable)error API_AVAILABLE(macos(26.0)) {
    (void)descriptor; (void)offset;
    if (error) *error = bufferError(@"Tensor views are not implemented.", {hackynavi::compute::Status::Unsupported, 0});
    return nil;
}
@end
