// SPDX-License-Identifier: MIT
#pragma once
#import "HNMetalComputePipeline.hpp"
#import "HNMetalRenderPipeline.hpp"
#import "HNMetalTexture.hpp"

NS_ASSUME_NONNULL_BEGIN
// Retained-resource, serial compute commands over the existing real Context.
// This is an internal device-factory entry point, not system device discovery.
// The factory must reject queue logging and other unsupported descriptor fields.
// limit is 1..64. commandBuffer blocks while all execution slots are reserved;
// actual completion frees a slot before handlers run. A completion wait still
// waits for that buffer's handlers to return, independently of slot reuse.
@interface HNMetalCommandQueue : NSObject <MTLCommandQueue>
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
- (nullable instancetype)initWithDevice:(id<HNMetalContextOwner>)device
                 maxCommandBufferCount:(NSUInteger)limit
                                 error:(NSError * _Nullable __autoreleasing * _Nullable)error;
@end

// Constructed only by HNMetalCommandQueue. Each buffer records at most 64
// dispatches, uses one encoder at a time and retains all encoded resources.
@interface HNMetalCommandBuffer : NSObject <MTLCommandBuffer>
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
@end

@interface HNMetalCommandEncoder : NSObject <MTLComputeCommandEncoder>
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
@end

// Ordered managed-buffer synchronization over actual native readback. Other
// blit operations are rejected until their real backend exists.
@interface HNMetalBlitEncoder : NSObject <MTLBlitCommandEncoder>
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
@end

@interface HNMetalRenderEncoder : NSObject <MTLRenderCommandEncoder>
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
@end
NS_ASSUME_NONNULL_END
