# Public accelerator base with owned standard clients

The preferred next investigation is a real `IOAccelerator` subclass attached to
`HackyNaviDevice`, with our own type-5 and type-6 `IOUserClient` implementations.
The measured userspace initializer does not require our kernel service to use
the private `IOGraphicsAccelerator2` vtable. This route can avoid that private
class layout while retaining the actual Metal initializer and the existing
hardware backend. It is a viable design direction, **not yet a proved discovery
implementation**: the normal Metal loader and several initializer dependencies
still require bounded measurement.

This is a design review of existing captured evidence on macOS15.8 build24H22,
x86_64. It makes no additional SSH call, kernel-collection capture, user-client
open, device construction or GPU submission. The user's reported15.8.1 build
remains a separate compatibility check.

## What the existing measurements establish

| Boundary | Measured dependency | Consequence for this route |
|---|---|---|
| Public kernel superclass | The actual `IOKit/graphics/IOAccelerator.h` declares `IOAccelerator : IOService` and static accelerator-ID methods. Its compiled136-byte extent and266 virtual entries match the installed base. | This base can be compiled against the real header; no private3544-byte family declaration is needed to derive from it. |
| `MTLIOAccelDevice initWithAcceleratorPort:` | Takes a service port, calls `IOAccelDeviceCreateWithAPIProperty`, reads configuration/registry identity, and invokes real initialization. | The kernel C++ object is not exposed to userspace. The observed crossing is the service/client protocol. |
| `IOAccelDeviceCreateWithAPIProperty` | Opens type5 with `IOServiceOpen`, calls selectors9,2,0,7 through `IOConnectCallStructMethod`, then consumes actual replies. | A custom type5 client can implement this boundary using public IOKit mechanisms if it implements the complete required behavior. |
| `IOAccelSharedCreate` and dirty-ring setup | Open type6 on the same accelerator service, then selectors9 and10. Successful setup immediately reads returned mapped memory. | A custom type6 client is possible, but a struct containing arbitrary addresses or zero placeholders cannot satisfy it. |
| Actual Apple type5 kernel implementation | `IOAccelDevice2` calls accelerator slot271 for stamp storage and reads private family/event fields. | These are dependencies of Apple's implementation of that client. They do not prove that a different client implementing the same userspace protocol must inherit the same private classes. |
| Our current compute client | Requires the immediate provider to be `HackyNaviDevice`; holds the real owner/session and validates its own client type. | Keep that endpoint intact. An accelerator child is a separate identity, not a reason to blindly forward or relabel the existing client. |

In the complete captured initializer bodies and the six followed IOAccelerator
userspace helpers, no `IOObjectConformsTo` check for
`IOGraphicsAccelerator2`, private kernel vtable access or registry class-name
comparison occurs. Their direct imported calls and exact selector/byte counts
are preserved in `helper-disassembly.json` and `helper-references.json`.
Ordinary service opening still invokes the provider's kernel `newUserClient`
and normal IOKit access checks; a custom provider would own that implementation.

This statement is deliberately limited to captured code. The framework's device
enumerator/plugin loader was not captured. Neither were `_MTLDevice::init`,
`updateGPUSelectionProperties`, all pool constructors, their release paths, or
the cold diagnostic paths of every helper. Therefore the existing evidence
cannot prove that **all** normal discovery paths lack a private class-name or
other family requirement. The pinned NVIDIA and Cobalt projects both choose a
private-family subclass; that choice is not proof that it is necessary.

## Required real behavior remains

Using the public superclass reduces the class ABI surface. It does not reduce
the standard clients to canned initialization replies.

| Contract | Known evidence | Still needed before implementing a successful reply |
|---|---|---|
| Type5 selector2 event reply,600 bytes | Real descriptor retained by client; unique read-only mapping into its task; mapped base plus byte offset; count/name/offset fields; immediate userspace stamp reads. Map is released before descriptor. | Exact stamp meanings, bounds/count limit, initial state, allocation, advancement, wrap comparison, waits, reset/error semantics and relationship to actual queue completion. Concurrent close/termination must revoke operations before unmapping. |
| Type5 selector0 config reply,64 bytes | Initializer consumes config/device bits, memory values, accelerator ID and shared-memory size. Config bit0x1000 changes bookkeeping. | Complete consumed field meanings and flags. Values must derive from actual owned hardware/storage/features. Public `createAccelID` is an available declaration, but its required lifecycle/usage must be established before assigning an ID. |
| Type5 selector9 API property | Actual initializer sends a16-byte API string; current kernel dispatch entry permits a variable output convention. | Exact setter validation/ownership and effect. The caller ignoring a status is not authorization to return success without doing the operation. |
| Type5 selector7 device/trace reply,24 bytes | This helper tolerates failure; success changes retained trace/device information. | Actual semantics before returning success. An explicit unsupported result is preferable until the caller's tolerated path and later uses are understood. |
| Type6 selector9 reply,16 bytes | Caller records two returned words; failure stores zeros but still returns a shared object. | Meaning, allocation and lifetime of those words, plus any subsequent association with other clients. Failure tolerance at creation does not prove later independence. |
| Type6 selector10 dirty ring,24 bytes | Caller immediately dereferences first returned pointer+4 as u32, subtracts1, retains all fields. | Header/ring layout, capacity/index arithmetic, producer/consumer roles, memory permissions, synchronization, resource identities, overflow behavior and teardown. Valid mapped bytes alone are insufficient. |
| Shmem/resource/storage pools | Actual lazy initializer constructs resource pools, shmem pools and a command-buffer storage pool. | Which constructors allocate immediately, further selector/memory-map calls, buffer formats, deallocation and error unwinding. The storage pool is explicitly required to be non-null. |
| Normal discovery and bundle creation | Reference personalities use `MetalPluginName`/`MetalPluginClassName`; actual `MTLIOAccelDevice` initializer exists and is measured. | Installed Metal loader's real match class, property filters, bundle loading, principal-class/factory checks and lifetime. Registry strings from another project are leads, not measured loader authority. |

