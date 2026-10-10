# Actual compute Context test

`hackynavi-context-smoke` exercises the production `compute::Context`,
`Buffer` and `Pipeline` through the real v1.4 IOKit client. It requires
`--run`; argument parsing and `--help` do not open a GPU. This is a target-only
compute test, not Metal device registration or rendering.

Use the Navi31 machine with an engine-enabled v1.4 kext. The display-capture
diagnostic package cannot provide this interface. Build and export this tool
with the runtime native build script; do not execute it on the build Mac.
The tool links the actual compiler C ABI, so its matching compiler dylib must
be available to the macOS loader. ACO, AIR reader and SPIR-V validator are
separate real tools. The build exports do not replace those dependencies.

On the target, use absolute paths:

```sh
./hackynavi-context-smoke --run \
  --aco /absolute/path/hackynavi-aco \
  --llvm-dis /absolute/path/apple-air-dis \
  --spirv-val /absolute/path/spirv-val \
  --input /absolute/path/add_one.air --format air --check add-one
```

Use the actual `userspace/metal/tests/fixtures/apple/add_one.air`, or supply
`userspace/metal/tests/fixtures/affine_xor.ll` with `--format llvm --check
affine-xor`. Entry names default to `add_one` or `affine_xor`; `--entry NAME`
selects a renamed supplied kernel. `--registry ID` selects a particular GPU
service if needed. `--check` changes only CPU comparison arithmetic and never
substitutes shader code. Affine output is
`((input[tid] * 7u) ^ 0x5a5a5a5au) + tid`, with unsigned 32-bit wraparound.

Each invocation compiles the supplied input through the linked compiler and
real ACO for 4,099 exact threads, nominal local size 64. It captures an immutable
`Pipeline`, frees the original shader and compiler-product handles, then
checks its actual local64/local3 metadata and code against the original bytes.
The kernel independently validates uploaded ISA; metadata is not admission.

The test allocates persistent 20 KiB A, B and C buffers. It uploads A plus
distinct B/C poison values and guard words once, before the first dispatch.
The first job computes A→B; the test downloads and checks B. The second job
uses that same B `Buffer` object as its input and writes C. There is no CPU
data upload after the first dispatch. The wrapper uploads immutable shader
staging as part of each dispatch, while the data allocations remain resident.

For add-one, B must equal A+1 and C must equal A+2. For affine-xor, C must
equal two applications at the same global thread index. B after the second
job must still match the first readback. Every comparison covers 4,099 active
values and all 1,021 suffix guards; C's poison/guards differ from B's at every
word. Both completion observations must have the original nonzero connection
epoch, and the second sequence must be greater than the first.
All three reserved GPU addresses must be valid, nonoverlapping and unchanged
across both jobs; the tool records B's retained address.

The tool explicitly releases B and requires a subsequent download to return
`NotFound` without changing its destination. It checks each explicit buffer
release and native context close before printing PASS, including cleanup
after an earlier failure. Compilation, allocation, admission, dispatch,
readback, guards, lifetime or close errors produce a nonzero exit status.
No CPU arithmetic fills a GPU result or replaces a failed operation.

Native target execution remains unverified until this exact tool passes on
the 7900 XTX. Compilation and linking alone do not establish repeated GPU
execution, engine recovery, asynchronous scheduling, shared/managed Metal
storage, presentation or Metal device discovery.
