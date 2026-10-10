# Runtime Metal source compilation: measured prerequisites

Audit date: 2026-10-11. This is an architectural source audit and a proposal for
bounded framework inspection. Later sections record actual read-only framework
captures run by the root agent. No compiler method, Metal object or GPU was
invoked for this audit. The current frontend accepts supported
actual AIR/metallib inputs; `newLibraryWithSource:` still returns an explicit
error. The completed offscreen render build does not change that distinction.

## What the reference implementations actually do

The pinned NVIDIA repository is commit
`508f3d2496e78a9640d59128a56ddb5a53061606`.
Its `NVMTLDevice` derives from `MTLIOAccelDevice` and runs that class's accelerator
initializer. Its source-library method invokes the superclass implementation,
then asks the actual returned Apple library for `libraryDataContents`. It copies
those container bytes into its own `newLibraryWithData:` route. It does not
contain a replacement MSL frontend in those methods. The asynchronous method
performs the synchronous compile first and schedules the handler afterward.
See [device base and initializer](../.research/nvidia/plugin/NVMTLDevice.m#L133),
[initializer](../.research/nvidia/plugin/NVMTLDevice.m#L344), and
[library bridge and source entry points](../.research/nvidia/plugin/NVMTLDevice.m#L985).

The optional NVIDIA vendor-compiler category is a different mechanism. It
constructs a private `MTLCompiler` using vendor target data, an NVIDIA plugin
path, cache identity and compiler flags. Its device `compiler` override falls
back to the superclass when that opt-in path is disabled. Neither its vendor
target structure nor its numeric architecture/compiler values establish an AMD
source-compilation contract. See
[vendor compiler selection](../.research/nvidia/plugin/NVMTLVendorCompiler.m#L17)
and [actual compiler initializer call](../.research/nvidia/plugin/NVMTLVendorCompiler.m#L231).
These PolyForm-Noncommercial files were studied; their implementation was not
copied into HackyNavi.

Cobalt64 is pinned at `d6c9aedf8e6526a39e349b5886cbc8c7ddf77f65`.
Its device also derives from `MTLIOAccelDevice`; it uses actual Apple function
`bitcodeData` for its downstream translation. Its source comments report a nil
`_MTLDevice compiler` in its environment and a Core Image environment workaround.
Those comments are reference-project observations, not measurements of the
HackyNavi device. The environment workaround is excluded here. See
[base declaration](../.research/cobalt64/tools/native/navi48metal/Navi48Device.m#L73),
[function AIR read](../.research/cobalt64/tools/native/navi48metal/Navi48Device.m#L1994),
[Core Image observation](../.research/cobalt64/tools/native/navi48metal/Navi48Device.m#L1928)
and [pipeline compiler limitation](../.research/cobalt64/tools/native/navi48metal/Navi48Device.m#L6197).

The successful actual framework capture below now narrows this distinction:
both bases resolve source-library methods to the **same** `_MTLDevice`
implementation on the measured mini. Source compilation must be traced
separately from vendor pipeline compilation; the base-class difference alone
does not rule out reusing it.

## Our existing boundary

`HNMetalDevice` is a genuine `_MTLDevice` subclass with independently owned native
Context/resources. It does not initialize `MTLIOAccelDevice`'s vendor resource
pools or client state. Consequently, copying a supercall from a different base
class is insufficient evidence that the source compiler will work here.

The current synchronous and asynchronous
[source-library methods](../userspace/metal/native/HNMetalDevice.mm#L581)
report unsupported. `getCompilerConnectionManager:`, `featureProfile`, and
`targetDeviceArchitecture` are explicitly unsupported
[SPI methods](../userspace/metal/native/HNMetalDeviceSPI.inc#L115).
These are candidate dependencies to inspect, **not a claim that the source
method calls all of them**. The actual framework call graph must settle that.

The retained native metadata already identifies `_MTLDevice` implementations for
`getCompilerConnectionManager:` and `targetDeviceArchitecture` in
`build/metal-capability-layout/capture.json`. The actual manager getter encoding
includes the C++ structure layout, whereas the required SPI protocol uses an
opaque forward structure. Neither encoding proves its ownership, allocation or
threading behavior. No raw layout or fake manager should be supplied.

An actual container returned by the runtime compiler could enter our existing
[immutable HNMetalLibrary initializer](../userspace/metal/native/HNMetalLibrary.hpp).
That initializer copies and validates the executable metallib, and the existing
function creation path independently translates real AIR with the owned explicit
tool configuration. This is the smallest existing integration seam. Returning an
Apple library directly would bypass our actual function/device ownership.

## Smallest credible implementation contract

1. Establish that the exact system framework on macOS 15 can compile source to
   an executable AIR container for this real device without a fabricated vendor
   profile, environment changes, another GPU, or an Xcode command-line tool.
   A class/method being present proves only metadata availability. The source
   compiler service, device callbacks and any required initialization still need
   their actual contracts measured.
2. Validate and retain bounded source and a copied `MTLCompileOptions` before
   asynchronous work. Initially support executable libraries with no external
   dynamic libraries, install name or undefined-symbol dependency. Preserve
   actual requested language/math options only after their downstream semantics
   are verified; never silently rewrite them. The actual SDK options include
   macro dictionaries, language version, math mode/functions, invariance,
   optimization, symbol visibility, threadgroup constraints and logging. See
   [captured MTLLibrary.h](../build/metal-discovery-prerequisites/sdk-headers/MTLLibrary.h#L291).
3. Invoke only a typed, verified framework contract. Obtain a real library or
   propagate the actual compiler diagnostic. A nil library with no diagnostic
   must become an explicit error. Keep the compiler/library owner alive until
   output bytes are copied. Verify `libraryDataContents`' actual return encoding,
   object type, lifetime and executable container format before calling it.
4. Bound and copy the actual returned container, then use `HNMetalLibrary` with
   the same admitted device and explicit compiler tools. Compare function names,
   stages and requested source entry points against the parsed output. Successful
   source compilation must not imply that every resulting shader is within our
   compute or graphics admission profile.
5. For the asynchronous public method, retain a copy of the handler and inputs,
   execute once outside device/Context locks, and deliver exactly one result or
   error. No callback should depend on a fabricated GPU completion; source
   compilation is CPU/service work.

Use an actual fresh source program and a changed source program in a subsequent
authorized compiler-only test. Require changed actual AIR/function data, valid
container ownership after the Apple result is released, and a genuine syntax
error path. Such a test is distinct from GPU submission. None has been run by
this audit, and an installed developer toolchain must not be used to conceal a
missing system-runtime route on the user's machine.

## Bounded next measurement

Prepared script:
[`scripts/measure-metal-runtime-compiler.py`](../scripts/measure-metal-runtime-compiler.py).
It reuses the previously reviewed project initializer image reader, with this
narrow selection:

- `_MTLDevice` and `MTLIOAccelDevice`: source-library sync/async entry points,
  data-library entry point, compiler/connection-manager and architecture getters,
  and the public new-compiler entry point, if present.
- Loaded classes with explicit `MTLLibrary`/`MTLCompiler` prefixes (including
  leading-underscore variants), plus exact function/internal-function and target
  architecture class names. This excludes the broad family of unrelated
  function-descriptor classes. Metadata includes exact
  `libraryDataContents`, `bitcodeData`, and target-data compiler initializer
  selectors when present. Missing loaded classes are recorded as missing, not
  treated as proof that no system implementation exists.
- Only complete functions whose actual `LC_FUNCTION_STARTS` bounds fit 1 KiB
  each. At most 16 KiB code and 128 KiB JSON output. Oversized or unbounded code
  is explicitly skipped; no inferred body or call behavior is recorded.
- Actual OS/build, method types, image offsets/UUIDs and header/load-command
  digests, plus hashes of captured code. This does not copy or hash bulk system
  frameworks. The known mini Metal UUID is checked before reading its code.

The script uses runtime metadata APIs only: no Objective-C message send,
`alloc`/`init`, Metal enumeration, user-client open, compiler invocation, GPU
submission or remote file creation. Python/metadata bookkeeping allocations are
ordinary host memory, not constructed Metal objects. It neither installs tools
nor downloads anything. Local Python syntax compilation passed; remote execution
awaits the root agent's review and lane ownership. The first root-run capture
stopped at the original broad 48-class selection bound and returned no report;
its evidence remains in `build/metal-runtime-compiler-capture-1`. The corrected
selection preserves all byte/count limits and reports skipped counts explicitly
if any bounded class or method catalog is incomplete.

Root can feed the saved script to its normal batch-authenticated SSH connection
and save stdout/stderr locally in a fresh evidence directory. Any later
follow-up should name only the direct callees or selectors revealed by this
capture, retaining the same no-object/no-GPU boundary until a real source
compiler contract is established.

## Actual capture 2: source builder is separate from `compiler`

The root agent ran the corrected reader on mini macOS 15.8 build 24H22, Metal UUID
`c3efbabe-6b06-33ab-bc45-43fe86297aa1`. The 49,083-byte result is
`build/metal-runtime-compiler-capture-2/stdout.json`, SHA-256
`1947c6ec62a39a14767378081da6c04d7e63af263a367bc509e1ff179e7a1c2a`.
It found exactly five selected classes, skipped no catalog entries and captured
1,589 code bytes. No Metal objects or compiler calls occurred.

The actual synchronous source method at Metal+`0xfd26a` has encoding
`@40@0:8@16@24^@32` and occupies 356 bytes. The asynchronous method is 81 bytes,
encoding `v40@0:8@16@24@?32`. Both load the base-owned field at offset 464 and
call Metal+`0x150e30`, passing different synchronous flags and completion blocks.
Neither captured body calls the `compiler` getter. The actual `compiler` and
`targetDeviceArchitecture` getters themselves return nil in eight bytes each.
This does not establish whether the shared builder later requests device hooks.

Previously retained constructor evidence already shows `_MTLDevice init`
allocating and constructing `MTLLibraryBuilder`, then storing it at offset 464;
the retained deallocator destroys that same owner. The offset is evidence for
identifying the path only. Production integration should call the real
superclass API and never access or reproduce the private field.
See `build/metal-public-accelerator-route/constructor-references.json` and its
byte-verified disassembly.

The actual `_MTLLibrary libraryDataContents` method is 22 bytes, type `@16@0:8`.
It loads its library-data field via the runtime ivar offset and dispatches through
that object's C++ vtable. This proves an Objective-C object return contract, not
that every result is an `NSData` instance or that borrowed bytes outlive the
library. The future bridge must keep the actual Apple library alive and validate
and copy the returned object/container before releasing either.

Local disassembly uses the pinned LLVM reader and rechecks captured hashes and
instruction bytes. The synchronous source method ends with one extra zero byte
after its final call; the report explicitly preserves that byte as a data suffix
instead of pretending that the decoder accepted it as an instruction.

The prepared
[`measure-metal-runtime-compiler-followup.py`](../scripts/measure-metal-runtime-compiler-followup.py)
checks those four original method hashes before reading the exact shared builder,
its source completion blocks, the error helper and the already identified builder
constructor/destructor. Only the main builder may exceed 1 KiB, up to 8 KiB if
its observed complete function extent requires it; the overall 16 KiB code and
128 KiB output limits remain unchanged. Eighteen code-derived references and
library/compiler ivar metadata identify the remaining actual callbacks and
lifetime contracts. No source compile or object construction is requested.

## Actual capture 3: builder arguments and result ownership

The root agent ran the follow-up on the same measured system. Its 43,229-byte
report is `build/metal-runtime-compiler-capture-3/stdout.json`, SHA-256
`5a8f25aafbbe8807679378f0ddce2aeb85d3586de148507281d7542a4c1fbbb7`.
All selected complete functions fit their bounds; 9,324 code bytes were
captured. The actual source builder is 8,001 bytes, not a one-instruction
forwarder. Its full byte-checked local disassembly is retained beside the
capture. Explicit zero-byte suffixes after the four affected function bodies
are preserved as data, not interpreted as additional execution.

The builder receives the real device separately from its own owner, validates
the input objects, processes real compile options, and constructs the source
request. Three device calls use two exact selector references: Metal-relative
`0x33ddbb10` once and `0x33ddbb18` twice. It also passes that device to
`0x142222`, whose returned value participates in default language/version
selection. The final request goes through `0x150bb6`. The unresolved names and
semantics of these exact calls are the remaining prerequisite; their presence
does not authorize a guessed vendor profile, architecture or connection manager.

The synchronous callback explicitly retains the real library and real error in
the caller's result storage. The public synchronous method returns the retained
library and transfers an error through the supplied error pointer; if both
library and warning exist, it logs and clears the warning. The asynchronous
callback forwards the actual result/error to the actual supplied block and
performs the same warning filtering. These are host compiler results, with no
GPU completion identity involved. The synchronous path additionally queries
`isCollectingLibraries`; this existing optional collection path is separate from
the source compiler request.

The actual `_MTLLibrary` initializer owns the passed device, library-data object
and cache container. Its deallocator releases those corresponding owners. The
private `libraryDataContents` selector remains a typed Objective-C-object return
through the data object's vtable. A prospective bridge can require an actual
`NSData` result at runtime and copy it while both the Apple library and the
returned object remain strongly held. This avoids depending on a private C++
data layout or assuming that the pointer is independently owned. The actual
returned object's type and executable container bytes still require a subsequent
authorized source-compilation test; this capture invoked no compiler.

The next reader,
[`measure-metal-runtime-compiler-request.py`](../scripts/measure-metal-runtime-compiler-request.py),
binds all selected references to the exact previously captured instruction bytes.
It resolves the source/options/device selectors and the specific lower callees,
then captures only the two lower functions and the two device query bodies, each
within 1 KiB. It rechecks the prior full-body hashes without publishing the large
builder again. The output remains bounded to 128 KiB; no Objective-C messages,
Metal objects, source compilation, GPU work or remote files are requested.

## Actual capture 4: a real feature-profile dependency

The next root-run report is `build/metal-runtime-compiler-capture-4/stdout.json`,
44,618 bytes, SHA-256
`edbc8a47b7efe87fbdccaddbfeb8d4faf9df957bb7df9e7ed8bb7563e79cb0c6`.
Its four new complete functions total 507 bytes; all disassembled bytes match
the retained capture. The two outer device queries are precisely
`workaroundNewLibraryWithSourceReplaceFastMathWithRelaxedMath` and
`workaroundNewLibraryWithSourceImplicitInvariantPosition`. They read the actual
base-owned flags initialized by the real superclass. They are not requests for
an invented GPU feature family.

The normal path of `_MTLGetMTLCompilerLLVMVersionForDevice` is different: it
asks the real device for `featureProfile`. The exact selector reference
`0x33dda6b8` was already resolved in `build/metal-device-hooks/final.json`.
The helper selects compiler version 32023 for profile values 0 through 6 and
10000 through 10002; other values enter a separate default-version initializer.
Its alternate policy branches may query another device selector or use a
process-wide override. Those branches do not authorize changing the process
environment or returning a borrowed feature profile from HackyNavi.

This establishes an actual production blocker: our `featureProfile` method
explicitly rejects the unknown contract. Choosing a numeric value merely because
it selects the wanted source compiler would falsely assign a private device
profile. Earlier measured uses of the same method initialize concrete limits
and GPU-family state. It is not simply a free-standing compiler-version field.
See [the existing hook contract](metal-device-hook-contract.md).

The lower request function performs an actual cache lookup, then either enters
the real cache-miss routine at `0x14f17a` or waits/notifies through the existing
cache completion. Its callback address is `0x150c91`. The final proposed reader,
[`measure-metal-runtime-compiler-service.py`](../scripts/measure-metal-runtime-compiler-service.py),
captures those two exact paths together with every observed compiler-version
policy branch and the relevant device-selector metadata. Only the actual cache
miss may use up to 8 KiB; every other new function is limited to 1 KiB. This
batch is intended to settle the remaining service/selection preconditions, not
to start an unbounded framework audit. If a valid profile contract remains
unavailable, the source method must retain its explicit unsupported result while
the already implemented actual AIR/metallib route remains available.

## Final measured blocker and future seam

The final root-run report is `build/metal-runtime-compiler-capture-5/stdout.json`,
30,302 bytes, SHA-256
`9418f14f7bf40ddeb2701f37d15b1e2d72bed7c5a30df13cda57e33702425569`.
Seven new complete functions total 3,238 bytes. The actual cache-miss routine is
2,870 bytes and its public-result callback is 129 bytes, including seven trailing
zero bytes explicitly retained outside decoded instructions. All captured code
and execution-report hashes were checked locally.

The cache-miss path calls `_MTLGetMTLCompilerLLVMVersionForDevice` again at
Metal+`0x14f625`, then passes the selected version and real device to the lower
request-provider helper at `0x150398`. Therefore supplying an explicit
`MTLCompileOptions.languageVersion` does **not** bypass the real profile
requirement for a new source compile. The request's compiler version and the
source's MSL language version are separate inputs.

The alternate version-selection initialization is guarded by the actual
`_MTLIsInternalBuild` function. The base `llvmVersion` method returns 3802.
An override of that method alone is not a normal-path replacement for
`featureProfile`, and relying on an internal-build or process-global override
would not provide an ordinary application compiler path. Neither an arbitrary
profile value nor another vendor's target data is justified by these captures.

The cache-result callback creates a library only when actual library data
exists, passes the real library/error to the retained completion, then releases
its temporary result after the callback returns. This agrees with the outer
synchronous callback's explicit retain. A future bridge should therefore:

- Invoke the real superclass source-library method only after the missing
  device-profile and request-provider contracts are satisfied.
- Keep the returned Apple library strongly held, check the exact
  `libraryDataContents` method type, require an actual bounded `NSData`, and
  copy its actual container while both owners remain alive.
- Pass that immutable copy to `HNMetalLibrary` with the same admitted device
  and explicit compiler tools; propagate the actual compiler error on failure.
- Preserve copied options, bounded source and exactly-once callback ownership
  for async calls, without holding native Context or command-queue locks.

**Current decision:** no production source-compilation change. The earliest
unmet requirement is a truthful `featureProfile` contract for this limited
Navi 31 implementation. The lower request-provider/service availability and
returned container still need a real compiler-only test after that prerequisite
is solved; they are not claimed verified. We stop expanding metadata here.
The existing source-method error remains accurate, and actual supported
AIR/metallib input remains the implemented route. No compiler, Metal object,
GPU operation, alternate device or environment change occurred in this audit.
