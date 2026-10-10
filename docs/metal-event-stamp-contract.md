# Installed event-machine and stamp-mapping contract

This advances the [vendor-family contract](metal-vendor-family-contract.md)
using actual code and references from the same **macOS 15.8 / 24H22 x86_64**
images. It establishes the standard type-5 client mapping lifecycle and
observable return/dispatch contracts. It does not implement a family bridge,
create a device or execute GPU work.

Evidence is in `build/metal-event-contract`. The capture reads selected
symbol/section-bounded code ranges, a 480-byte method table, selected vtable
and metaclass references, and five 4 KiB relocation pages. The local validator
walks **2,323 actual chain nodes** and verifies **25 selected pointer words**
and their exact symbol identities. Code was disassembled locally with LLVM;
literal encoding bytes and total decoded lengths match every captured byte.
A symbol boundary is not automatically a function boundary; padding and the
earlier AMD extra code sequence remain explicitly distinguished.

All offsets below identify observations in this exact binary. They are not
production C++ structure declarations or permission to write private ivars.

## Standard device selector 2

The actual `IOAccelDevice2::getTargetAndMethodForIndex` stores the client as
the dispatch target, accepts indices below 10, multiplies the index by 48 and
returns the matching entry in `IOAccelDevice2::sDeviceMethods`. Out-of-range
indices return null.

The actual table has ten 48-byte `IOExternalMethod` entries. A strict compile
against the existing real kernel headers independently verifies that type's
48-byte extent, object at +0, member-function representation at +8, flags at
+24 and counts at +32/+40. The actual table uses zero object/this-adjustment
words and relocatable direct function pointers.

Selector **2** resolves to
`IOAccelDevice2::get_event_machine(IOAccelDeviceEventMachineData*)`, flags **2**
(`kIOUCScalarIStructO`), zero scalar input and **600 bytes** of structure output.
This independently matches the observed userspace initializer call. Its
function is not selected merely because its name sounds appropriate.

The client override of `externalMethod` calls the actual `OSIncrementAtomic`
on its in-flight field, checks a separate stopped/termination flag, then calls
the inherited dispatch path. It preserves the returned status. After
`OSDecrementAtomic`, a particular final-operation/flag combination runs a
deferred `detach_device` path under the accelerator's busy/lock operations.
This establishes an actual in-flight/deferred-detach mechanism, but does not
alone prove every close/termination entry point: `detach_device`, the flag
writers and all stop callers remain outside this bounded capture.

## Descriptor and task mapping lifetime

`IOAccelDevice2::deviceStart()` performs this exact sequence:

1. Loads its accelerator from client offset +264. Calls accelerator virtual
   slot **271** (`getStampMemory`) with the address of client +296 as the
   `unsigned int*` output. Stores the full returned pointer at client +280.
   A null descriptor takes the false return path.
2. Calls the returned object's public `retain` slot. This means this client
   takes its own reference to the supplied existing descriptor.
3. Calls the resolved public
   `IOMemoryDescriptor::createMappingInTask(task*,uint64_t,unsigned int,uint64_t,uint64_t)`.
   The task comes from client +232, requested address is zero, offset and length
   are zero, and options are **`0x04001001`**. The actual public header decodes
   these options as `kIOMapUnique | kIOMapReadOnly | kIOMapAnywhere`.
4. Stores the returned map at client +288. If mapping fails, it releases and
   clears the descriptor reference and returns false. On success it adds this
   client to the accelerator's actual device list and returns true in AL.

The public mapping API documents that the returned map retains the descriptor
and that releasing the final map reference destroys the mapping. The extra
client descriptor reference is therefore a real separate ownership decision,
not an inferred alias of the mapping reference.

`deviceStop()` removes the client from that same list, releases/clears the map
at +288 first, releases/clears the descriptor at +280 next, and releases/clears
another owned field at +256. The earlier `stopLocked()` invokes the actual
deviceStop virtual slot and continues through its inherited stop/close paths.
`free()` handles other client resources; it is not a substitute for deviceStop's
mapping cleanup.

