# Owned Metal capability state

This extends the frozen `metal-device-hook-contract.md` evidence. The selected
accessor capture is `build/metal-capability-layout/capture.json`: 75 complete
implementations, 1,869 code bytes, plus four explicitly absent methods. The
image remains Intel macOS 15.8/24H22 Metal UUID
`c3efbabe-6b06-33ab-bc45-43fe86297aa1`. No object was created, enumerated or
messaged; no client, policy function or GPU was invoked. The later 19,862-byte
SDK batch also resolves the sole common getter selector reference as `limits`.
Both batches ran from stdin and created no remote files.

## Implemented capability owner

`HNMetalCapabilities.hpp/.mm` captures information from the exact live reusable
`compute::Context`. It requires the actual negotiated v1.4 ABI, identity,
capabilities, page geometry and resource capacity. A ready snapshot cannot be
silently recaptured with another owner or epoch. It is immutable metadata;
actual allocation, execution and retirement still go through the real Context.
Closing that Context does not turn this snapshot into liveness evidence.

The owner exposes its native registry ID, epoch, system allocation budget,
VRAM capacity, page size and admitted compute limits. The maximum buffer size
is bounded by both the native allocation limit and the actual budget, rounded
down to the admitted allocation page. It does not report the system budget as
total physical RAM, CPU mapped memory or current allocation usage.

The limits storage is a complete owned anonymous POD, with the exact observed
const-pointer encoding:

```
r^{?=IIIIIIIIIIIIIIIIIIIIIIIIIIffIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIQ}16@0:8
```

Its layout is 26 uint32 fields, two float fields, 36 uint32 fields and one
uint64 field: 264 bytes, alignment eight. All field offsets have compile-time
assertions. An Objective-C compiler probe independently emits the same pointer
encoding. Sixty-three scalar getter bodies call `[self limits]`, then read
one known typed offset. The unnamed uint32 at offset 116 is explicitly zeroed
by the measured base `initLimits`; it remains named `reserved116`. The final
uint64 at 256 is established by the base initializer's actual `maxBufferLength`
call and store at self+264, with `limits` returning self+8. These are observations
used to declare the data ABI; production never copies those object offsets.

Only three limit fields are nonzero today:

| Field | Value and implemented source |
|---|---|
| `maxComputeBuffers` | 31 public Metal binding slots; the actual descriptor array rejects index 31 and above. Native capacity 32 does not enlarge this public bound. |
| `maxTotalComputeThreadsPerThreadgroup` | 640, the conservative occupancy bound for every currently admitted wave32 variant. Actual compiled variants and dispatch dimensions are still independently validated. |
| `maxBufferLength` | The real native allocation/budget bound described above. |

Zero resource maxima correspond to factories or commands the frontend rejects:
rendering, textures, samplers, indirect arguments, ray tracing, setBytes,
threadgroup memory and function constants. They do not invent an unsupported
enum. The helper also creates an owned query snapshot with the same actual
owner/epoch and two positive implemented queries: read/write buffer arguments
and nonuniform threadgroup dispatch. The latter uses actual independently
compiled edge variants; it is not a claim of unrestricted shader-language
support or native hardware nonuniform waves.

The Device worker owns integration. Its `initLimits` must store the complete
candidate exactly once, and `limits` must return the stable address of that
owned POD only after initialization. Its feature hook can store the complete
owned `CapabilityQueries` result, while family queries use its own explicit
empty supported-family set. Those are real initialization operations; no
private Apple profile or successful empty reply is necessary. The superclass's
raw feature-query field must not be written through a copied object offset.

## What discovery actually calls

This list separates measured direct calls from later optional application
work. It is not a claim that every transitive CPU helper in Apple's initializer
has been exhaustively evaluated. Actual target construction remains a test.

| Stage | Exact required behavior | Implementation route |
|---|---|---|
| Loader class choice | Genuine `_MTLDevice` subclass; `initWithAcceleratorPort:` returns the actual device or nil. | Actual exported superclass and retained Context connected to the authenticated immediate HackyNaviDevice provider. |
| Service association | `_setAcceleratorService:` strongly retains actual wrapper; wrapper weakly holds device. | Frontend-owned strong ivar, matching measured lifetime. |
| `MTLAddDevice` entry | Actual `MTLDevice` and `MTLDeviceSPI` protocol conformance. | Required method implementations must be assessed before adding conformance; a protocol name alone is insufficient. |
| First init hook | `initLimits`. The inherited implementation calls `featureProfile`, `argumentBuffersSupport`, `maxBufferLength`. | Own typed limits initialization avoids those first two inherited dependencies entirely. |
| Second init hook | `initFeatureQueries`, whose inherited body calls `initGPUFamilySupport` and creates its feature-query object. | Own actual query state and own family state; explicitly override every positive query and any inherited accessor that consumes unsupported private state. |
| Third init hook | `initWorkarounds` calls `supportsFamily:1007` and checks application identifiers. | The truthful family override must be ready before retaining this genuine superclass hook. |
| Conditional init hook | `allowLibrariesFromOtherPlatforms`. | Measured inherited library-builder policy is not shader admission; our actual parser/compiler still validates every input. |
| Final publication | `_deviceWrapper`, `registryID`, `_registerInterestNotification`. | Actual unwrapped self, actual accelerator registry ID, and genuine superclass notification hook. |
| Removal | Wrapper sends `_wasRemoved`; base uses `_deviceWrapper`, then genuine termination helper. | Close actual native Context, then inherited CPU notification path. Hardware retirement remains the native owner's responsibility. |

