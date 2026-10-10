# Native Metal compute objects

`HNMetalDevice` now connects these objects through a genuine subclass of the
framework's exported `_MTLDevice`. Its own ivars use the Objective-C nonfragile
ABI; it does not copy the private superclass layout. The initializer validates
the real `HackyNaviAccelerator`, its immediate `HackyNaviDevice` provider and
the provider's registry ID against `HackyNaviOwnerRegistryID`. Only that owner
ID is used to open the actual reusable compute context. The accelerator ID is
retained separately for the public Metal registry identity.

The public loader entry point resolves `hackynavi-aco`, `apple-air-dis` and
`spirv-val` in its own `HackyNaviMetal.bundle/Contents/Helpers`, verifies each
canonical path remains inside that bundle, and requires actual executable
regular files. No environment or PATH override is accepted. The internal
target-tool initializer takes all three explicit paths with the same registry,
executable and native-profile checks. Library objects copy the two compiler
paths immutably and pass them to the real explicit-tools shader API; the owned
shader retains those paths for later pipeline validation. No plugin method
changes the application environment.
The device connects Private/Managed buffer allocation, bounded owned metallib loading,
actual compute pipeline compilation and serial command queue creation. Compute
descriptors validate every supported SDK field, including all 31 real buffer
descriptor entries, before passing function/cap/label to the compiler object.
Unsupported source compilation, graphics, texture, Shared storage, optional
pipeline and Metal 4 factories return explicit errors or nil. Unknown private
profiles, private architecture codes and timestamps are not made up. The public
architecture object is an immutable genuine `MTLArchitecture` subclass holding
the admitted gfx1100 identity. Allocation
accounting reads the actual live native owner's serialized BO byte count.

The device source now implements the measured private device contract using
owned limits and feature state. It overrides 391 required SPI methods directly;
the remaining notification hook is the genuine framework implementation.
Unsupported feature queries return false, while unknown profiles, emulation
requirements and backend operations raise explicit unsupported errors. The
actual limits helper derives a complete owned 264-byte structure from the live
native profile. Its 63 scalar accessors read that owned structure.

Before opening the native owner, the initializer compares the framework's
actual `limits` encoding with the compiler's exact const-pointer encoding. It
also checks every required selector and implementation encoding against the
measured 392-method contract, rejects duplicates or unexpected class methods,
and only then adopts the existing framework `MTLDeviceSPI` protocol. It creates
no replacement protocol. The device owns its read/write-buffer and nonuniform
dispatch query state and retains the genuine superclass workaround hook after
its truthful family-query override.

The source freeze is `build/metal-device-spi-local/source-freeze-6.json`.
`build/metal-device-native-2/report.json` records strict native compilation of
all 14 frontend/runtime objects, linking against the actual 32-export compiler,
and compilation of the public Metal target smoke. The four exported artifacts
include the real `HackyNaviMetal` MH_BUNDLE and frontend dylib; their remote
stage was removed. The first native attempt remains preserved as evidence of
SDK availability/deprecation declaration fixes; the retry changed no method
bodies. No Metal object or GPU operation was executed by either build.

The bounded static checker inspects the linked Objective-C metadata without
loading either image. `objc-methods-dylib-2.json` and
`objc-methods-bundle-2.json` in that build directory verify all 391 implemented
required selector/type pairs, the single inherited notification hook and the
strong import of the genuine `_MTLDevice` from Metal. Both images contain the
same 518 Device instance methods. Runtime checks still validate the actual
framework contract before construction on the target.

The framework's measured other-platform policy predicate checks the real dyld
active platform for iOS or Mac Catalyst. Before opening the context, the device
requires the actual metadata API to report native macOS; other platforms and
an unavailable API fail construction. It does not invent other-platform shader
acceptance. Bundle trust, installation and public discovery remain unverified.
No bundle was installed and no Metal device object was constructed by this work.

`HNMetalBuffer` implements a Private or Managed `MTLBuffer` backed by an actual
`compute::Buffer`. Construction requires the initialized driver's exact retained
native compute context; it then allocates real native storage. The private
`HNMetalContextOwner` protocol identifies that context. The device factory must
validate the accelerator-service-to-native-owner relationship when it opens the
backend. Public Metal registry IDs and native-owner registry IDs identify
different services, so they are not compared as if they were interchangeable.
No substitute device is created for testing or discovery.

The object reports the actual logical length, padded allocation size and stable
reserved GPU address. Private storage has no application CPU pointer. Managed
storage owns a separate page-aligned CPU allocation, initially zeroed alongside
the zeroed native GPU allocation. Initial bytes are copied and uploaded before
construction succeeds. `didModifyRange` uploads the requested actual bytes;
failure raises an exception containing the native error. Labels and debug
marker ranges are retained in the object.

