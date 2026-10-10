# Installed macOS 15.8 Metal initializer contract

This advances [the discovery prerequisite audit](metal-discovery-prerequisites.md)
from reference-source leads to the actual installed initializer and userspace
IOAccelerator call sequence. All observations below are from the mini's
**macOS 15.8, build24H22, x86_64**. They are not yet measurements of the user's
reported 15.8.1 target. No Metal object was constructed, user client opened,
GPU enumerated or command submitted.

The two methods were read as code data from their loaded images, bounded by
the actual `LC_FUNCTION_STARTS` and executable section. They were disassembled
locally with LLVM20.1.8; every literal encoding byte and total decoded length
was checked against the captured bytes. Each followed call stub and selector
reference came from those instructions. Function pointers were never called.
Some dyld-cache GOT slots do not belong to a named image section: for those
explicit 8-byte slots only, the reader checked this process's readable VM
region before reading and resolving the pointed-to symbol. All returned Mach
object-name rights were released. Captures created no remote files.

Evidence is under `build/metal-initializer-audit/`: `runtime.json`,
`disassembly.json`, `references-v2.json`, `helpers.json`,
`helper-disassembly.json` and `helper-references.json`. The earlier reference
capture that declined coalesced GOT slots remains separate historical evidence.

## Actual base initialization

Metal framework UUID: `c3efbabe-6b06-33ab-bc45-43fe86297aa1`.

- `-[MTLIOAccelDevice initWithAcceleratorPort:]`: image offset `0x156f`,
  **539 bytes**, SHA256
  `97d028e23d66659c49fa0fa68286c87be90330dc8f852302d82cb3befa9ac46d`.
- `-[MTLIOAccelDevice lazyInitialize]`: image offset `0x1155fa`,
  **602 bytes**, SHA256
  `f4e91aff2fe1cbe381ca33b4eaf16cac3519a16a8ea398c78c6c30da78dae57f`.

The non-lazy initializer does the following:

1. Sends the real super `init` using `objc_msgSendSuper2`.
2. Calls **`IOAccelDeviceCreateWithAPIProperty(port, "Metal")`**, stores its
   result as `_deviceRef`, and takes the release/nil path if creation fails.
3. Gets the accelerator's actual registry entry ID and retains the IOKit port.
4. Reads configuration via `IOAccelDeviceGetConfig64`, peer fields via
   `IOAccelDeviceGetPeerInfo` and shared-memory size via
   `IOAccelDeviceGetSharedMemorySize`; obtains the global trace object ID.
5. Sends `updateGPUSelectionProperties`, then `supportLazyInitialization`.
   When lazy initialization is not supported, it sends `lazyInitialize`
   immediately and requires a true BOOL result. Failure releases the object
   and returns nil.

This establishes actual device/standard-client prerequisites, rather than
assuming that three registry strings suffice. It also confirms why Cobalt's
void declaration of `lazyInitialize` cannot be used on this OS. The bodies of
`_MTLDevice::init`, `updateGPUSelectionProperties` and full deallocation are
outside this bounded capture; their additional requirements are not claimed
to be settled.

The lazy initializer first calls **`IOAccelSharedCreate(_deviceRef)`** and
requires a non-null result. It immediately calls
**`IOAccelSharedSetupDirtyRing(_sharedRef)`** and requires status0. It then
constructs actual Metal resource-list pools (capacities1024 and256) and
`MTLIOAccelDeviceShmemPool` objects using `MTLIOAccelDeviceShmem` and the
`initialKernelCommandShmemSize` / `initialSegmentListShmemSize` selectors.
It creates dispatch queues and a command-buffer-storage pool; a null storage
pool takes the false-return path. It sets up cleanup timer handling and returns
true. These additional pool constructors can have their own standard-client
requirements; they have not been replaced or exhaustively followed here.

## Exact standard user-client calls recovered

IOAccelerator userspace framework UUID:
`e2bac781-b505-38fc-a912-fc0b204f4802`. The six followed helpers all matched
function starts and ends and were captured without truncation.

