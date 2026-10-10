# macOS 15 Metal discovery: exact next prerequisites

This is the focused follow-up to [the native integration audit](metal-native-integration.md),
dated 2026-10-11. It does not re-audit the compiler or claim a working Metal
device. The native reusable VM/queue work has now compiled and passed its
portable protocol tests; its two-job GPU probe still requires target execution.
The new reusable userspace profile is a separate, ongoing implementation.

The smallest credible discovery route is a real accelerator-family bridge
attached to the existing hardware owner, followed by a Metal bundle whose real
base initializer and actual HackyNavi backend both succeed. The public kernel
headers support the owner/nub/client routing work today. They do **not** supply
the private accelerator-family declaration or its initialization contract.
Publishing a nominal Metal service before those contracts work would not close
that gap.

## Additional evidence, beyond the previous audit

The NVIDIA source remains pinned to
`508f3d2496e78a9640d59128a56ddb5a53061606`, Cobalt64 to
`d6c9aedf8e6526a39e349b5886cbc8c7ddf77f65`. The new bounded measurement is
`build/metal-discovery-prerequisites/sdk-runtime.json`, produced by
[`scripts/measure-metal-discovery.py`](../scripts/measure-metal-discovery.py).
It ran on the mini's actual **macOS 15.8, 24H22, x86_64**, with SDK26.2. The
user's reported 15.8.1 build remains a separate compatibility target. It
created no remote files, enumerated no GPUs, constructed no device, opened no
user client and invoked no GPU function. It only loaded the named system
libraries, inspected metadata, resolved named exports and read bounded named
headers/plists. The mini lane was released immediately afterward.

| New measured fact | Implementation consequence |
|---|---|
| `MTLIOAccelDevice` is 54,368 bytes; its superclass `_MTLDevice` is 656 bytes | An Objective-C subclass must use the runtime's nonfragile superclass layout. Do not copy a private C struct or hard-code these sizes into a class declaration. |
| `lazyInitialize` is declared by `MTLIOAccelDevice`, encoding `c16@0:8` | On this x86_64 runtime the result is a signed char/BOOL, **not void**. Pinned Cobalt's `- (void)lazyInitialize` declaration and implementation cannot be transplanted. |
| `initLimits` is inherited from `_MTLDevice`, encoding `v16@0:8`; `llvmVersion` is `i16@0:8` | These are actual available selectors, but existence does not establish the values or state their bodies require. |
| `featureProfile` is absent from both inspected base classes | A subclass's feature-profile value and the consumers that require it need independent validation. Cobalt's `10000` is not HackyNavi capability evidence. |
| `deviceRef`/`sharedRef` return opaque `__IOAccelDevice*`/`__IOAccelShared*`; `acceleratorPort` returns unsigned32; `registryID` returns unsigned64 | The base object has actual family device/shared state. The accessor types are now measured; their ownership and initialization sequence are not. |
| Named exports `IOAccelDeviceCreate`, `IOAccelDeviceGetConfig`, `IOAccelDeviceGetName`, `IOAccelSharedCreate`, `IOAccelContextCreate` resolve | These are leads for the initializer call graph. No signature was inferred or function called. Some guessed `Destroy`/event-machine names did not resolve and must not be invented. |
| IOAcceleratorFamily2 bundle version is `485.2`; neither named family has a standalone executable at its normal kext path | The missing private kernel ABI cannot be recovered by reading that nonexistent standalone binary. A later bounded investigation must identify the relevant collection entry or obtain independently verified declarations for the exact OS build. No kernel collection was copied here. |

Selected ivar offsets are recorded as diagnostic metadata only, including
`_deviceRef`, `_sharedRef`, `_acceleratorPort` and `_acceleratorService`. No
production code reads or writes those offsets. Export lookup through a library
handle can resolve a dependency; it does not establish which image defines the
symbol or a callable C prototype.

The actual SDK's kernel `IOAccelerator.h` declares the public
`IOAccelerator : IOService` base and three static accelerator-ID functions.
`IOAccelClientConnect.h` declares the public surface client and marks the
remaining client types private; it does not define the modern device/shared
client contract. These exact header hashes and selected lines are captured.
SDK26.2 header presence is build evidence, not proof of a private 15.8 layout.

## Kernel family contract still to establish

