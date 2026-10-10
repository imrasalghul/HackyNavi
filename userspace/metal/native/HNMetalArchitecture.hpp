// SPDX-License-Identifier: MIT
#pragma once
#import <Metal/Metal.h>
#include "HNMetalCapabilities.hpp"

NS_ASSUME_NONNULL_BEGIN
// Genuine public Metal metadata class. This object represents the identity of
// an actually admitted native Context; it creates no device or GPU resources.
@interface HNMetalArchitecture : MTLArchitecture
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
- (nullable instancetype)initWithCapabilities:(const hackynavi::metal::Capabilities&)capabilities;
@end
NS_ASSUME_NONNULL_END
