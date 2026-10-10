# Actual fragment-buffer native draw

Updated 2026-10-11. This is implementation and offline/build evidence. No target
macOS shader execution, Metal device, display takeover or presented frame has
been demonstrated.

## Implemented contract

The genuine Apple fixture returns `constant float4& color [[buffer(0)]]`.
Actual AIR and metallib translation produces four scalar loads; real Mesa ACO
produces a 72-byte RDNA 3 program, with SHA-256
`f6680fdaa64d6e8d296ab163205c5727d29c715850dc02c32ed230e6b02046dc`.
The separate compiler and immutable pipeline preserve actual reflection: one
read-only buffer, Metal index zero, and the exact static 16-byte span.

The kernel independently decodes those instructions. It admits only the bounded
four DWORD loads, required memory wait and correctly ordered RGBA export under
the exact stage configuration. It rejects altered addressing, descriptor writes,
missing waits and invalid export provenance. The existing resource-free shader
profile is unchanged. No constant color is inferred for a GPU buffer load.

The new 720-DWORD stream derives its four-word RAW descriptor from a retained
native read mapping. The user never supplies a descriptor or GPU address.
Mappings must share the actual owner and epoch; code, target, attribute ring,
indirect buffer and fragment resource must have distinct storage and disjoint
complete GPU ranges. The 16-byte fragment window is aligned and lies within
the actual mapping. DMA/CPU aliases and changed capture generations are checked.
The fragment data BO remains alive through actual queue completion; it is not
treated as disposable private command storage.

Native protocol v1.6 adds one exact capability bit and selector 17. Its 160-byte
request embeds the old graphics request plus a fragment handle, BO-relative
offset and exact 16-byte length. Old v1.5 requests/layouts remain supported;
old profiles cannot invoke the new selector. IOUserClient copies and validates
the wire record before the session captures real native storage.

Context retains the immutable pipeline, render target and fragment Buffer. It
checks the logical buffer bounds and owning context before allocation, uploads
actual shader bytes, maps the actual fragment storage read-only, submits the
native request, waits for completion, releases history, and unmaps resources.
Failures after mutation close the context through existing native containment;
there is no CPU color substitution or automatic retry.

## Evidence

| Component | Retained report | Result |
| --- | --- | --- |
| Actual Apple compiler | `build/gfx11-fragment-buffer-tests-4` | 873 offline compiler/ISA checks |
| Kernel admission/draw | `build/gfx11-fragment-buffer-admission-local-1` | Strict/UBSan tests, real Mesa descriptor oracle, kernel objects |
| Retained capture | `build/native-buffer-capture-local-2` | Five real kernel-header objects, independent review |
| Native wire/session | `build/native-buffer-wire-local-1` | Three real kernel-header objects, independent review |
| Actual IOKit transport | `build/native-buffer-transport-1` | Native compile/link, 160,054 ASan/UBSan protocol checks, stage removed |
| Immutable runtime | `build/runtime-graphics-buffer-local-1` | 7,328 strict and UBSan checks each, independent review |
| Actual Context | `build/context-buffer-darwin-local-1` | Darwin object cross-compilation; subsequent native frontend build passes |
| Metal frontend | `build/metal-buffer-native-1` | 22 actual units compile/link, three target tools compile only; stage removed |
| Development bundle | `build/metal-buffer-bundle-package-1` | Exact sources/fixtures/binaries packaged; package checks pass |
| Native compiler library | `build/graphics-buffer-darwin-native-1` | 88 compiler checks, 12 actual Apple products; exact four C exports, stage removed |
| Real POSIX pipeline builder | `build/graphics-buffer-runtime-native-1` | 54 ASan/UBSan checks, four actual pipelines; six instrumented objects, stage removed |
| Complete development kext | `build/macos-fragment-buffer-integration-1` | 109 units, 414 imports, 271 verified source inputs; stage removed |

Complete kext executable SHA-256:
`c39afcee43f96070f8a230f11ff890360cc6edbe2bc2bb7ab67ef743b9560180`.
UUID: `100496F0-532E-374A-B4A4-3D6E0D61E3D0`.
The independent archive/binary verification report is
`build/macos-fragment-buffer-integration-1/product-wire-independent-verification.json`.
Unsigned ownership and SDK/libkmod deployment diagnostics remain recorded; no
loading or target readiness is implied.

## Current integration and remaining work

Actual native compiler-library validation and the real POSIX pipeline builder
passed on mini without opening a Context or instantiating a Metal object.
The compiler stage peaked at 215,811,700 bytes; the runtime stage peaked at
63,100,054 bytes. Both were removed after verified export. Actual compiler
libraries reused by the runtime test were not sanitizer-instrumented.
The Metal frontend now connects exact v1.6 negotiation, fragment buffer setters,
immutable command snapshots, preflight identity/bounds checks, and Texture's
real Context draw; all 22 units compile/link on mini and independent source
review passes. A separate target-only public Metal smoke must verify two
colors and offset snapshots through actual GPU readback; it will only be
compiled on mini. It is now compiled and included in the development package,
without execution or installation.

This bounded profile is one position-only triangle and one fragment-buffer color
into a linear RGBA8 target. General shaders/resources, truthful Apple runtime MSL
source compilation, display/presentation, complete startup/recovery and real
target execution remain required for an accelerated driver. The issued passive
diagnostic package is unchanged by this development build.
