// SPDX-License-Identifier: MIT
#pragma once
#import <Metal/Metal.h>
#include "../../runtime/compute_context.hpp"

NS_ASSUME_NONNULL_BEGIN
// Implemented by the actual driver device after it validates the accelerator
// service -> native owner relationship. Public MTLDevice.registryID identifies
// the accelerator service, not our separate native owner's IORegistry entry.
@protocol HNMetalContextOwner <MTLDevice>
- (std::shared_ptr<hackynavi::compute::Context>)nativeComputeContext;
@end
// Private or Managed Metal buffer backed by a real native compute allocation.
// Managed storage owns a separate CPU allocation: didModifyRange performs an
// actual native upload and the ordered blit synchronize operation downloads
// the actual GPU bytes. This is not a Shared-storage mapping.
// Construction requires an already initialized matching device/context. This
// class does not manufacture a Metal device or publish a discovery service.
@interface HNMetalBuffer : NSObject <MTLBuffer>
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
- (nullable instancetype)initWithDevice:(id<HNMetalContextOwner>)device
                               context:(std::shared_ptr<hackynavi::compute::Context>)context
                                length:(NSUInteger)length
                               options:(MTLResourceOptions)options
                                 error:(NSError * _Nullable __autoreleasing * _Nullable)error;
// Managed storage only. Captures the initial bytes into its owned CPU copy
// and uploads them to the actual native allocation before returning success.
- (nullable instancetype)initWithDevice:(id<HNMetalContextOwner>)device
                               context:(std::shared_ptr<hackynavi::compute::Context>)context
                                 bytes:(const void*)bytes
                                length:(NSUInteger)length
                               options:(MTLResourceOptions)options
                                 error:(NSError * _Nullable __autoreleasing * _Nullable)error;
// Driver-internal encoder access. The shared owner pins the actual BO while a
// command captures its resource list. It is never returned to application code.
- (std::shared_ptr<hackynavi::compute::Buffer>)nativeBuffer;
- (std::shared_ptr<hackynavi::compute::Context>)nativeContext;
// Driver-internal command execution only, after earlier native dispatches have
// completed. Leaves the CPU copy unchanged on failure. No synthetic fence.
- (hackynavi::compute::Result)synchronizeManagedContents;
@end
NS_ASSUME_NONNULL_END
