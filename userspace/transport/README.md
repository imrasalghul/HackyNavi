# Native IOKit transport

The v1.1 built-in profile exposes the driver's real, isolated clear/copy path.
It is a driver test interface, not Metal registration or arbitrary shader/PM4
submission. `Client::openBuiltIn()` requests `BuiltInCompute`, `CopiedBufferIO`
and `SealedSingleJob` plus actual memory, fence and retirement capabilities.
`Client::open()` retains the original generic requirements and v1.0 handshake;
it fails with Unsupported when the driver cannot provide those requirements.

Each connection owns a kernel-issued epoch and generation-checked buffer
handles. Allocate system buffers, map isolated GPU VAs, upload bytes with
`write()`, and submit exactly one `clear()` or `copy()`. I/O uses copied inline
messages of at most 2048 payload bytes; no CPU mapping, physical address,
register access or raw command buffer crosses this interface. `wait()` reports
the actual completion recorded by the session; `readResult()` returns the
immutable kernel copy of the GPU result. The sealed connection cannot allocate,
map, write or launch a different job afterward. Open another connection for
another job.

The explicit v1.2 compiler profile, `Client::openCompiler()`, additionally
requires `CompilerFixtureCompute`. Its `compilerAddOne()` method executes the
exact pinned `AppleAddOneGfx1100V1` ACO product from
`userspace/compiler/fixtures/apple-add-one/`: Apple AIR translated to SPIR-V,
then compiled by Mesa ACO for gfx1100. The native owner supplies immutable code,
descriptors, push constants and commands; the client sends only two owned buffer
handles and offsets. Each buffer must contain a 16,640-byte range, comprising
4,096 active uint values and 64 guard words. Source and destination must be
distinct buffers. This is one fixed compiled shader class, not arbitrary AIR or
ACO loading, and does not register a Metal device. The existing v1.1 profile
remains separately negotiated.

`close()` returns the explicit native Close RPC result before releasing the
connection. This matters because XNU's ordinary `IOServiceClose` discards the
driver's `clientClose()` return value. Failed exposed resources remain owned
by the kernel retirement machinery. The connection is bound to its opening
task; ordinary users can open the bounded profile, but transferring the Mach
connection port to another task does not transfer that authority.

Build on the development Mac without executing GPU work:

```text
python userspace/transport/build-native.py --host victor@mini --output build/native-transport-unique
```

The script compiles the library, probe and both smoke tools, runs only host protocol
ASan/UBSan tests, exports `artifacts.tar.gz`, and removes its small unique remote
stage. It does not load a kext or execute any target program.

On the Navi31 target, after the matching engine-enabled kext has completed
startup and its user client is available:

```sh
./hackynavi-transport-probe --built-in
./hackynavi-compute-smoke --run
./hackynavi-transport-probe --compiler
./hackynavi-compiler-smoke --run
```

An optional final registry-entry ID selects a specific device. The smoke tool
performs copy and clear in separate real sessions, checks zeroed allocations,
uploads data, waits for GPU completion, compares all 1024 result words per job,
and checks explicit close. An error or mismatch returns nonzero. It has no
software fallback. This tool has been compiled; target GPU execution must be
reported separately.

The compiler smoke tool opens an explicit v1.2 connection, checks two zeroed
20 KiB allocations, maps the source read-only and destination read/write, and
uploads patterned inputs plus poisoned output and guards. It checks all 4,096
GPU `input + 1` results, including unsigned wraparound, and all 64 untouched guard
words through the actual 16,640-byte readback. It requires a GPU completion and
successful explicit close. Any failure exits nonzero; there is no CPU fallback.
`--run` is mandatory, and this command is for the Navi31 test machine only.
