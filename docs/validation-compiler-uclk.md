# Native compiler dispatch and UCLK ownership integration

The combined development build in `build/macos-compiler-uclk-final` compiles
86 translation units and resolves all 406 imported kernel symbols on
`victor@mini`. All 225 recorded source hashes matched the working files after
export. The 9,452 KiB temporary remote stage was removed. No kext was loaded,
no GPU smoke tool was run, and no Metal device or controlled display output
was established by these checks.

## Exact build

| Item | Value |
| --- | --- |
| Executable SHA-256 | `932b945bf9264e6f94c2e7b4e509282c54ac5742abd45ee19d98436603b0e857` |
| Executable size | 2,976,848 bytes |
| Mach-O UUID | `C480114D-839D-3EDE-86CA-1FC94CC3C858` |
| Source archive SHA-256 | `525e655eea066db3a280a95bc892c16c7ae0fe710e8bedfc268ff095520a7fde` |
| Uncompressed source | 4,734,623 bytes, 225 files |
| Build report SHA-256 | `5521770ec1897b89d5fd36c2bedfe6b7679af20534b0b4ec52d720d83bacb814` |
| Build host | Intel macOS 15.8 / 24H22, Apple Clang 17, SDK 26.2 |
| Runtime target | x86_64 macOS 15.8.1; deployment baseline 15.0 |

`build-report.json`, `build.log`, `integration-validation.json`, and
`host-integration.log` preserve the evidence. `kmutil print-diagnostics`
returned zero but reported build-folder ownership 501:20 instead of 0:0;
SDK libkmod deployment warnings are also retained. This is a native build and
symbol-resolution result, not a target loader test.

## Implemented compute path

The exact 104-byte Apple AIR-to-SPIR-V-to-ACO product is now a sealed native
shader class, `AppleAddOneGfx1100V1`. The build verifies its pinned artifact,
metadata and generated header locally and again in the remote stage. The
kernel owns separate code/IB/parameter pages and resolves actual retained
source/destination mappings. Code and IB are RX; parameters are R; source is
R and destination RW. Canonical class validation precedes publication.

Explicit ABI v1.2 exposes selector 12 and compiler capability negotiation.
Task-bound Session/IOUserClient use one actual FunctionGroup/GART scope through
capture, queue submission, GPU fence/RPTR observation, copied readback and
normal completed-VM retirement. Only then is a result sequence published.
Global retained-result accounting and exposed-resource failure retention are
preserved. The v1.1 clear/copy profile remains separately negotiated.

The compiled target-only tool `hackynavi-compiler-smoke --run` checks two
zeroed 20 KiB allocations, poisoned output, 4,096 GPU add-one results including
unsigned wraparound, 64 untouched guard words, actual completion and explicit
close. It has not run. Tools are in
`build/native-transport-compiler-v12/artifacts.tar.gz`. This path still lacks
arbitrary shader admission, reusable general pipelines, Metal resource and
encoder objects, MTLDevice registration, IOSurface and presentation.

## Implemented clock path

The DAL owner now requests a real UCLK minimum using PPCLK2, preserves its raw
reply without inventing units, and requires fulfillment bits `0x24`. A private
journal precedes the possible command write and retains ambiguous failures.
Its native entry is restricted to Prepared, an actual normal scope/deadline,
fresh DMCUB disabled/reset readbacks, physical idle pipes and no locally owned
DTO/link consumers. Actual DPM inventory constrains the numeric request; it
is not a measured frequency or a clock hold.

Scoped boot preserves the generation-bound fulfilled hold and avoids a second
self-retain. Engine now passes its existing actual scope to display boot.
There is no UCLK activation callsite yet. Normal stop refuses to discard a
possible UCLK constraint because zero-MHz withdrawal is not source-proven;
the existing verified device-reset retirement path remains required. This
does not establish SOC/Fabric holds, metrics, FAMS coordination, a trusted
DML plan or a complete modeset transaction.

## Checks and review

- Clean Windows build and six targeted suites passed: native ABI, compiler
  fixture, isolated compute, queue, display SMU and DPM (23.66 seconds total).
- Fixture artifact verifier and 13 rejection/determinism checks passed.
- Exact shader/descriptor/PM4 tests: 81,217 ASan/UBSan checks plus native kernel
  object compilation, `build/native-gfx-aco-fixture/report.json`.
- Native VM/queue final objects: `build/native-compiler-vm-final/report.json`;
  preceding isolated/queue sanitizer suites passed 6,965 and 5,156 checks.
- Session/UC/protocol native objects:
  `build/native-userclient-compiler-v12/report.json`.
- Userspace protocol: 135,829 ASan/UBSan checks; library/probe/both smoke tools
  compiled, `build/native-transport-compiler-v12/report.json`.
- UCLK/legacy SMU and DPM: 9,007 and 725,284 sanitizer checks;
  `build/display-uclk-owner-report.json` records real kernel-header checks.
- Independent source reviews found no concrete defect in compiler VM/queue,
  Session/UC or UCLK ownership; see `build/compiler-vm-report.json` and
  `build/display-uclk-peer-review.json`. Native allocator fault injection and
  target recovery behavior were not exercised.

## Next work and target package

Parallel work continues on a real translator dispatch-planner C ABI and the
concrete Metal discovery/frontend boundary, plus PPSMC SOC/Fabric requests
and metrics transfer with native firmware/allocation ownership. Those later
files are not evidence from this frozen build.

The current target request is still
`build/releases/HackyNavi-live-capture-1e4040229efa.zip`, whose 14 manifest
files were reverified in both ZIP and extracted directory. Follow its own
HANDOFF.md with `-hackynavi-device` and existing `-radvesa`, without
`-hackynavi-engines`. It obtains the raw DOMAIN0 power value omitted by the
earlier successful HDMI capture. It is a distinct immutable diagnostic build,
not this mutable development kext and not an acceleration release.