The blit encoder's `synchronizeResource` records a retained Managed buffer in
the same ordered stream as compute dispatches. Execution downloads the actual
GPU contents only after earlier dispatches have completed. Native download
keeps the CPU copy unchanged on failure. This operation creates no synthetic
GPU fence or sequence. Applications must observe Metal's Managed CPU/GPU access
synchronization rules. The Managed and ordered readback components passed
strict native compilation and linking in `build/metal-managed-native-1`: all
11 real source objects and the public Metal target smoke compiled, three
artifacts were hash-verified and exported, and the remote stage was removed.
That frozen snapshot excludes the unfinished Device/Capabilities layer and
predates explicit compiler-tool integration. No test executable, Metal object
or target GPU operation was run by that build.

This implementation accepts Private/Managed storage with default CPU caching
and default/tracked hazards. Shared storage, purgeable memory, heap allocation/aliasing,
texture/tensor views, peer-device sharing and VM ownership reassignment are not
implemented. Unsupported operations reject requests explicitly. The object is
not yet verified through system Metal discovery: the actual device
feature/limit implementation now compiles, while accepted bundle installation
and target discovery remain unverified.

The implementation uses the actual SDK26.2 public `MTLBuffer` and `MTLResource`
declarations captured in `build/metal-discovery-prerequisites/sdk-headers`, and
the inherited `MTLAllocation` declaration exported by the native runtime build.
The additional SDK26 tensor/sparse declarations do not enable those features
on the macOS15 target. `build/native-compute-runtime-final` passed strict ARC
protocol compilation, real linking and 233 compiler-backed sanitizer checks.
The later exact-context identity correction also passed its focused ObjC
relink in `build/native-compute-runtime-identity-final`; the four unchanged
tested C++ objects were verified byte-for-byte and the stage was removed.
No substitute device or buffer object was instantiated, and execution on the
target GPU remains unverified.

`HNMetalLibrary` owns a parsed executable metallib snapshot through the bounded
container C ABI. It copies its function-name index and compiles the exact
selected AIR when creating a compute `HNMetalFunction`. The function retains
the actual `compute::Shader`, including owned SPIR-V/reflection, independently
of the library and caller input. Function labels are synchronized; asynchronous
requests snapshot mutable input and deliver their actual result/error once.
No process-global compiler environment is changed by these objects.

The initial function object supports ordinary compute functions. Unsupported
graphics functions, function constants, argument buffers, binary archives,
visible/intersection functions and stage-input attributes produce errors.
Empty constants/attribute reflection is reported only after the actual
compiler output establishes that contract. This is not an MSL source compiler,
and the original MTLB2.5 fixture contains graphics functions only. The actual
Apple 32023 toolchain has since compiled the authored `add_one.metal` for
macOS15/MSL2.4 into the pinned MTLB2.8 compute fixture. The real parser extracts
its owned AIR and the actual compiler produces the same admitted ISA from that
AIR and the metallib. Metal library/function object construction on the target
remains unverified.

`build/native-compute-shader-library-final-2/report.json` records strict native
ARC compilation/linking of these objects and 252 offline ASan/UBSan checks for
the retained Shader layer using real AIR and ACO. All six exported artifacts
were verified, and the 60,328,604-byte stage was removed. Five unchanged native
runtime objects were reused with byte-for-byte verification. No Metal object
was constructed and no native GPU connection was opened. The resulting dylib
is a component for the future driver bundle, not a system-discovered Metal
driver.

`HNMetalComputePipeline` constructs a real 1x1x1 prototype from the function's
owned Shader, then compiles and admits the actual shape of every dispatch.
Its published thread limit is at most 640, further reduced by actual AIR
metadata and a nonzero requested cap. Every real variant is checked against
the shared ISA validator and register occupancy limit. Wave32 and zero static
LDS describe this verified narrow profile; no indirect, imageblock or pipeline
GPU-address capability is invented. The pipeline retains CPU code, so its
persistent GPU allocation size is zero.

`build/native-compute-pipeline-final/report.json` binds strict SDK26.2 ARC
compilation, the real nine-object runtime link, and 265 ASan/UBSan checks of
actual compiler/admission products. All 20 native variant-code hashes match
the corresponding Windows compiler products. The seven reused objects and
six exports were verified, and the 60,486,732-byte stage was removed. These
checks did not construct Metal objects, open a native context or execute GPU
work. The runtime dylib hash is
`722af6710c50549769135cd1b4e85f30e2347b10e298b6bc92076dd203af505c`.