| Actual helper/location | Client or method call | What the captured body requires |
|---|---|---|
| `IOAccelDeviceCreateWithAPIProperty`, `0xde0`, 687B | `IOServiceOpen(accelerator, mach_task_self(), **5**, &connection)` | Open success; otherwise returns null. |
| Same, `0xe7d` call | Device selector **9**, input struct **16B**, no output | Copies the API string with `strncpy(...,16)` before sending. This body's return path does not test this selector's status. This does not authorize ignoring it in a kernel implementation. |
| Same, `0xeef` call | Device selector **2**, no input, output capacity **600B / 0x258** | Nonzero status fails creation. The successful reply is used to construct and immediately read stamp pointers; zero-filled fake output cannot establish that state. |
| Same, `0xfbc` call | Device selector **0**, no input, output capacity **64B / 0x40** | Nonzero status fails creation. Actual configuration is retained in the device object. |
| Same, `0x1027` call | Device selector **7**, no input, output capacity **24B / 0x18** | Failure is tolerated by this body; success populates trace/device information, including a version-conditioned field. |
| `IOAccelSharedCreate`, `0x12b3`, 251B | `IOServiceOpen(same accelerator, mach_task_self(), **6**, &connection)` | Open success and actual allocation; otherwise null. The real device owner is stored in the shared object. |
| Same, `0x135a` call | Shared selector **9**, no input, output capacity **16B** | Copies two returned words on success; on error this body stores zero words and still returns a shared object. Later operations may require them. |
| `IOAccelSharedSetupDirtyRing`, `0x1da1`, 98B; call at `0x1dcd` | Shared selector **10**, no input, output capacity **24B** | Returns the actual status. On success it immediately dereferences a uint32 at **first returned pointer +4**, subtracts1 and retains that value with the reply fields. Thus a successful reply requires actual valid mapped state. |

Every method call in the table resolves to the installed
`IOConnectCallStructMethod`; the open calls resolve to the installed
`IOServiceOpen`. Type5/type6 and the selector/byte counts are now observed facts
for this OS build. They were not taken from the reference projects or inferred
from an export name. No production code in this audit calls these private
helpers or reimplements their payloads.

The 600-byte event-machine reply's first 64-bit word is used as a base pointer.
A signed count is read at reply offset`0x14`. For multiple stamp pointers,
32-bit offsets beginning at `0x58` are added to that base; for a positive count,
the resulting pointers are read immediately as uint32. This is a concrete
memory-lifetime requirement, not evidence that a caller-supplied pointer or a
zero stamp page is acceptable. The real family kernel mapping/ownership and
actual stamp progression still must be established.

The config getter `IOAccelDeviceGetConfig64` (`0x10e6`,139B) copies from the
retained 64-byte config reply. The fields consumed by this initializer are:

| Reply offset | Width | Destination identified by actual ivar reference |
|---|---|---|
| `0x00` | u32 | `_configBits` |
| `0x04` | u32 | `_deviceBits` |
| `0x08` | u64 | `_textureRam` |
| `0x10` | u64 | `_videoRam` |
| `0x18` | u32 | `_accelID` |
| `0x20` | u64 | `_sharedMemorySize`, through its separate getter |

These are observed reads, not a complete `IOAccelConfig` definition. The
kernel configuration structure and this user reply are not automatically the
same layout. Bit meanings, remaining fields, limits and factory ownership must
come from the actual family implementation. In particular the initializer
branches on config bit`0x1000` and alters its RAM/shared-size bookkeeping;
assigning that bit a convenient unverified capability name would be unsafe.

## Kernel-family binary source now located

The installed standalone family binaries are absent, but the exact entries
exist in `/System/Library/KernelCollections/SystemKernelExtensions.kc`:

| Entry | Entry UUID | Header file offset |
|---|---|---|
| `com.apple.iokit.IOAcceleratorFamily2` | `97a7305c-fb14-39fc-bc20-ed834abceb2d` | `350302208` |
| `com.apple.iokit.IOGraphicsFamily` | `ca8b49bc-3655-312c-91aa-fb848251c78e` | `351973376` |

The collection UUID is `ae091e50-7cbe-b977-ea9b-39e162eebeaa`.
`kernel-collection-metadata-v2.json` captures these headers, segment mappings,
symbol-table metadata and selected symbols. Only bounded name prefixes for
these two entries were read from the shared string table: **1,746,432 bytes**,
plus their small headers/symbol tables. The 373,112,832-byte collection was not
copied. The initial metadata capture that skipped the large shared string
table remains separate; the later reader used individual bounded name reads.

## Actual superclass extent and virtual entries

The follow-up captures two complete symbol-bounded vtable ranges, four
50-byte metaclass constructors, their precise references and six 4 KiB chain
pages. `scripts/decode-accelerator-family-layout.py` validates **3,005 actual
chain nodes**, byte hashes, RIP-relative constructor/stub references and every
table entry. `family-layout-decoded.json` records all resolved targets.

