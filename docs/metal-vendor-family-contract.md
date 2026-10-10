# Shipped vendor overrides for the measured accelerator family

This follows [the installed initializer and base-family audit](metal-initializer-contract.md).
It resolves the 17 pure virtual slots against actual shipped Intel/AMD class
tables on **macOS 15.8, build 24H22, x86_64**. It does not create production
private declarations or establish the separate user's 15.8.1 ABI.

The read-only collection inventory identified these two entries in the same
System Kernel Collection already measured by the base audit:

| Entry | UUID | Captured class |
|---|---|---|
| `com.apple.kext.AMDRadeonX6000` | `4f6a4bf7-2365-3d73-8001-ada280aba018` | `AMDRadeonX6000_AMDGraphicsAccelerator` |
| `com.apple.driver.AppleIntelKBLGraphics` | `a7d4e6cd-7a4f-3595-a2dc-665abeb41d38` | `IntelAccelerator` |

The selected symbol reads total 1,152,384 bytes of bounded name prefixes and
180,448 bytes of nlists. The follow-up reads two complete vtable ranges, four
constructor symbol ranges and six 4 KiB fixup pages. No collection was copied,
no remote file was created and no running kernel or GPU was accessed.

`scripts/decode-vendor-accelerator-layout.py` validates byte hashes, all
2,871 reached chain nodes, all table-word membership in those actual chains,
and constructor/stub references. The independently captured AMD C2 and both
Intel constructors call the actual `OSMetaClass` constructor, name their
classes and point to **`IOGraphicsAccelerator2::gMetaClass`** as direct parent.
Their size arguments are 8,168 and 14,080 bytes respectively. The AMD C1
symbol-bounded range includes a second code sequence after its first return;
it is not misrepresented as one 60-byte function. The C2 supplies the exact
50-byte constructor evidence used here.

The AMD class has 365 virtual entries and Intel has 352, in addition to the
two ABI header words and one zero trailer. The direct parent has 350 entries.
Slots below use the **zero-based C++ address-point index**; their qword index
from `__ZTV` is two greater.

| Pure family slot | Actual recovered method and parameters | Intel | Generic AMD base |
|---|---|---|---|
| 271 | `getStampMemory(unsigned int*)` | Override | Matching override |
| 273 | `newEventMachine()` | Override | Matching override |
| 307 | `createUserGPUTask()` | Override | Matching override |
| 310 | `populateAccelConfig(IOAccelConfig*)` | Override | Matching override |
| 312 | `configureDevice(IOPCIDevice*)` | Override | Matching override |
| 313 | `teardownDevice(IOPCIDevice*)` | Override | Matching override |
| 314 | `createKernelGPUTask()` | Override | Matching override |
| 320 | `newDisplayMachine()` | Override | Matching override |
| 324 | `newGLContext()` | Override | Still pure |
| 325 | `newCLContext()` | Override | Still pure |
| 326 | `newSurface()` | Override | Matching override |
| 327 | `new2DContext()` | Override | Matching override |
| 328 | `newVideoContext()` | Override | Still pure |
| 332 | `newSysMemory()` | Override | Matching override |
| 333 | `newVidMemory()` | Override | Matching override |
| 334 | `newResource()` | Override | Still pure |
| 337 | `newMemoryMap()` | Override | Matching override |

All 17 Intel targets resolve to exact symbols from its own shipped entry. All
13 implemented AMD targets independently agree on method and parameter
encoding; its other four resolve to the real kernel's `___cxa_pure_virtual`.
For example, slot 271 resolves to
`__ZN16IntelAccelerator14getStampMemoryEPj` and
`__ZN37AMDRadeonX6000_AMDGraphicsAccelerator14getStampMemoryEPj`.
The decoder accepts only the actual observed simple nested-name and parameter
encodings. It obtains no method names from NVIDIA/Cobalt declarations.

The generic AMD base remains abstract; this capture does not claim it can be
instantiated as a complete AMD driver. Three other AMD table targets (slots
209, 319 and 350) lack names in this bounded accelerator-symbol inventory and
remain explicitly unresolved in the report. None is one of the 17 required
slots; no identity is invented for them.

**Return types remain unresolved for all 17 methods.** Ordinary method
mangling supplies their parameter types, not the non-template return type.
Correct names and slot positions do not justify choosing pointer, BOOL,
IOReturn or void from naming convention. Object layout, factory ownership,
configuration semantics and hardware execution are also separate contracts.

The next production-needed contract is the family event/stamp path used by
standard device client type 5 during the real Metal base initializer. The
next bounded implementation/caller inspection should follow:

1. `IOGraphicsAccelerator2::createEventMachine` and its calls to slots 271/273:
   establish the actual returned object/pointer use, output size, allocation
   ownership and failure handling for `getStampMemory` / `newEventMachine`.
2. The actual device client's event-machine selector implementation: connect
   that ownership to the previously observed 600-byte user reply and the
   mapped stamp pointers it contains.
3. `populateAccelConfig`'s real family caller and standard device configuration
   reply conversion: distinguish the kernel structure from the observed
   64-byte user reply and derive fields from actual writes/reads.
4. Family `start` / device configuration error paths and the initial GPU-task
   factories: derive return-value checks, release ordering and prerequisites
   before authoring any private interface that can be instantiated.

The existing reusable native compute backend can supply real execution, but
does not by itself implement these family objects and shared mappings. A
success status, zero-filled reply or empty factory cannot replace them.

Evidence is under `build/metal-vendor-abi-audit`: `metadata.json`, `layout.json`
and `decoded.json`; `validation.json` binds those inputs, the scripts and this
document. The earlier base audit remains frozen and unchanged. No production
source, kext package or third-party implementation was modified or imported.
