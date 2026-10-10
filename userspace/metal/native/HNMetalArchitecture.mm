// SPDX-License-Identifier: MIT
#import "HNMetalArchitecture.hpp"

namespace {
[[noreturn]] void unsupportedArchitecture(NSString* selector) {
    @throw [NSException exceptionWithName:NSInvalidArgumentException
        reason:[selector stringByAppendingString:@" has no implemented native architecture contract."]
        userInfo:nil];
}
}

@implementation HNMetalArchitecture {
    NSString* _nativeName;
}
- (instancetype)init { return nil; }
- (nullable instancetype)initWithCapabilities:(const hackynavi::metal::Capabilities&)capabilities {
    // The capability capture independently admitted vendor1002/device744c and
    // GC11.0.0 from the exact live v1.4 owner. Metadata is not GPU liveness.
    if (!capabilities.valid() || !capabilities.ownerRegistryID() || !capabilities.epoch()) return nil;
    self = [super init]; // Measured genuine inherited NSObject initializer.
    if (!self) return nil;
    _nativeName = @"gfx1100"; // Actual admitted compiler target for Navi31.
    return self;
}
- (NSString*)name { return _nativeName; }
- (id)copyWithZone:(NSZone*)zone {
    (void)zone;
    return self; // Immutable actual metadata; ARC preserves copy ownership.
}
// The base class exposes these private scalar/identifier methods. Their
// integer namespace is not established for this backend; inherited zeroed
// base fields must not be reported as a successfully decoded architecture.
- (int)cpuType { unsupportedArchitecture(@"cpuType"); }
- (int)cpuSubtype { unsupportedArchitecture(@"cpuSubtype"); }
- (id)revision { unsupportedArchitecture(@"revision"); }
// Block inherited alternate initializers from constructing an object without
// an actual admitted owner. Neither path takes ownership of caller resources.
- (nullable instancetype)initWithCPUType:(int)cpuType cpuSubtype:(int)cpuSubtype {
    (void)cpuType; (void)cpuSubtype;
    return nil;
}
- (nullable instancetype)initWithCPUType:(int)cpuType cpuSubtype:(int)cpuSubtype revision:(id)revision {
    (void)cpuType; (void)cpuSubtype; (void)revision;
    return nil;
}
@end
