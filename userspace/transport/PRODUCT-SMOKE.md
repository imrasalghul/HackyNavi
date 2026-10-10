# Supplied-source native compute test

`hackynavi-product-smoke` compiles a supplied AIR or LLVM IR kernel through the
actual owned pipeline runtime and Mesa ACO, then submits the freshly returned
machine code through the v1.3 IOKit product interface. It contains no stored
shader binary or replacement kernel. `--check` selects only the expected-output
arithmetic used to verify returned GPU bytes.

This requires the Navi31 test machine running an engine-enabled kext that
exposes the v1.3 product profile. The display-capture diagnostic kext does not
provide that interface. This is a compute test, not Metal device registration.

`build-native.py` compiles and exports the executable with the other transport
artifacts. It runs only portable protocol tests on the build Mac; it never
executes this program or submits GPU work there. The runtime, ACO and translator
tools are separate existing artifacts and are not included in the transport
archive.

On the Navi31 test machine, supply absolute paths to all files:

```sh
./hackynavi-product-smoke --run \
  --library /absolute/path/libhackynavi_metal_compiler.dylib \
  --aco /absolute/path/hackynavi-aco \
  --llvm-dis /absolute/path/apple-air-dis \
  --spirv-val /absolute/path/spirv-val \
  --input /absolute/path/add_one.air --format air --check add-one
```

The actual Apple-generated source is
`userspace/metal/tests/fixtures/apple/add_one.air`. The existing `apple-air-dis`
artifact is the Apple AIR reader used by the native compiler validation; a
compatible real `llvm-dis` executable may also be supplied through this option.
For the separately authored second kernel, select the actual
`userspace/metal/tests/fixtures/affine_xor.ll` file, `--format llvm` and
`--check affine-xor`. Its expected output is
`((input[tid] * 7u) ^ 0x5a5a5a5au) + tid`, with unsigned 32-bit wraparound.
Entry names default to `add_one` or `affine_xor`; `--entry NAME` permits a renamed
input kernel without substituting its code. `--registry ID` optionally selects
a specific GPU service.

Each invocation compiles 4,099 threads at nominal local size 64. It requires the
actual compiler to return separate local64 and local3 variants and the exact
full/tail dispatch regions. It copies their owned code, frees the original
shader and pipeline handles, allocates unmapped staging BOs, maps distinct data
BOs with R and RW access, and uploads input plus poisoned output. Kernel code
admission independently decodes the copied ISA and builds private descriptors
and commands; caller RSRC values or resource masks do not cross the wire.

PASS requires a real GPU-written completion, immutable native readback matching
all 4,099 active values and all 1,021 suffix guard words, and successful explicit
native close. Allocation, compilation, admission, timeout, transport, readback,
guard or close failures return nonzero. CPU arithmetic computes expected values
only; it cannot fill the GPU result or replace a failed dispatch.

Native target execution remains unverified until this program actually passes
on the 7900XTX. Cross-compilation or a successful Mac link does not establish it.

## Retained buffers across two real jobs

Add `--reuse` to the same command to select the explicit v1.4 reusable product
profile. This requires a kext and transport built with that profile; v1.3 and
the capture-only diagnostic cannot run this mode. Its current source has been
cross-compiled; native linking and target execution are still pending.

The test compiles the supplied program once into its real local64 and local3
variants. It allocates A, B and C plus unmapped code staging. All initial input,
output poison and guards are uploaded before the first submission. Job one
computes A→B. After its real completion and native job cleanup, the test unmaps
A, changes B from the output mapping to the input mapping, maps C as output,
and submits the same actual compiled pipeline for B→C. There are no CPU writes
after the first submission; the first GPU-written B allocation supplies the
second job's input.

For `add-one`, C must contain A+2. For `affine-xor`, C must equal two successive
applications of the actual kernel arithmetic at the same global thread index.
Both jobs cover all 4,099 active elements and preserve all 1,021 suffix guards;
C has different guard values from B. The test also reads the retained B and C
BOs, reads job one's historical output after job two, and checks that releasing
job one's result makes its saved fence return `NotFound` without invalidating
job two's history. It releases both result records, explicitly unmaps/frees
all data and code staging allocations, and checks native close before PASS.

This checks serialized buffer persistence and result lifetime through the real
IOKit path. It does not test asynchronous overlap, CPU-mapped Metal buffers,
general command buffers or Metal device registration.