A stamp or ring buffer is legitimate only when its protocol is implemented and
connected to the actual owner operations it describes. A mapped zero page, a
counter that advances without proven completion, or successful resource methods
that discard their inputs would remain incomplete even if initialization got
past the first read. A zero initial counter can be valid only after its actual
initial-state semantics have been established; zero itself supplies no proof.

## Concrete architecture to pursue

```text
IOPCIDevice
  HackyNaviDevice                         existing single hardware owner
    existing private compute client     real sessions/VM/queue/completion
    HackyNavi accelerator : IOAccelerator
      owned standard type5 client       event/config/API/identity contract
      owned standard type6 client       shared/dirty-ring/shmem contract
        actual Metal bundle/device      real MTLIOAccelDevice initializer
          retained native Context       existing compute backend
```

The accelerator child would retain the live owner and coordinate readiness,
stop and termination with it. It must not open the PCI device again or start a
second engine/GART/VM lifetime. Its `newUserClient` would admit only implemented
types, validate the caller/task, allocate its own client class, and unwind each
failed init/attach/start step. Standard-client memory must be owned descriptors
mapped only into the authorized task; output pointers must never be kernel
pointers or user-supplied addresses reclassified as owned memory.

The device factory must establish the exact accelerator-to-owner registry
relationship and then negotiate an actual native Context with that owner.
The accelerator registry ID returned by Metal and the owner registry ID in the
current native Context remain distinct. The existing private context-owner
interface for Metal objects preserves that distinction. Any required sharing
between standard clients and that compute Context needs an explicit retained
association and task validation, not only equality of process IDs or integers.

The kext will also need an actual IOGraphicsFamily dependency and resolved
public `IOAccelerator` imports when this production service is added; the
current diagnostic/native kext does not declare that dependency. No arbitrary
family version should be added from a reference project's plist. Normal
publication of Metal plugin properties should follow implemented initialization
and supported object paths, with truthful limits and failure behavior. A
private test can validate attach/close/lifetime earlier, but is not normal Metal
discovery evidence.

This compute route does not settle IOSurface/WindowServer/framebuffer clients,
textures/rendering, display presentation or compatibility with consumers that
specifically require Apple's private family objects. Those may introduce
additional contracts later; there is no reason to reconstruct their entire
vtable as a prerequisite to testing this narrower real route.

## Next bounded measurement and implementation order

1. Read the actual loaded-image code for `MTLCopyAllDevices` and
   `MTLCreateSystemDefaultDevice`, without calling either. Resolve their direct
   discovery helper targets using `dladdr`, actual function boundaries and
   image UUIDs. Follow at most the next four loader helpers, only if each is
   bounded to8192 code bytes and the selected batch totals at most24KiB. Read
   only referenced selector/class/property strings and pointer slots. Stop and
   report an uncovered edge when those limits are exceeded. This directly tests
   class matching and factory loading; it needs no kernel-collection read.
2. Independently follow actual `_MTLDevice::init`,
   `updateGPUSelectionProperties` and the captured shmem/storage-pool constructor
   targets, including failure/release edges. The existing capture identifies
   these calls; choose the next small batch from their real boundaries rather
   than dumping a framework. This determines the remaining reachable standard
   client surface before designing a type6 implementation.
3. Recover event/ring producer-consumer and cleanup semantics needed by that
   reachable surface. Prefer actual userspace reads/writes and protocol clients
   before further family reconstruction. Use the captured Apple client mapping
   lifecycle as evidence, while implementing our own public-IOKit ownership and
   teardown. Integrate counters only with actual completion/recovery proof.
4. Build the public-base child and its complete supported clients, then validate
   genuine task mapping, close/death/termination and backend binding before
   publishing the bundle. A real initial accelerator object, successful base
   initialization, real compute result and normal enumeration are separate
   checks; each should have its own evidence.

If step1 proves an exact private-family superclass check, record the precise
callsite/property and revisit the private subclass route. Do not spoof a class
name or patch/interpose Apple's device enumeration to evade it. If it instead
matches the public superclass and ordinary properties, keep the public-base
route and focus engineering on authentic client behavior and the real frontend.

Evidence: [initializer and helper audit](metal-initializer-contract.md),
[event/stamp mapping audit](metal-event-stamp-contract.md),
`build/metal-initializer-audit/public-base-layout.ll`, and the hash-bound review
in `build/metal-public-accelerator-route/review.json`. Earlier discovery notes
that proposed a private subclass are historical proposals; they do not override
this distinction between measured requirements and implementation choices.
