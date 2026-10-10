# Actual device hooks and frontend obligations

This follows `metal-loader-contract.md`. It records additional code and runtime
metadata from the same Intel mini, macOS 15.8/24H22, Metal UUID
`c3efbabe-6b06-33ab-bc45-43fe86297aa1`. It does not instantiate or enumerate a
device, open a user client, invoke an Objective-C method, execute a policy
function, install a bundle or submit GPU work. Captures ran through stdin and
created no remote files. See `build/metal-device-hooks/validation.json` for
exact bytes, hashes, explicit data tails and limits.

## Established lifetime and publication behavior

| Actual method | Encoding | Behavior observed in complete captured body |
|---|---|---|
| `MTLIOAccelDevice _setAcceleratorService:` | `v24@0:8@16` | Calls `objc_retain` on the wrapper, then stores it at its runtime-resolved `_acceleratorService` ivar. Its actual `dealloc` releases the same field. |
| `_MTLDevice _registerInterestNotification` | `c16@0:8` | Returns YES. This is the real superclass implementation; it does not open a client or register another notification. |
| `_MTLDevice _deviceWrapper` | `@16@0:8` | Returns nil; this abstract default is unsuitable for normal device removal. |
| `MTLIOAccelDevice _deviceWrapper` | `@16@0:8` | Reads its weak wrapper. When none exists, returns self. When present, sends the wrapper `_deviceWrapper` before returning; the actual selector reference at image offset `0x33dda850` was resolved. |
| `_MTLDevice _wasRemoved` | `v16@0:8` | Asks `_deviceWrapper`; when it equals self, calls actual `_MTLDeviceTerminated`; otherwise sends `_wasRemoved` to that object. |
| `_MTLDevice supportsFamily:` | `c24@0:8q16` | Searches the initialized owned family vector. It dereferences that vector, so skipping its initialization while inheriting this method is unsafe. |

The unwrapped frontend can retain its service wrapper in an ordinary strong
ivar and truthfully return self from `_deviceWrapper`. It must not copy the
measured private offset into its class. The service wrapper stores the device
weakly, avoiding a strong cycle. Its `_wasTerminated` method destroys its
notification port, releases the notification object, loads the weak device
and sends `_wasRemoved`, then removes itself from the global service array and
registry-ID dictionary. Its `dealloc` checks notification cleanup, releases
the retained accelerator port and calls its superclass. The device's actual
Context must close through the existing native retirement path on removal;
an inherited CPU notification does not establish GPU retirement.

The final `MTLAddDevice` block reads the real device `registryID`, applies the
process's allowed-device list, and requires `_registerInterestNotification`
to return true before adding the device to the array and invoking the added
notification helper. Under tooling it first uses `_deviceWrapper` and the
installed wrapper function. Thus `_deviceWrapper` is needed even though
ordinary discovery starts through `initWithAcceleratorPort:`.

## Initialization is not a generic hardware capability source

`_MTLDevice initLimits` calls `featureProfile`, `argumentBuffersSupport` and
`maxBufferLength`, and writes many inherited limit fields. The full 3,311-byte
range was captured; its last three bytes are padding. **There is no base
implementation of `featureProfile`, `maxBufferLength` or `registryID`.** A
subclass must supply actual behavior rather than assume usable defaults.

`initFeatureQueries` calls `initGPUFamilySupport`, then allocates
`MTLDeviceFeatureQueries`, sends `initWithDevice:`, stores the result and sends
`validate`. In this actual image the abstract feature-query initializer is an
eight-byte implementation returning nil; that class has no `validate` method.
These are facts about Apple's implementation, not proposed placeholder methods
for HackyNavi. A subclass returning successful empty feature state would still
need to satisfy every accessor consuming that state.