With these owned init hooks, neither `featureProfile` nor an argument-buffer
tier is a required numeric input to the measured publication sequence. An
unimplemented later query must still fail explicitly. This removes the need
to invent a Mac/Apple profile just to construct a compute frontend.

## Exact remaining SPI work

The actual runtime implements 373 of 392 required SPI selectors through the
`_MTLDevice` superclass chain. The complete captured list includes exact type
encodings and IMP locations in `required_spi_implementations`. Existing
implementation is not correctness proof: for example, several Boolean
`supports...` methods simply delegate through the private feature object at
base offset 392. Production must use owned query methods, not rely on nil
messages to produce useful feature state. The following 19 selectors have no
inherited implementation at all:

| Selectors | Exact return/argument shape | Smallest concrete implementation |
|---|---|---|
| `dedicatedMemorySize` | uint64 result | Actual native VRAM capacity; does not imply current VRAM allocation support. |
| `sharedMemorySize` | uint64 result | Establish its consumed meaning before choosing a value; session budget is not total RAM. |
| `singleFPConfig`, `halfFPConfig`, `doubleFPConfig` | uint64 result each | Recover exact bit meanings and compare with admitted compiler operations. Do not equate hardware floating-point capability with implemented language behavior. |
| `featureProfile` | uint64 result | No established custom compute-only profile enum. Keep explicit unsupported behavior for this later query; owned hooks avoid using it. |
| `metalAssertionsEnabled`, `setMetalAssertionsEnabled:` | BOOL result; void + BOOL | Own real assertion policy or reject enabling an unimplemented assertion compiler path. |
| `supportPriorityBand` | BOOL result | False: no priority-band scheduler is implemented. |
| `supportsSampleCount:` | BOOL + NSUInteger | False: no texture/render sample counts are implemented. |
| `newIndirectArgumentBufferLayoutWithStructType:` | object + object | Actual layout/encoder/compiler/runtime binding work; return explicit failure until available. |
| `newIndirectCommandBufferWithDescriptor:maxCount:options:` | object + object, NSUInteger, NSUInteger | No indirect command ownership/execution path; explicit failure. |
| `newIndirectComputeCommandEncoderWithBuffer:`, `newIndirectRenderCommandEncoderWithBuffer:` | object + object each | Explicit failure while indirect execution is absent. |
| `newIntersectionFunctionTableWithDescriptor:`, `newVisibleFunctionTableWithDescriptor:` | object + object each | Explicit failure while those executable tables are absent. |
| `newTextureWithDescriptor:iosurface:plane:` | object + object, IOSurface pointer, NSUInteger | Current frontend implements explicit failure; no texture mapping/rendering claim. |
| `newTiledTextureWithBytesNoCopy:length:deallocator:descriptor:offset:bytesPerRow:` | object + void pointer, NSUInteger, block, object, NSUInteger, NSUInteger | Explicit failure without taking the caller's storage/deallocator ownership. |
| `newTiledTextureWithBytesNoCopy:length:descriptor:offset:bytesPerRow:` | object + void pointer, NSUInteger, object, NSUInteger, NSUInteger | Explicit failure while tiled resource ownership is absent. |

The public `readWriteTextureSupport` has a real unsupported value,
`MTLReadWriteTextureTierNone`. Use that named value now. In contrast,
`argumentBuffersSupport` has only Tier1=0 and Tier2=1. Its exact private
`indirectArgumentBufferCapabilities` return is a 32-bit bitfield struct, not
uint64. A zero private struct would still make the inherited public method
advertise Tier1. The minimum unfinished implementation is a genuine argument
layout/encoder, persistent backing storage, compiler lowering for argument
buffer loads and runtime resource-residency binding; it is independent of
the direct-buffer compute path and can be developed in parallel.

Other later application work includes real `MTLArchitecture` metadata,
topology/location/removability, shared/managed buffer semantics, textures,
rendering and presentation. Each needs its own source-backed object or backend
contract. None should block compilation of the existing real factories, but
none is supplied by choosing an arbitrary scalar enum or successful dummy
object. HDR remains outside this milestone.

## Validation scope

`build/metal-capabilities-local/report.json` binds the production helper,
metadata tests and exact ABI probe. Tests cover wrong identities, versions,
reserved fields, every capability-bit mutation, invalid geometry, public/native
resource-count boundaries, budget rounding and clearing failed outputs. They
test metadata admission and unopened helper behavior. They do not manufacture
a live Context or stand in for real Metal construction, target user-client
execution, shader execution or display output. Native frontend compilation is
performed separately by the root build worker.
