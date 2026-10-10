# Architecture metadata and native process admission

The production `HNMetalArchitecture` is a real subclass of the public
`MTLArchitecture` class. It exposes the compiler target `gfx1100` only after
`Capabilities` has admitted the live native v1.4 Navi 31 context. The object
owns immutable name metadata and implements copying by retaining that same
immutable object. It neither creates a device nor opens or submits GPU work.

The exact SDK `MTLDevice.h` declares only `name` and `NSCopying` for this public
class. The actual macOS 15.8/24H22 runtime has a 32-byte superclass instance.
Its inherited `init` is the genuine NSObject method: move self into the return
register and return. The complete 74-byte `MTLArchitecture dealloc` releases
the two base pointer fields and calls superclass deallocation. Those fields
remain zero initialized in our subclass; no production code writes private
ivar offsets. The Objective-C nonfragile ABI places our own ivar after the
actual superclass layout. Our `copyWithZone:` overrides the base implementation,
which otherwise copies the private base fields rather than our own name.

Private `cpuType`, `cpuSubtype`, and `revision` meanings are not established
for this compiler target. Their exact measured return types are `int`, `int`,
and object, respectively. Typed overrides raise an explicit unsupported
exception; zeroed base fields are never reported as an architecture code.
Default initialization and both observed CPU-code initializers return nil.
Only the initializer accepting actual captured capabilities constructs the
object. This is public architecture metadata, not a fabricated private target
profile or a substitute for the missing private target-architecture APIs.

## Loader condition

The actual Metal publication helper calls `_CFMZEnabled` after `initLimits`,
`initFeatureQueries`, and `initWorkarounds`. It calls
`allowLibrariesFromOtherPlatforms` only when that function returns nonzero.
The complete CoreFoundation helper calls `dyld_get_active_platform` and returns
true exactly for platform 6 or 2. Its actual call stub and mapped pointer resolve
to that named libdyld export. Apple's primary
[dyld declaration](https://github.com/apple-oss-distributions/dyld/blob/main/include/mach-o/dyld_priv.h)
defines a `uint32_t` result with no arguments; Apple's
[Mach-O platform constants](https://github.com/apple-oss-distributions/xnu/blob/main/EXTERNAL_HEADERS/mach-o/loader.h)
identify native macOS as 1, iOS as 2, and Mac Catalyst as 6.

The concrete admission rule is therefore to resolve this actual metadata API
and accept only `PLATFORM_MACOS` before opening the native context. A missing
symbol or any other platform fails admission. The unsupported cross-platform
policy method can remain an explicit failure: native macOS does not reach it.
No loader function is replaced, no policy result is modified, and no private
platform constant is guessed. Device implements this guard independently in
its own source freeze.

## Evidence and limits

`build/metal-architecture/capture.json` binds 110 selected code bytes;
`followup.json` binds the 74-byte deallocator and rechecks the 36-byte CF helper,
plus a bounded call-stub/pointer resolution. The two disassembly reports verify
every instruction byte against the captures. The three bytes after NSObject's
actual return are explicitly recorded as padding. Existing same-image evidence
in `build/metal-device-hooks/references.json` resolves `objc_release`,
`objc_msgSendSuper2`, and the `dealloc` selector used by the new capture.

Metal UUID: `c3efbabe-6b06-33ab-bc45-43fe86297aa1`.
CoreFoundation UUID: `c204e205-b457-333e-8da9-3d11902a7390`.
The target's reported macOS 15.8.1 build has not been measured here.
No GPU/device/object construction or target execution occurred in this audit.
Native compilation and actual object lifetime validation are separate evidence;
the source freeze does not claim either has passed.
