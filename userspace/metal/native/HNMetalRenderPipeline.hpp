// SPDX-License-Identifier: MIT
#pragma once
#import "HNMetalLibrary.hpp"
#include "../../runtime/graphics_snapshot.hpp"
#include "../../runtime/graphics_buffer_snapshot.hpp"

NS_ASSUME_NONNULL_BEGIN
// Actual position and constant-color or readonly buffer-color pipeline. The full
// descriptor is copied and checked before compiling either real stage.
@interface HNMetalRenderPipeline : NSObject <MTLRenderPipelineState>
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
- (nullable instancetype)initWithDevice:(id<HNMetalContextOwner>)device
                             descriptor:(MTLRenderPipelineDescriptor*)descriptor
                                  error:(NSError * _Nullable __autoreleasing * _Nullable)error;
- (std::shared_ptr<hackynavi::compute::Context>)nativeContext;
- (std::shared_ptr<const hackynavi::compute::GraphicsPipeline>)nativePipeline;
- (std::shared_ptr<const hackynavi::compute::FragmentBufferGraphicsPipeline>)nativeBufferPipeline;
- (std::shared_ptr<const hackynavi::compute::VertexBufferGraphicsPipeline>)nativeVertexBufferPipeline;
- (std::shared_ptr<const hackynavi::compute::VertexFragmentBufferGraphicsPipeline>)nativeVertexFragmentBufferPipeline;
@end
NS_ASSUME_NONNULL_END