The shipped AMD getter returns an existing pointer from its accelerator state
and writes zero to a non-null output-offset pointer. The Intel getter writes
zero to the offset and tail-calls the actual
`IGMappedBuffer::getMemoryDescriptor() const`, whose body returns its existing
descriptor through the buffer's owner fields. These facts, together with the
caller's actual `retain` and typed public mapping call, establish a
**descriptor-compatible object-pointer return** and an output **byte offset**.
The caller owns the retain it takes; a getter must not fabricate a CPU address
or imply an already-created userspace mapping.

## Contents of the actual 600-byte reply

The method queries the actual retained map's virtual-address operation. A
real-header compiler check places public `IOMemoryMap::getVirtualAddress()`
at C++ virtual slot **35**, matching the call in the captured method.

| Reply range | Actual write or use |
|---|---|
| +0, 8 bytes | Map virtual address plus saved unsigned offset at client +296. |
| +8/+12/+16, 4 bytes each | Three accelerator configuration fields; their semantic names remain unresolved. |
| +20, 4 bytes | Actual event-machine `getNumStamps()` result, handled as signed by the caller. |
| +24, capacity 64 bytes | Name copied by the resolved **`strlcpy`**, from event-machine virtual slot 36. It is not a raw 64-byte event-data copy. |
| +88, 4 bytes per stamp | Each actual `getStampOffset(index)` result. |

The event machine is read from accelerator +896. Its measured base table
resolves slot 37 to `IOAccelEventMachine2::getNumStamps()` and slot 39 to
`getStampOffset(int)`. Base slot 36 is actually pure virtual, so the concrete
event object must supply the name operation too. The bounded capture does not
invent that pure method's name from convention.

The producer must ensure its stamp count and offsets fit the reply and actual
mapped descriptor. The reply has space for 128 32-bit offsets; this calculation
does not prove the base initializer's count validation or establish that every
offset is valid. The userspace helper previously captured dereferences these
addresses immediately and retains them for later event observation. Returning
success with unmapped or fabricated pointers would not meet this contract.

The actual reply method returns zero status after its normal locked path. It
relies on valid startup-owned state; it does not allocate a replacement stamp
page while answering selector 2. The accelerator's actual lock/busy calls
surround its event queries.

## Event factory return and failure behavior

`IOGraphicsAccelerator2::createEventMachine()` calls virtual slot **273**,
retains the full returned pointer in a register and rejects null. It calls the
object's init operation with the accelerator and two actual configuration
arguments, tests AL as a boolean result, and releases the object on failed
init before returning null. Success returns that actual object pointer.

Intel's implementation allocates through the resolved
`IGAccelEventMachine::metaClass` pointer. AMD calls its actual
`AMDRadeonX6000_AMDAccelEventMachine::operator new`, with 3,416 as the requested
allocation size, then its actual C1 constructor and returns the object. Those
observations prove actual vendor object construction paths; they do not justify
allocating an arbitrary 3,416-byte buffer as an event object.

`IOAccelEventMachineFast2::init(IOGraphicsAccelerator2*,unsigned int,int)` first
calls its real `IOAccelEventMachine2` base init and checks AL before touching its
stamp bookkeeping. Its free operation delegates to the same real base. The
base init/free bodies, concrete event subtype declarations and stamp-completion
behavior are still required before constructing a compatible event object.
The pointer-return and boolean-use evidence is stronger than names alone, but
does not recover every non-template source return spelling.

## Production implications and remaining proof

The getStampMemory contract can be planned around an actual driver-owned
`IOMemoryDescriptor`, retained by the standard client and mapped read-only in
that client's own task. It must describe real memory with the correct GPU
visibility, offsets and stamp progression. The GPU and the native queue retain
their own ownership independently of a userspace map's reference count.
Hardware completion must update the actual shared state; CPU success counters
cannot replace execution.

No private production declaration is introduced here. The remaining minimum
proof includes:

- Event-machine base init/free and concrete subtype requirements, including the
  pure name operation and stamp-count/offset invariants.
- The actual stamp value, wrap, wait and completion semantics, and how real
  native queue completion can satisfy them.
- The remaining concurrent detach/close callers and flag transitions; this
  report does not certify complete teardown race safety from one caller.
- The accelerator configuration structure, startup/error order and the other
  required factories identified by the vendor audit.

`validation.json` binds the scripts, captures, local disassembly and public
kernel-header compiler experiment. All inspection was read-only, with no
remote files left behind, no user client opened and no device/GPU execution.