Each page's actual fixup metadata selects format 11. The decoder uses the
one-byte chain stride and target/cache-level fields from Apple's pinned
[dyld fixup schema](https://raw.githubusercontent.com/apple-oss-distributions/dyld/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/include/mach-o/fixup-chains.h).
All observed authentication/reserved fields are zero. A word is accepted only
if the walk from the actual page-chain start reaches it; stripping high bits
from an arbitrary qword is insufficient.

The kernel cache base is independently established as
`0xffffff8000100000`: actual symbols for `OSMetaClass`'s constructor,
`IOService::gMetaClass` and `___cxa_pure_virtual`, minus their captured target
offsets, all agree. The collection-header and kernel-entry-header addresses
are different and are **not** substituted for this base. Earlier unsuccessful
base hypotheses remain in separate historical captures. The final bounded
kernel-symbol read resolves all 266 requested kernel targets; the actual kernel
image UUID is `b7b3e8ad-9ca6-382c-bd56-8d52139342a4`.

Both C1/C2 constructors call the resolved
`OSMetaClass::OSMetaClass(char const*, OSMetaClass const*, unsigned int)`.
The actual public kernel header identifies its last argument as the represented
class's allocation size. The two versions agree on names, parent and sizes:

| Class | Verified parent metaclass | Instance-size argument | Virtual entries |
|---|---|---|---|
| `IOAccelerator` | `IOService::gMetaClass` | 136 bytes | 266 |
| `IOGraphicsAccelerator2` | `IOAccelerator::gMetaClass` | 3,544 bytes | 350 |

The base's symbol range is 2,152 bytes and the family's is 2,824 bytes. Each has
two zero ABI header words, the stated number of relocated virtual targets and
one unrelocated zero trailer. The real-header Apple-kext compiler labels that
trailer `vcall_offset (0)`; it must not be counted as another function.
There are exactly **84 new family entries** and **17 pure virtual entries**.
All 17 resolve to the actual kernel's `___cxa_pure_virtual`, rather than merely
sharing an unnamed target. Their zero-based C++ virtual slots are:

`271, 273, 307, 310, 312, 313, 314, 320, 324, 325, 326, 327, 328, 332, 333, 334, 337`.

These slot numbers start at the vtable address point. The qword index from
the `__ZTV` symbol is two greater. For example, `start(IOService*)` is virtual
slot **184**, symbol qword **186**, byte offset **1,488**; the family's
four-argument `newUserClient(task*,void*,unsigned int,IOUserClient**)` is slot
**239**, qword **241**. The five-argument overload remains inherited at slot
238. The family changes 17 inherited entries: two destructors, `getMetaClass`
and 14 other methods. The decoded report gives their actual symbol identities.

The existing public MacKernelSDK header
`IOKit/graphics/IOAccelerator.h` already declares the base. A strict
`x86_64-macos.15.0`, `-fapple-kext` compiler experiment verifies its 136-byte
extent and 266 virtual entries. All **263 unchanged** entries in a trivial
derived-class compiler vtable match the installed base exactly; the remaining
three are that experiment's two destructors and `free` override. The experiment
also emits a qualified base `start` call through byte offset 1,488. Thus this
SDK needs no reconstructed `IOAccelerator` declaration. This was compilation,
not a loaded-kext test or a running-kernel class-registry query.

This establishes class extents, parent identities, ordering and observed
inherited signatures for this measured OS. It does **not** recover names or
return types for the pure slots: a pure entry points to a common function, and
ordinary Itanium mangling does not encode return types. The next bounded step
is to match those slots against real shipped AMD/Intel subclass vtables, prove
their parent links, and resolve their actual override symbols. Factory-object
layouts, configuration field meanings, allocation/mapping ownership and error
contracts still require the actual implementations/callers. No production
private declarations are generated by this audit.

The immediate implementation result remains a concrete specification for
what our real accelerator bridge must supply: standard device/shared clients,
valid family-owned mapped event/dirty-ring state and initialization failure
handling, all attached to the existing hardware owner. The custom reusable
HackyNavi client can carry GPU work, but cannot replace those standard base
initializer prerequisites with fabricated replies.

The first base-family validation is frozen in
`build/metal-initializer-audit/initializer-family-validation.json` with source
and evidence hashes. Later vendor-subclass evidence is a separate follow-up.
