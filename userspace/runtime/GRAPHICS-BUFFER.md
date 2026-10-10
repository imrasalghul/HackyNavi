# Actual fragment-buffer CPU pipeline path

`GraphicsShader::compileFragmentBuffer` is an additive factory for one real
read-only constant float4 at Metal buffer0. It invokes the existing actual owned
compiler with explicit tool paths, then admits the exact version56 reflection:
set0/binding0/count1, address space2, declared/Object size16, one static range
`[0,16)`, ReadOnly, float4 and no other resources or stage inputs. The immutable
binding accessor reports these admitted indices/range and the native profile's
required16-byte alignment. No caller-supplied JSON or profile flag can construct
the object. The original `compile()` remains resource-free.

`buildFragmentBufferGraphicsPipeline` writes that object's real SPIR-V and the
real resource-free vertex SPIR-V into a private stage. It calls actual NGG and
the additive fragment-buffer C ABI under the **same** Mesa mutex as the old
builder, reads bounded regular machine-code files, and calls the independent
`validateFragmentBuffer` path on copies of those actual bytes. It does not use
compiler JSON as code or state authority. Known temporary files are removed
before successful output publication and on failure through the existing owner.

The returned `FragmentBufferGraphicsPipeline` is a distinct immutable type with
no conversion to `GraphicsPipeline`. The existing resource-free `Context::draw`
cannot accept it. Its image is owned, zero-padded, and remains unchanged when
input bytes are destroyed or rejected output aliases/lengths are supplied.
Native source-buffer ownership, transport negotiation and submission are
separate work; these CPU objects confer no GPU access or allocation authority.

Local validation in `build/runtime-graphics-buffer-local-1/report.json` passed
7,328 checks under strict compilation and UBSan separately, using actual Apple
AIR/metallib, the actual owned compiler, and independently captured actual
VS/PS code. The tests cover exact reflection mutations/truncations, owned result
lifetime, component metadata, image ownership/padding, incorrect ranges/aliases,
actual instruction mutations and mutual old/new profile rejection. The three
changed runtime units cross-compile for x86_64 macOS15. The original resource-free
shader regression passed 5,248 checks in each configuration.

The full POSIX builder passed actual native validation in
`build/graphics-buffer-runtime-native-1/report.json`: six authored objects compiled
with ASan/UBSan, and 54 checks passed across four actual pipelines (resource-free
and buffer-reading, each from genuine Apple AIR and metallib). Each complete
owned image matched the independently captured machine bytes. The checks also
cover exact binding metadata, mutually rejected profiles, destroyed input/shader
lifetimes and unchanged outputs on failure. The seven exported artifacts and
snapshot were rehashed in `post-export-verification.json`; the 63,100,054-byte
remote stage was removed.

The dependency in `build/graphics-buffer-darwin-native-1/report.json` exposes the
four actual graphics C ABI symbols and passed 88 checks covering 12 CLI/C ABI
stage products, real descriptor boundaries and valid unsupported programs. Its
buffer-reading outputs retain the actual 72-byte shader hash
`f6680fdaa64d6e8d296ab163205c5727d29c715850dc02c32ed230e6b02046dc`.
No Context, Metal object or GPU was opened by these tests, and no rendered output
is claimed. The prior local freeze remains historical evidence for its exact
source and documentation snapshot.
