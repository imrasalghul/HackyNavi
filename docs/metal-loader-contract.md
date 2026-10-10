# Measured Metal loader and device-construction contract

The installed loader accepts a genuine `_MTLDevice` subclass. It does not
require the plugin class to derive from `MTLIOAccelDevice`. This makes a real
`_MTLDevice` subclass connected to HackyNavi's existing native compute Context
the preferred implementation route. The type-5/type-6 clients investigated in
`metal-public-accelerator-route.md` are dependencies of the alternative
`MTLIOAccelDevice` initializer; they are not prerequisites established by this
loader. That earlier document and its evidence remain historical.

This is measured design evidence, not a working discovery or Metal claim. The
measurements read loaded code and metadata on the Intel mini running macOS
15.8, build 24H22. Metal image UUID is
`c3efbabe-6b06-33ab-bc45-43fe86297aa1`. The user's reported 15.8.1 target is
not this measured build. No device was enumerated or instantiated, no
Objective-C message was sent by the capture, no user client was opened and no
GPU operation occurred. Remote scripts ran through SSH stdin and created no
remote files. Constant CFString and Objective-C runtime metadata getters were
used only on validated, code-referenced locations.

## Actual discovery and plugin selection

All offsets below are relative to the measured Metal image base. Complete
captured bytes, exact reference resolution and literal-byte-checked offline
disassembly are in `build/metal-public-accelerator-route/`.

1. `MTLCopyAllDevices` at `0x13d2` initializes the device array, then copies it
   on its serial queue. Its initializer at `0x11752a` eventually calls
   `MTLRegisterDevices` at `0x19f79f`. The separate system-default-device path
   also contains a CoreGraphics fallback; that fallback is not our route.
2. `MTLRegisterDevices` creates `MTLIOAccelServiceGlobalContext`. Its actual
   `init` at `0x19ec2c` creates the notification port/queue and calls
   `IOServiceMatching("IOAccelerator")`, then registers `IOServicePublish`.
   Initial iterator entries and later matching notifications converge on
   `registerService:deviceClass:` at `0x19eed8`.
3. `getMetalPluginClassForService` at `0x19e98d` reads the exact registry key
   `MetalPluginName`, requires a CFString, converts it to UTF-8 and calls
   `gpu_bundle_find_trusted`. Only a successful result supplies the path to
   `NSBundle bundleWithPath:`. The helper resolves to the actual
   `/usr/lib/system/libsystem_sandbox.dylib`; its path/trust policy has **not**
   been measured. An arbitrary plugin path or unsigned bundle is not proved
   acceptable by this capture.
4. It reads `MetalPluginClassName`. A usable class name is passed to
   `classNamed:`; otherwise the bundle's `principalClass` is used. It then
   requires `isSubclassOfClass:` against the actual `OBJC_CLASS_$__MTLDevice`
   loaded from the verified class reference. The comparison target is
   `_MTLDevice`, not `MTLIOAccelDevice`.
5. Registration avoids duplicate service ports, then constructs an actual
   `MTLIOAccelService` wrapper with the selected class and service port.

The measured matching and plugin-selection code has no
`IOGraphicsAccelerator2` requirement. Public `IOAccelerator` inheritance is
therefore sufficient for the observed match. This is not a claim that registry
publication alone supplies a working accelerator, or that unexamined removal
and downstream methods have no additional contracts.

## Wrapper construction and publication

`MTLIOAccelService initWithAcceleratorPort:deviceClass:` is at `0x19f327`,
602 captured bytes, encoding `@28@0:8I16#20`. After its actual superclass
initializer it follows these relevant steps:

- It calls `IOObjectConformsTo(service, "IOServiceCompatibility")`. A **true**
  result rejects the service. This is an exclusion, not a required class.
- It retains the real service port, reads that service's registry ID and
  registers an `IOGeneralInterest` notification on its notification queue.
  The `Removable` and `SafeEjectRequested` properties can exclude/defer device
  creation along the measured branches.
- It stores the wrapper in the global service collection and records the
  selected class. When eligible, it allocates that class and sends
  `initWithAcceleratorPort:` with the real accelerator service port.
- A nil initializer result is not published. A non-nil result is stored
  weakly in the wrapper, sent `_setAcceleratorService:` with that wrapper,
  passed to `MTLAddDevice`, and then locally released.

`MTLAddDevice` at `0x2110` asserts that the device conforms to both the actual
`MTLDevice` and `MTLDeviceSPI` protocols. It then sends, in order,
`initLimits`, `initFeatureQueries` and `initWorkarounds`. A conditional branch
also sends `allowLibrariesFromOtherPlatforms`. It dispatches the final
publication block (`__MTLAddDevice_block_invoke`, `0xac51`) to the global
device-array queue. The block and the service-interest callback have not yet
been captured, so final array ownership/removal is not fully established.

