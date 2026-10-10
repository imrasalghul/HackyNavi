// SPDX-License-Identifier: MIT
#pragma once
#import "HNMetalCommand.hpp"
#import <IOKit/IOKitLib.h>

NS_ASSUME_NONNULL_BEGIN
// Declaration of the actual framework superclass, not a replacement class.
// No inherited ivars are described here. The Objective-C nonfragile ABI moves
// our own ivars after the framework's real runtime instance size.
@interface _MTLDevice : NSObject
- (void)_wasRemoved;
@end

// The real plugin class named by the HackyNaviAccelerator registry service.
// The initializer adopts the actual runtime MTLDeviceSPI protocol only after
// every required selector and implementation encoding matches the measured
// contract. Unsupported selectors are explicit failures, not capability claims.
@interface HNMetalDevice : _MTLDevice <HNMetalContextOwner>
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
// Actual loader entry point. Resolves the ACO, AIR disassembler and SPIR-V
// validator executables in this class's own Contents/Helpers directory.
// No environment-variable or PATH fallback is accepted.
- (nullable instancetype)initWithAcceleratorPort:(io_service_t)accelerator;
// Internal target-tool entry point. All three explicit helpers must be
// absolute executable regular files; it does not bypass registry, native ABI,
// superclass or publication-contract checks.
- (nullable instancetype)initWithAcceleratorPort:(io_service_t)accelerator
                                        acoPath:(NSString*)absolutePath
                                  compilerTools:(const hackynavi::compute::Shader::Tools&)tools
                                          error:(NSError * _Nullable __autoreleasing * _Nullable)error;
- (std::shared_ptr<hackynavi::compute::Context>)nativeComputeContext;
@end
NS_ASSUME_NONNULL_END