NVIDIA's reconstructed
[`IOGraphicsAccelerator2.h`](https://github.com/nullmoth/nvidia-macos-driver/blob/508f3d2496e78a9640d59128a56ddb5a53061606/kexts/NVRM/accel/re/IOGraphicsAccelerator2.h)
describes the chain `IOGraphicsAccelerator2 -> IOAccelerator -> IOService`, a
3,544-byte accelerator and 84 new virtuals. Its comments expressly say the
return types were inferred, rather than recovered. Its 17 pure hooks are:

- Stamp/event/task/configuration: `getStampMemory`, `newEventMachine`,
  `createUserGPUTask`, `populateAccelConfig`, `configureDevice`,
  `teardownDevice`, `createKernelGPUTask`.
- Factories: `newDisplayMachine`, `newGLContext`, `newCLContext`, `newSurface`,
  `new2DContext`, `newVideoContext`, `newSysMemory`, `newVidMemory`,
  `newResource`, `newMemoryMap`.

This is a concrete investigation list, **not a verified macOS15.8 vendor ABI**.
Implementing every hook as a null/true return is not a driver. For each hook
reachable during family start, device/shared creation, or our supported compute
path, establish its actual signature, allocation/retain ownership, failure
behavior and required initialized fields. For hooks outside the supported
path, establish whether the caller safely handles unsupported/null. In
particular Cobalt reports a display-pipe caller that stores through an
unchecked factory result; a blanket null-factory policy is unsafe.

The source's reconstruction scripts, binary inputs and referenced
`re/DRIVER-CONTRACT.md` are not present at the referenced paths in this pin.
Cobalt's accelerator includes generated `IOAccelFamily2_decl.h`, layout gates
and trampolines from `tools/native/ioaccel-layout/generated`; that generated
directory is absent from its pinned checkout. Its source lists a different
get-stamp virtual-slot index from NVIDIA. A slot number without the actual
base-class build/layout is not portable evidence.

A local real-header compiler experiment checked one apparent source hazard:
the NVIDIA declaration omits inherited `start`, but calls
`IOGraphicsAccelerator2::start`. With the actual `-fapple-kext` compilation
mode, the generated LLVM loads the function through the **family vtable**
instead of directly calling IOService. Therefore ordinary C++ qualified-call
reasoning would be misleading here; this audit does not report a bypass defect.
`reference-super-call-retry.json` binds the successful experiment. Its vtable
offset is a compiler/header observation, not a runtime slot verification.

The following private details remain required before a family bridge can be
loaded:

| Boundary | Exact unresolved information | Acceptance evidence |
|---|---|---|
| Kernel class ABI | Family class sizes/alignment, complete inherited/new virtual order and actual return/argument types for every implemented factory; metaclass dependencies | Bind declarations and compiler vtables to the exact target family/kernel build; verify class sizes and inheritance before constructing subclasses. A runtime size check alone cannot prove signatures or slot order. |
| Family initialization/config | Required event machine, task and stamp setup; `IOAccelConfig` layout/units and limits; start/stop/free ordering and partial-start cleanup | Trace the actual family start/config/client call paths. Do not use Cobalt's raw `this+0xc88` or NVIDIA's inferred fields. Incorrect IOSurface limits can affect other clients. |
| Provider topology | Whether each family method expects immediate IOPCIDevice or can accept a software nub; display-machine traversal | Verify all reachable casts/provider walks. Cobalt explicitly substitutes the PCI provider for its display-machine start, demonstrating this is a real contract, not an automatic property of IOService. |
| Accelerator enabled state | Source-backed headless enable/disable and event-progress lifecycle | NVIDIA observes allocations waiting for `enableAccelerator`; verify the 15.8 behavior and pair it with real engine lifetime. Never set a private flag by offset or imply display ownership. |
| Standard user clients | Actual open type IDs, four-/five-argument dispatch route, device/shared linkage, required selectors/output fields and mappings during base initialization | Follow the actual userspace initializer and kernel family clients. Prefer real family standard clients after correct family initialization. Do not emulate success by returning zero-filled opaque replies. |
| Failure/teardown | Which constructor failures permit ordinary release; shared mappings and clients destroyed on failed base/backend init; live owner removal | Test explicit partial-initialization paths after the ABI is known. A nil return must not leak clients or free GPU-visible storage before real retirement. |

NVIDIA's separate reconstructed device/shared-client headers list 10 and 21
selectors respectively, with several opaque payload layouts. They are useful
cross-checks, but do not establish the installed initializer's required subset,
client type IDs or payload semantics. Cobalt's later-OS initializer comment
about calling super before an admission failure also needs 15.x verification;
it cannot be treated as a universal ARC rule.

## Smallest backend-connected service and bundle path

The proposed topology preserves one hardware owner:

```text
Existing HackyNaviDevice / FunctionGroup / Engine / VM / GraphicsQueue
  -> retained public IOService child for accelerator attachment
    -> verified IOGraphicsAccelerator2 subclass
      -> standard family device/shared clients
      -> separately typed HackyNavi native client, bound to that same owner

Apple's Metal loader -> HackyNavi bundle principal class
  -> actual MTLIOAccelDevice initializer with the accelerator port
  -> exact owner-bound native session -> compiler products -> actual queue
```

This is the common concrete integration pattern visible in both references;
the audit has not proved it is the only possible private provider route. There
must be no second PCI match that opens the GPU, initializes another VM domain
or independently resets the engines. An attached software nub can be built
with the public IOService API now, provided it has explicit retained owner
identity, registration only after complete setup, shutdown admission closure,
and teardown without owner locks held across matching/termination callbacks.
Cobalt's publish path deliberately drops its lock before `registerService`,
retaining the nub across that call; this is useful architecture, not code to
copy unreviewed.

There are two concrete mismatches in our current client path to resolve:

1. `HackyNaviUserClient::start` currently requires its immediate provider to be
   `HackyNaviDevice`, and that device creates/attaches the client itself. Simply
   calling it with the accelerator provider will fail. Factor an explicit
   owner-plus-provider construction route using the existing task checks,
   copied-message protocol, session creation and cleanup. Validate both retained
   identities and their live attachment; do not accept an arbitrary kernel
   pointer or follow an unbounded provider chain. Keep standard family client
   types in the actual family's route and handle only our explicit native type
   in this bridge. The current five-argument IOService override does not prove
   the family's four-argument override is correctly intercepted.
2. `Client::openProfile` currently matches `HackyNaviDevice`, selects its registry
   ID and requires Hello to return that same ID. A Metal accelerator registry
   ID will be different. Define endpoint-versus-hardware-owner identity
   explicitly when adding an accelerator route. Verify the selected endpoint,
   retained native owner and negotiated session before exposing the device;
   never accept the first matching GPU or disable the existing ID check. An
   initial unsandboxed integration test can use the exact verified owner
   service, but sandboxed Metal application access is a separate measured
   requirement, not something a service name or reference comment proves.

The real bundle can use a measured, ivar-free forward declaration of
`MTLIOAccelDevice`, compiled for Objective-C's nonfragile ABI, and expose an
`NSPrincipalClass` matching its loader properties. It must call the actual
base initializer, open the real owner-bound backend, and return nil on either
failure. Do not redeclare `_MTLDevice` storage, patch its private ivars, borrow
Cobalt's feature-profile/LLVM values, or advertise a feature family from a
successful compute-only experiment. If `lazyInitialize` is overridden, its
15.8 signature must match the measured BOOL result and its actual base
contract must be established before use.

Bundle code for library/function/pipeline and command objects can progress
against the actual public Metal headers and existing owned compiler products.
Its work must reach the existing real native admission/submission/completion
path. Persistent buffers also need actual CPU/GPU visibility and map/unmap
semantics: the current copied staging interface is not itself a shared
`MTLBuffer.contents` mapping. Output shadows cannot stand in for GPU results.
Unsupported source compilation, resource types and rendering operations must
fail explicitly; no fake success/pipeline or fabricated completion is allowed.

## Practical task split and the next proof

- **Public owner/client bridge:** implement and real-header compile explicit
  owner/provider lifetime and endpoint binding; exercise failed starts,
  stopped owner, stale identity, close/death races and old native profile
  compatibility. Do not publish Metal plugin properties yet.
- **Private family ABI:** recover only the named family classes and reachable
  initializer/client paths for the exact 15.x binaries. Start with kernel
  collection entry metadata and bounded symbol/section extraction, not a full
  framework/filesystem dump. The current read-only capture resolved selector
  types but cannot settle this kernel work.
- **Objective-C compute objects:** implement real library/function/pipeline,
  buffers and command lifetime against the owned compiler and reusable native
  API. Preserve actual queue errors/retirement. This can proceed in parallel
  with the private-family investigation without pretending discovery works.
- **Integration:** only after both contracts work, package the actual bundle
  and accelerator personality, enable discovery in a controlled target test,
  verify normal Metal enumeration and matching registry identity, then execute
  two dependent GPU jobs with retained buffers and compare real output/guards.
  A device appearing in the list is one check, not GPU or WindowServer proof.

The immediate research blocker is specific: private family layout/config and
the standard base initializer's client/mapping sequence. It is not the absence
of a generic `MTLDevice` protocol declaration. The bounded runtime measurement
has already removed one wrong signature from consideration and identified the
precise binaries/classes needed for the next investigation.

## Source use and evidence

`build/metal-discovery-prerequisites/report.json` binds the document, script,
captures, compiler experiment and inspected reference files. No production
source, installed kext or previously issued diagnostic package changed. NVIDIA
implementation remains PolyForm Noncommercial and was not imported. Cobalt's
root MIT license does not validate its private ABI, later-OS assumptions or
placeholder paths; none of those implementations was imported here.
