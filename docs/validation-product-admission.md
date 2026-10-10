# Native compute product integration validation

Completed 2026-10-11. This records source and native build validation, not
target GPU execution. The implementation contract is in
[compute-product-admission.md](compute-product-admission.md).

The GFX1100 leaf validator decodes the actual copied instructions and derives
resource access and launch registers. The dispatch builder generates bounded
full/tail regions, descriptors, parameters and command packets from validated
variants. The native VM capture copies code into privately owned DMA, resolves
real buffer handles, checks all resident buffer and page-table CPU/DMA ranges
for aliasing, and retains the sealed controls through the existing queue,
completion, result and retirement path. The new internal API has no general
Session/IOUserClient wire exposure yet.

## Verification

Independent validator and native capture reviews found no concrete defects;
reports are `build/gfx11-leaf-independent-review.json` and
`build/native-product-review/review.json`. The exact native interval helper
passed 822,327 cases against an independent overlap oracle. Build integration
is recorded in `build/native-product-review/build-integration.json`: native
and cross source lists match, and fresh Windows CMake/CTest passed both new
suites with 84,007 and 120,732 checks respectively.

`build/native-product-admission-final` ran native macOS ASan/UBSan suites:

| Suite | Assertions | Failures |
|---|---:|---:|
| GFX1100 instruction validator | 84,007 | 0 |
| Multi-region dispatch builder | 120,732 | 0 |
| Forced-on boot display retirement regression | 22,512,972 | 0 |

The same job compiled six actual x86_64 kernel objects: VMContext, VMProduct,
VMCompletion, GraphicsQueue and the two portable product modules. All 61
source hashes matched after export. The 2,384 KiB temporary stage was removed.
The sanitizer suites execute portable algorithms; they do not run kernel DMA
allocation, IOKit lifecycle callbacks or GPU work.

`build/macos-product-admission-final` then built the complete development
kext: **91 translation units**, all **406 imports resolved** on the build
Mac. All 233 archived source files (4,867,079 bytes) matched the manifest and
current workspace. The 9,688 KiB temporary stage was removed. Build host:
Intel macOS 15.8 / 24H22, Apple Clang 17, SDK 26.2, deployment baseline 15.0.
The intended target remains macOS 15.8.1 / 24H32.

- Executable SHA-256: `8d31eb8847b05eab297426994fafc1d2fa12bfaf01f6e4f3fa5681587ba74751`.
- Mach-O UUID: `7CF4B810-AE8E-3B67-ADD6-19506B9ED81F`.
- Source archive SHA-256: `cbd04eda6d77ef7cb489d17a96ad129ebad3f4af351e4bd9885a54353577fed4`.
- Artifact archive SHA-256: `233f59f0a8f23a99e3561c617bc3ceea71c66e947890f0c7224f7fe80c0a93be`.

The full no-load diagnostic and library check exited 0. The former still
reports ownership 501:20 rather than 0:0 on the staged bundle; the linker
warns that the SDK's libkmod start/stop objects target a newer macOS than
15.0. These are retained verbatim in `build.log`, `build-report.json` and
`validation-summary.json`. No kext was loaded on the mini or target.

## Remaining work and test separation

The general product needs a versioned bounded Session/IOUserClient boundary,
matching compiler/frontend transport, actual target submission/readback and
recovery verification. Metal device registration, rendering, presentation and
controlled display output remain incomplete. Compiler output accepted by a
validator and a successful native link do not establish any of those results.

The next target capture remains the separately frozen
`HackyNavi-live-capture-dbbcb5ec0ba9` package described in
[the power capture record](validation-2026-10-11-power-capture.md). It contains
the narrow display capture changes and excludes this development integration.
Do not substitute the mutable development bundle for that user test.
