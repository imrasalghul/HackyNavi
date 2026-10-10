// SPDX-License-Identifier: MIT
#pragma once
#import "HNMetalBuffer.hpp"
#include "../../runtime/graphics_snapshot.hpp"
#include "../../runtime/graphics_buffer_snapshot.hpp"

NS_ASSUME_NONNULL_BEGIN
// A standalone one-mip/layer/sample RGBA8Unorm render target backed by an
// actual v1.5/v1.6 reusable native allocation. Managed storage has a distinct CPU
// copy, synchronized only through real uploads/ordered native downloads.
@interface HNMetalTexture : NSObject <MTLTexture>
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
- (nullable instancetype)initWithDevice:(id<HNMetalContextOwner>)device
                               context:(std::shared_ptr<hackynavi::compute::Context>)context
                            descriptor:(MTLTextureDescriptor*)descriptor
                                 error:(NSError * _Nullable __autoreleasing * _Nullable)error;
// Driver-only immutable identity/layout. Application-facing parent/buffer/view
// properties do not expose this private allocation or manufacture a view.
- (std::shared_ptr<hackynavi::compute::Context>)nativeContext;
- (HNColorLayout)nativeLayout;
// The command owner retains this texture and calls in queue order. These
// serialize against region I/O; completion is produced only by actual draw.
- (hackynavi::compute::Result)drawWithPipeline:(std::shared_ptr<const hackynavi::compute::GraphicsPipeline>)pipeline
                                  completion:(hackynavi::compute::Completion&)completion;
- (hackynavi::compute::Result)drawWithBufferPipeline:(std::shared_ptr<const hackynavi::compute::FragmentBufferGraphicsPipeline>)pipeline
                                           buffer:(std::shared_ptr<hackynavi::compute::Buffer>)buffer
                                           offset:(uint64_t)offset
                                       completion:(hackynavi::compute::Completion&)completion;
- (hackynavi::compute::Result)synchronizeManagedContents;
- (hackynavi::compute::Result)drawWithVertexBufferPipeline:(std::shared_ptr<const hackynavi::compute::VertexBufferGraphicsPipeline>)pipeline
                                                 buffer:(std::shared_ptr<hackynavi::compute::Buffer>)buffer
                                                 offset:(uint64_t)offset
                                             completion:(hackynavi::compute::Completion&)completion;
- (hackynavi::compute::Result)drawWithVertexFragmentBufferPipeline:(std::shared_ptr<const hackynavi::compute::VertexFragmentBufferGraphicsPipeline>)pipeline
                                                         vertex:(std::shared_ptr<hackynavi::compute::Buffer>)vertex
                                                   vertexOffset:(uint64_t)vertexOffset
                                                       fragment:(std::shared_ptr<hackynavi::compute::Buffer>)fragment
                                                 fragmentOffset:(uint64_t)fragmentOffset
                                                     completion:(hackynavi::compute::Completion&)completion;
@end
NS_ASSUME_NONNULL_END
