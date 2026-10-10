// SPDX-License-Identifier: MIT
#pragma once
#import "HNMetalBuffer.hpp"
#include "../../runtime/compute_shader.hpp"
#include "../../runtime/graphics_shader.hpp"

NS_ASSUME_NONNULL_BEGIN
// Functions retain their actual translated AIR/SPIR-V and reflected stage.
// Graphics support is currently resource-free position/color functions only.
@interface HNMetalFunction : NSObject <MTLFunction>
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
- (std::shared_ptr<const hackynavi::compute::Shader>)nativeShader;
- (std::shared_ptr<const hackynavi::compute::GraphicsShader>)nativeGraphicsShader;
@end

// An immutable owned snapshot of a supported executable metallib container.
// Function names describe the container; creating a supported function
// additionally compiles/verifies its actual AIR and exact entry point.
// Compiler paths are copied immutably and passed explicitly; library creation
// never changes process environment or falls back to tools on PATH.
@interface HNMetalLibrary : NSObject <MTLLibrary>
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
- (nullable instancetype)initWithDevice:(id<HNMetalContextOwner>)device
                          metallibData:(NSData*)data
                         compilerTools:(const hackynavi::compute::Shader::Tools&)tools
                                 error:(NSError * _Nullable __autoreleasing * _Nullable)error;
// Error-reporting internal path also used by the public descriptor APIs.
- (nullable HNMetalFunction*)newStageFunctionWithName:(NSString*)name
                                               error:(NSError * _Nullable __autoreleasing * _Nullable)error;
// Preserves the internal compute-only contract for existing callers.
- (nullable HNMetalFunction*)newComputeFunctionWithName:(NSString*)name
                                                 error:(NSError * _Nullable __autoreleasing * _Nullable)error;
@end
NS_ASSUME_NONNULL_END
