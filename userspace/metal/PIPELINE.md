# Retained offline compute products, ABI v1

`include/hackynavi_pipeline.h` exposes an owned product built from a live,
successful `HNShaderResult`. `hn_pipeline_build` runs the existing dispatch
planner against that exact result, then invokes the real Mesa NIR/ACO executable
for each distinct local size. It passes the actual SPIR-V entry name and each
region's specialization values. There is no source-name lookup and no embedded
replacement shader. If any compiler call, reflection check, or metadata check
fails, all accumulated variants are discarded and only an error handle returns.

The caller supplies a trusted, absolute `hackynavi-aco` executable path. Tool
environment overrides must be set before compiler threads start. This is a
compiler component, not a subprocess sandbox or arbitrary-ISA admission policy.

## Ownership and launch contract

Free every result with `hn_pipeline_free`, including error results. The product
owns its machine code, original SPIR-V, entry string, full reflection, original
ACO metadata, typed launch metadata and dispatch regions. It remains valid
after the input shader is freed. Except for the error string, every accessor
copies into caller memory; exact-size copies leave output unchanged on failure.
Read-only access can be concurrent, but destruction must be externally joined.

`HNPipelineRegion` identifies the variant and preserves the actual region's
group counts, local size, thread/group bases, specialization IDs and push words.
For 4099 threads at nominal local64, the compiler generates local64 and local3
code. The second region starts at thread4096/group64. Its 48-byte push payload is
`[4099,1,1,4096,0,0,64,0,0,65,1,1]`. Changing hardware thread registers without
compiling the second variant would be incorrect.

The current backend contract is GFX1100 wave32, WGP mode, singleton SSBOs in
descriptor set0 at bindings0..31, a16-byte raw-buffer descriptor stride, and
two64-bit user-SGPR pointers: descriptor table in SGPR0/1 and dispatch push data
in SGPR2/3. System SGPR4..7 carry workgroupXYZ and TG_SIZE; VGPR0 contains packed
local invocation ID. Typed RSRC1/2 values are recomputed from the real metadata
and compared exactly, not accepted as opaque caller register values. Scratch,
LDS, spills, shared VGPRs, relocations and appended constants are rejected.
Nominal and actual workgroups must fit1024 threads. These are compiler limits,
not advertised `MTLDevice` limits.

`descriptor_table_bytes` includes holes up to the highest reflected binding;
future submission must initialize holes and bind actual owned resources at the
specified indices. A binding's `declared_element_bytes` is **not** its required
total allocation size. Retained reflection includes full extent/footprint data;
future native admission must enforce actual buffer/descriptor bounds and
lifetime independently. The product does not construct GPU addresses.

The adapter derives conservative read/write masks from the emitted SPIR-V
storage-buffer load/store pointer graph and checks them against reflection.
It currently supports ordinary variables, access chains, pointer copies,
selects and acyclic phi merges, up to4096 definitions and64 provenance levels.
Unknown provenance, pointer calls/escapes/conversions/cycles, copy-memory,
array-length instructions, atomics, textures, argument buffers, and runtime
resource/function specialization return explicit errors. The NIR/ACO backend
independently enforces its final-intrinsic subset. Per-variant masks remain
conservative when specialization eliminates an access; they never downgrade
the original shader's required writes.

Up to8 variants may be retained, each with at most1MiB code and64KiB metadata.
The actual compiler process has a shared120-second deadline, bounded output
reads and periodic output-size checks. Validation uses the existing genuine
SPIRV-Tools identity/negative probe and validator deadlines. Compiler execution
is synchronous; a future asynchronous Metal frontend must schedule it on an
appropriate worker. Empty dispatches return an explicit no-product error.

## Verification

`python userspace/metal/test-pipeline-product.py` runs the real translator and
ACO using existing local caches. The C ABI test uses actual Apple-generated
`add_one.air` and the separately authored `affine_xor.ll`, whose body computes
`((input[tid] * 7u) ^ 0x5a5a5a5au) + tid`. Both generate real local64 and local3
products. Each retained code/metadata pair must match a separate ACO invocation
on the exact retained SPIR-V and entry. Renaming the second kernel must preserve
its generated code. The test also frees each original shader before accessing
the product and checks bounds, access masks and output-preserving failures.

The seven Rust unit tests cover planner exact-once coverage over480 grids, buffer
access contradictions, malformed inputs,14 launch-metadata corruptions, ABI
layouts, failure preservation and completed-output/deadline limits. The native
C ABI test also runs real ACO for the first variant and deliberately fails the
tail subprocess; it verifies the genuine first-code hash and no partial product.
This failure-only test emits no substitute code. Unit fixtures test parsers; successful
pipeline products always invoke the actual compiler. No CPU shader execution
or GPU output substitution is present.

The current native exports and actual C ABI execution are tested with:

```powershell
python userspace/metal/build-darwin-objects.py
python userspace/metal/test-macos-staged.py --host victor@mini --output build/metal-pipeline-native-20261011b
```

The native stage uploads only Rust objects, the existing macOS ACO executable,
SPIRV-Tools validator, tiny AIR reader and focused test inputs. It checks source
and binary hashes, links the current dylib with all compiler/dispatch/product
exports, runs the APIs on the Intel Mac and removes its unique temporary stage.
No Rust installation, Mesa source checkout, kext loading, or GPU submission is
part of this validation.

Recorded results: `build/metal-pipeline-product/report.json` passed locally;
`build/metal-pipeline-native-20261011b/report.json` passed all16 native commands
with23 current exports. Native and Windows code hashes agree for all four
kernel/local-size combinations. Peak native stage size was135,016,925 bytes;
cleanup was confirmed. Returned dylib SHA-256 is
`393c71ea8196473b6c762016a09348b1b27caca85896087d80a819983ea35072`.

## Remaining native and Metal boundary

This API does not register `MTLDevice`, create Objective-C pipeline objects,
submit PM4, or expand native code admission. The existing kext accepts only the
sealed fixed Apple-add-one fixture. It cannot execute the new local3 or second
kernel through its current transport. Next work is a real immutable pipeline
admission policy, retained multi-region VM/queue submission, buffer lifecycle
and error/completion semantics, followed by the actual Metal frontend. The
product metadata and hashes cannot establish that arbitrary ISA is safe.

The new adapter is MIT. It calls the existing LGPL translator and Mesa compiler;
their sources/notices remain separate. No NVIDIA PolyForm plugin code was copied.