`HNMetalCommandQueue`, `HNMetalCommandBuffer` and `HNMetalCommandEncoder` now
record retained-resource serial compute commands for that same existing
device/context. The worker first builds all actual dispatch products and
checks reflected buffer indices, aligned offsets, remaining descriptor spans
and resource identity. It then calls the real synchronous `Context::dispatch`
for each command. A second dispatch can use the first dispatch's actual output
BO directly; command execution introduces no CPU readback/reupload substitute.
Kernel admission and real native retirement remain authoritative.

The queue uses the production `command_lifecycle` ticket scheduler. An earlier
enqueued but uncommitted buffer blocks later committed buffers. Abandoning an
uncommitted object cancels that ticket. At most 64 execution slots may be
reserved; `commandBuffer` blocks at the configured capacity until an actual
execution finishes or an uncommitted object is abandoned. Each buffer accepts
at most 64 ordered compute/synchronization operations and 64 handlers of each
kind, with 31 public buffer slots per dispatch. Only one compute or blit encoder
may be open, and command buffers are single-use.

The current native submission RPC returns only after actual fence completion,
result consumption and cleanup. Accordingly, Scheduled is reported late, only
after all native dispatches succeed; an Error releases scheduling waiters
without asserting a successful schedule. Zero-work buffers perform no GPU
call. The concurrent callback executor runs scheduled handlers before completed
handlers for each successful buffer, outside internal locks. On failure it runs
completed handlers with the actual error. The GPU worker advances the FIFO
and releases its execution slot before invoking callbacks, so a handler may
create the next buffer even on a capacity-one queue or wait for a later buffer.
The completed object's lifecycle survives slot reuse. Scheduling waits also
wait for scheduled handlers to return; completion waits include all completion
handlers. A scheduled handler cannot wait for its own scheduling handlers, and
no handler may wait for its own completion. A completed handler may query/wait
for its already-finished scheduling handlers. Waiting from the GPU worker is
rejected. Handler exceptions
are logged without fabricating a different GPU outcome.

Command objects currently support direct buffer compute dispatches, serial
buffer barriers and ordered Managed buffer synchronization. Unretained
resources, compute/blit-pass descriptors, other blit operations, render
encoders, drawables, textures, samplers, argument buffers, indirect commands,
events/fences, dynamic threadgroup memory, counters, residency sets and inline
`setBytes` storage are explicitly unsupported. Timing getters also reject the
request because the current RPC supplies no real GPU timestamps. Factories
return nil where the public API permits it; unsupported void selectors raise
`NSInvalidArgumentException`. This is a bounded compute component, not a claim
of general Metal or a system-discovered driver.

The additional inherited encoder/compute-pass declarations were read from the
actual SDK into `build/metal-command-sdk`, with hashes verified and no remote
files created. `build/command-lifecycle-local-3` validates the corrected pure
host ticket and lifecycle bookkeeping (55,881 checks each in strict and UBSan
builds), including capacity-one callback reentry, detached callbacks after slot
reuse and all 576 enqueue/commit ordering permutations. The earlier report
remains historical evidence of the superseded callback-held capacity policy;
the second report predates the separate scheduled-handler completion predicate.
`build/native-compute-command-final/report.json` now records strict native ARC
compilation of all three actual command protocols, the real eleven-object
runtime link, and 55,881 native ASan/UBSan host lifecycle checks. All 17 commands
succeeded without diagnostics. The nine earlier objects are byte-identical;
six exports were hash-verified and the 42,435,464-byte peak stage was removed.
The new runtime dylib hash is
`c128ccb0c12ad50c0501cc98f3014a2e2c41d351b7f32133ddf70e86063276b0`.
This native validation constructed no Metal object and executed no GPU work.
It predates Managed storage/ordered synchronization and the public buffer-index correction: current function
reflection, pipeline resource admission and encoder arrays accept indices
0 through 30. The native ABI's separate 32-resource limit is unchanged. The
three affected Objective-C objects were rebuilt in the later Managed and full
Device builds described above; the earlier command artifact remains intact.
Actual callback/thread behavior and target compute submission through the
Metal command objects remain unverified until the authentic device frontend
can construct them on the target.

The blocking capacity behavior and release of retained command resources follow
Apple's [commandBuffer contract](https://developer.apple.com/documentation/metal/mtlcommandqueue/makecommandbuffer%28%29?language=objc).
The separate wait predicate preserves the documented requirement that
[waitUntilCompleted](https://developer.apple.com/documentation/metal/mtlcommandbuffer/waituntilcompleted%28%29)
also waits for completion handlers to return.
[waitUntilScheduled](https://developer.apple.com/documentation/metal/mtlcommandbuffer/waituntilscheduled%28%29)
likewise waits for the scheduled-handler list, independently of completion
handlers and execution-slot reuse.
