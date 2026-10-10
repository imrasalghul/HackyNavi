// SPDX-License-Identifier: MIT
#pragma once
#import "HNMetalLibrary.hpp"

NS_ASSUME_NONNULL_BEGIN
// A compiled compute function for the current restricted native profile.
// Construction runs the real compiler for a 1x1x1 prototype and validates its
// owned ISA. Each dispatch builds and validates its own actual shape/variants.
// This object does not discover a device, open a GPU or allocate GPU storage.
@interface HNMetalComputePipeline : NSObject <MTLComputePipelineState>
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
// Narrow internal initializer, not a full descriptor implementation. The
// device factory MUST reject every unsupported descriptor field before using
// this path: stage input, custom buffer mutability, linked/preloaded libraries,
// binary archives/functions, indirect commands, nondefault call depth, shader
// instrumentation, fixed threadgroup requirements, and optimization promises
// such as threadGroupSizeIsMultipleOfThreadExecutionWidth. It must not blindly
// forward a descriptor's function/cap/label and silently discard other fields.
// cap==0 means unspecified. The published maximum is conservatively bounded
// across future variants, then reduced by actual AIR metadata and nonzero cap.
- (nullable instancetype)initWithDevice:(id<HNMetalContextOwner>)device
                               function:(HNMetalFunction*)function
                                acoPath:(NSString*)absolutePath
          maxTotalThreadsPerThreadgroup:(NSUInteger)cap
                                  label:(nullable NSString*)label
                                  error:(NSError * _Nullable __autoreleasing * _Nullable)error;
// Driver-internal exact-thread dispatch compilation. Empty grids are rejected;
// an encoder handles a zero-work no-op separately. out must be empty and stays
// unchanged on every failure. The kernel still revalidates its own code copy:
// this CPU validation is neither a submission nor an execution authority.
- (BOOL)buildForGrid:(MTLSize)grid
 threadsPerThreadgroup:(MTLSize)local
              pipeline:(std::shared_ptr<const hackynavi::compute::Pipeline>&)out
                 error:(NSError * _Nullable __autoreleasing * _Nullable)error;
@end
NS_ASSUME_NONNULL_END