The protocol checks are real runtime conformance checks. Merely declaring
conformance cannot implement required behavior. No successful zero reply,
arbitrary capability field, interposed enumeration API or NSObject pretending
to be a device is justified by this evidence.

## Base initializer and lifetime facts

The actual `_MTLDevice init` at `0x178a` has encoding `@16@0:8`; all 905 bytes
were captured. Its actual `dealloc` at `0xfb6e3` has encoding `v16@0:8`; all
546 bytes were captured. They use real superclass initialization/deallocation,
CPU dispatch queues, containers, a resource-list pool, logging and library
bookkeeping. Resolved C++ helpers include `MTLLibraryBuilder` construction and
destruction and `MTLVersionedDB` destruction. No direct type-5/type-6 client
open occurs in the captured base initializer. Transitive constructors and all
base method behaviors have not been followed, so the absence of those direct
calls does not prove the entire inherited implementation is backend-neutral.

These runtime methods were queried without invoking them:

| Selector on `_MTLDevice` | Measured encoding | Metal image offset | Status |
|---|---|---:|---|
| `initLimits` | `v16@0:8` | `0x22dc` | Body still needed |
| `initFeatureQueries` | `v16@0:8` | `0x2feb` | Body still needed |
| `initWorkarounds` | `v16@0:8` | `0x1044e6` | Body still needed |
| `allowLibrariesFromOtherPlatforms` | `v16@0:8` | `0xfbdb7` | Body still needed |
| `_setAcceleratorService:` | absent | — | Subclass must implement the actual lifetime contract |

Do not write a guessed `_MTLDevice` instance layout. Objective-C runtime
subclassing should preserve the real superclass layout and initialization.
The service setter's exact ownership and the removal path remain required
measurements before implementing that association. The inherited
`MTLDeviceSPI` conformance and required selectors must also be verified rather
than assumed.

## Smallest real implementation route

```text
IOPCIDevice
  HackyNaviDevice                         existing single hardware owner
    existing private compute client      actual Context / BO / job lifecycle
    HackyNavi accelerator : IOAccelerator
      MetalPluginName / MetalPluginClassName
        real bundle class : _MTLDevice
          initWithAcceleratorPort:        exact owner relationship + Context
          actual Metal resource/library/pipeline/command objects
```

The public accelerator child must retain and coordinate the existing owner;
it must not reopen the PCI device or construct a second engine/GART lifetime.
The plugin initializer must authenticate that accelerator's relationship to
the existing owner before opening and negotiating the native Context. Its
public accelerator registry ID and the Context's owner registry ID are
different identities. Private resource objects should use the exact retained
Context owner, as the current object implementations already require.

The next implementation prerequisites are narrowly bounded:

1. Read `gpu_bundle_find_trusted` and its immediate policy inputs to establish
   the real bundle location/admission route. Do not install a bundle first.
2. Read the existing `MTLIOAccelDevice _setAcceleratorService:` implementation,
   wrapper deallocation/removal callbacks and final publication block to
   establish authentic association lifetime. This does not require adopting
   `MTLIOAccelDevice` as the superclass.
3. Read the four inherited initialization bodies listed above and query
   `_MTLDevice` protocol conformance/required method encodings. Identify exact
   selectors/capability fields that must derive from the real backend before
   any device construction.
4. Implement the real public accelerator child and backend-connected class
   only against those established contracts. Keep unimplemented operations
   explicit and do not advertise graphics, shared storage or other features
   that the backend cannot perform.

These measurements can be userland-only code/metadata reads with explicit
function/count/byte bounds. No additional kernel-collection capture or
type-5/type-6 protocol emulation is presently needed for this path choice.

## Evidence validation and limits

`scripts/validate-metal-loader-evidence.py` binds the document, capture
scripts, every input/execution/capture file and final disassembly to SHA-256.
It independently checks each code hash, function bounds, instruction-byte
coverage, and the two explicit jump-table suffixes against decoded instruction
targets. The final byte of the bounded `MTLAddDevice` range is recorded as
trailing zero padding rather than silently decoded as an instruction.
Historical failed attempts to linearly disassemble those data bytes remain
preserved; they are not the final validation result.

The report establishes reproducible measured facts and the next implementation
boundary. It does not establish target-machine discovery, bundle signing
acceptance, complete superclass behavior, object construction, hardware
execution, application compatibility or a bootable accelerated desktop.