`initGPUFamilySupport` allocates or clears a real CPU vector and fills it from
`featureProfile`. Its complete 9,704-byte range includes a 48-byte table for
profile values 0 through 11; other explicit branches cover values around
10,000, 20,000 and 30,000. The vector includes concrete Apple/Mac/Common-family
IDs. Unsupported numeric branches can leave it empty, but that does **not**
establish a legitimate private enum for a custom compute-only implementation.
Do not choose an arbitrary out-of-range profile to make initialization pass.

`initWorkarounds` asks `supportsFamily:1007` and checks application identifiers.
It writes two inherited workaround flags. Its family lookup must therefore
use actually initialized, truthful state. `allowLibrariesFromOtherPlatforms`
changes the inherited library-builder policy and then delegates through its
compiler accessor; it is not a capability-admission substitute.

The actual `indirectArgumentBufferCapabilities` SPI encoding is
`{IndirectArgumentBufferCapabilities=b1b1b1b29}16@0:8`: a 32-bit bitfield
structure, **not uint64_t**. The public base `argumentBuffersSupport` computes
its tier from whether all three low bits are set. Returning zero here still
maps to an argument-buffer tier; it does not mean unsupported. Our existing
frontend has no argument-encoder implementation, so it cannot advertise a
tier merely to satisfy `initLimits`.

The runtime reports neither `_MTLDevice` nor `MTLIOAccelDevice` as directly
conforming to `MTLDevice` or `MTLDeviceSPI`. The actual SPI metadata has 392
required and 80 optional instance method descriptions, preserved verbatim in
the capture. Declaring a private protocol name is not implementation proof.
The independent bundle-trust capture verifies the actual Metal.tbd export and
runtime `dlsym` class/metaclass pointers for `_MTLDevice`. A genuine statically
linked subclass can use the nonfragile Objective-C ABI without declaring or
copying inherited ivar offsets. See `build/metal-bundle-trust/policy-2.json`.

## Bound confirmed for pipeline descriptors

The actual `MTLPipelineBufferDescriptorArrayInternal
objectAtIndexedSubscript:` checks the unsigned index against 31 and calls its
error path for index >=31. Valid indices are 0 through 30. An absent descriptor
is allocated and installed with compare-and-exchange. The abstract public
array class's method returns nil. The public frontend can now inspect all 31
actual descriptor slots to reject unsupported mutability instead of guessing
the array extent. The separate native transport's capacity of 32 bindings is
not authority to use Metal descriptor index 31.

## Concrete implementation division

The Device frontend owns accelerator-to-provider authentication, distinct real
registry IDs, retained real native Context, service association/removal and
factories for the already implemented buffer, library, function, pipeline,
queue and command objects. `_deviceWrapper` returns this actual device while
it has no tooling wrapper. `_wasRemoved` closes its actual Context before the
measured inherited termination path.

A separate capability helper can validate the real negotiated reusable
profile and capture its allocation limit, system-memory budget, page/VA
geometry and device identity. It can expose the admitted wave32/zero-LDS
compiler profile's conservative 640-thread limit, while actual pipeline
variants retain their own stricter checks. Unsupported Boolean queries must
be false. Scalar enums without an unsupported value must fail explicitly
until implemented; zero is not a substitute.

The remaining work to publish a truthful SPI device is exact feature/profile
state ownership and required accessor behavior. Neither a copied Mac/Apple
profile nor empty successful init hooks is acceptable. The backend-connected
factories and real limits can be implemented independently while this private
contract is completed. No HDR, renderer, argument-buffer, texture or display
capability is inferred from the GPU hardware model alone.

## Exact installed kernel dependency metadata

The actual graphics bundle uses a flat Info.plist, not Contents/Info.plist:
`/System/Library/Extensions/IOGraphicsFamily.kext/Info.plist`, 3,606 bytes,
SHA-256 `cb7ec460a374aacb3112754943911f5dbe6df26680958cd7bcbfa7343594e262`.
Its identifier is `com.apple.iokit.IOGraphicsFamily`, version `599`, compatible
version `1.0.0b1`. The driver's declared `2.4.1` dependency is inside that
range; actual full linking was separately verified by the root build worker.
