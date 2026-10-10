# Metal library extraction

`metallib::Library::parse` validates and owns an input container; `functions()`
lists its exact names/stages, and `extract(name)` returns independently owned raw
AIR bitcode and metadata. Name selection is case sensitive. The compiler must
still match the selected name and stage against AIR reflection. A table record
or its SHA-256 digest does not prove executable safety or authenticity.

The parser accepts **macOS executable MTLB 2.5**, verified against SDL's
published macOS shader library, and **MTLB 2.8 with the exact macOS 15 target
tuple** produced by Apple Metal 32023.864. It validates every
function before returning the library, including functions the caller does not
select. Input is limited to 64 MiB, 4,096 functions, 1,024 UTF-8 bytes per name
and 64 tags per group. Offsets, sizes, section overlaps, duplicate names/tags,
metadata group boundaries, bitcode wrappers and all wrapped AIR SHA-256 records
are checked. Public/private metadata is retained as opaque bounded data; it is
not used for resource permissions or GPU admission.

The verified layout has an 88-byte header, contiguous sections, an empty `ENDT`
header extension, target fields zero, and mandatory `NAME`, `TYPE`, `HASH`,
`OFFT`, `VERS` function tags. Its function-list size excludes the initial
four-byte count; each function-group size includes its own four-byte size field.
Metadata-group sizes instead exclude their four-byte size field. `OFFT` supplies
three relative offsets. This layout has no `MDSZ`: sorted, distinct bitcode
offsets delimit complete wrapped AIR extents. Metadata extents must also exactly
partition their sections. This deliberately rejects aliasing or unexplained
bytes rather than guessing their meaning.

The LLVM wrapper must have magic `0x0b17c0de`, version zero, offset 20,
CPUType `0xffffffff`, an aligned raw bitstream starting `BC c0 de`, and only
zero padding to the next 16-byte boundary. The `HASH` record covers the complete
wrapper plus padding. Extraction removes the wrapper/padding without altering
the raw bitcode. This is container validation; AIR semantics are checked later.

The modern fixture was compiled from the existing authored `add_one.metal` on
the Mac using its already installed Apple Metal 32023.864 toolchain, explicitly
`-std=macos-metal2.4 -mmacosx-version-min=15.0`, then linked by `metallib`.
No GPU was opened and no installation occurred. Its 2.8 header has exact bytes
`81 0f 00 00 00` at offsets 11..15. This profile requires `MDSZ` in every function:
its eight-byte size must equal the independently bounded wrapped AIR extent,
whose wrapper, zero padding and SHA-256 are still checked. It does not authorize
overlap, shared extents, gaps or guessed target versions. Legacy 2.5 retains its
original zero-target/no-MDSZ contract.

Other MTLB versions, platforms, target profiles, library kinds, header extensions,
extra function tags, shared section extents and alternate wrapper layouts return
explicit unsupported-format or structural errors. Stages 0/1/2 map to
vertex/fragment/compute. The pinned SDL fixture exercises vertex/fragment;
the actual modern fixture exercises compute with AIR 2.7 and MSL 2.4.

`tests/fixtures/metallib/apple-32023-macos15-provenance.json` records the exact
commands, compiler version, original authored source hash, artifact hashes,
export verification and removal of the 7,331-byte temporary Mac stage. The
3,597-byte metallib SHA-256 is
`ed77099cd3e56ac404331fa68dd9401a7b326a7b7ea80d942b29b8b1fe0af865`.
Its copied 3,332-byte raw AIR SHA-256 is
`8d5c89281870b3d1f8f6868ad56f9396fdd8804acc697c59297c9f338889a13f`.
The prelink `.air` is independently retained; linking changes the actual bitcode,
so it is not falsely equated with the extracted linked module.

Local validation in `build/metal-compute-metallib-local` passed 23 Rust tests,
the prior 460 owned-ABI checks/seven SDL compiles/25 rejected compiler cases,
and 48 new actual compute-library ABI/compiler checks. The new checks mutate and
free input/parse ownership, recompile copied AIR, and compare actual translated
SPIR-V/reflection with the intact metallib path. Native validation of this parser
revision then passed all 19 commands in `build/metal-compute-metallib-native-final`,
including 460 prior owned ABI checks and 48 modern compute checks. The copied
linked AIR, intact metallib and prelink AIR all compiled on the Mac; the copied
AIR and metallib produced identical SPIR-V/reflection. The dylib exports exactly
the same 31 public symbols. Every archived source and exported artifact matched
its hash; the 158,361,660-byte stage was removed. Native CLI execution, Metal
object construction and GPU execution were not performed.

- Dylib SHA-256: `1718d24dc9a08896d50c140ef5fe418d40769404ad184d177212ef693954f943`.
- Report SHA-256: `0cb8bf4bfab7eec2f28d9ebe144fa900d04726725be711b097e63aa8e9c94ecb`.
- Post-export verification SHA-256: `cc2de1e1f444df618033ffdd90811f5726f15ed04b06d360413bd1da349aad57`.

Successful extraction/translation is not native ISA admission or GPU work.
The earlier native artifact recorded below predates modern 2.8 support and
remains preserved.

`compile(Request)` accepts the additive `InputFormat::Metallib` variant. C callers
use `HN_SHADER_METALLIB=2`; prior values 0/1 and all function signatures remain
unchanged under ABI v1. Both interfaces select the exact table name and check its
stage, then run the existing actual AIR translation, reflected-entry/descriptor
checks and SPIRV-Tools validation. A renamed table or altered table stage cannot
substitute for matching AIR. Errors contain no SPIR-V words, reflection or entry.

The CLI selects this path for the `.metallib` extension:

```sh
hackynavi-shader sdl-2.30.11-macos.metallib SDL_Solid_fragment fragment new-output-directory
```

The implementation does not expose an `MTLDevice`, submit GPU work or establish
Metal support. The dispatch/pipeline layouts and admission rules are unchanged.

## Owned library C ABI

`include/hackynavi_metallib.h` adds an independently versioned ABI v1 with an
opaque owned parse result, error/count accessors and exact-size copies of a
64-byte function record, UTF-8 name including its trailing NUL, and raw AIR.
Its stage values match `HNShaderStage` (compute=0, vertex=1, fragment=2).
Parsing uses the same bounded format implementation above. Input is borrowed
only during parsing; successful function copies survive both input mutation
and library release. An error result exposes zero functions. Failed copies
leave output unchanged. The wrapper digest describes the original wrapped
extent; it is not a digest of only the copied AIR or execution authority.

The eight new `hn_metallib_*` exports are additive. Existing shader/pipeline
v1 signatures and values are unchanged. Local evidence is in
`build/metal-metallib-owned-local-2`: strict C11/C++17 header compilation, 19
Rust library tests, 460 actual DLL ABI checks, and all seven real SDL AIR
functions compiled after freeing their parse handles and original input.
The existing compiler route, 25 negative cases and CLI checks also passed.
Independent review found no defect. Native validation in
`build/metal-metallib-owned-native-final` passed all 18 commands and the same
460 owned ABI checks. All seven copied AIR functions compiled after library
free. The actual Mach-O exports exactly the 31 expected public symbols,
including the unchanged earlier 23; every archived source and exported
artifact matched its hash. The 158,332,943-byte stage was removed.

- Dylib SHA-256: `876680e8255777987cbbe9a1c9b3acf42047a8c7281658bcb2cf7fa95ceb25cc`.
- Native report SHA-256: `f56be45bc6ab434b6cf435ef535f029522515ac529cb18fc9c720b43418987e9`.
- Post-export verification SHA-256: `edb92cdd8c5f12224d22e58d24a06bd2b7b459bc961acfad7dd7217c0b2f03d2`.

Native CLI execution was explicitly skipped, and no GPU operation occurred.
The previous native 23-export artifact is unchanged. Run local checks with
`python userspace/metal/test-metallib-owned.py --output FRESH_DIR`.

## Actual fixture and provenance

`tests/fixtures/metallib/sdl-2.30.11-macos.metallib` is the unchanged byte array
extracted from SDL release 2.30.11, commit
`fa24d868ac2f8fd558e4e914c9863411245db8fd`, from
[SDL_shaders_metal_osx.h](https://github.com/libsdl-org/SDL/blob/fa24d868ac2f8fd558e4e914c9863411245db8fd/src/render/metal/SDL_shaders_metal_osx.h).
It is 21,858 bytes, SHA-256
`7c898daf138f3d72385140b81f99578628fb6972a8dde31cc3382a0c40e2bc26`.
The adjacent source, build script, provenance JSON and exact SDL zlib license
identify its origin. The upstream build script uses Apple's `metal`, `metal-ar`
and `metallib` tools with macOS Metal 1.1, then embeds the output with `xxd`.
This repository did not regenerate or modify those shader bytes.

The seven actual entries are `SDL_Solid_vertex`, `SDL_Copy_vertex`,
`SDL_Solid_fragment`, `SDL_Copy_fragment`, `SDL_YUV_fragment`,
`SDL_NV12_fragment` and `SDL_NV21_fragment`. Their records report AIR 1.8 and
Metal language 1.1. Tests compare the exact selected bytes, names, stages, lengths
and wrapper digests, verify ownership after caller mutation, reject every
truncation, and exercise malformed headers, tables, names, offsets, wrappers and
hashes. Mutated inputs are negative/robustness tests, not hardware evidence.

Run `python userspace/metal/test-metallib.py` from the repository root. It uses
the existing local Rust/LLVM caches, runs all library tests offline with the
locked dependency graph, builds the real DLL/CLI and runs their integration
checks. It saves a hash-bound report under `build/metal-metallib/`. It performs
no remote or GPU operations.

All seven unchanged SDL functions pass actual LLVM disassembly, AIR-to-SPIR-V
translation, exact entry/stage checks and real SPIRV-Tools validation. Their
container and raw-AIR routes produce byte-identical SPIR-V/reflection. The C ABI
suite also checks 25 rejected cases, including absent/wrong names, both wrong
stages for every function, unknown input formats, damaged containers, and table
names/stages that disagree with actual AIR. Failures expose no partial shader.
CLI success matches the C ABI; CLI failures publish no output directory.
Existing raw AIR and authored LLVM text input values still compile.

SDL3 main was also inspected at commit
`ea3042350d3aac4b83ae83047185ba703d8f0a5c`: its
[actual embedded macOS library](https://github.com/libsdl-org/SDL/blob/ea3042350d3aac4b83ae83047185ba703d8f0a5c/src/render/metal/SDL_shaders_metal_macos.h)
still reports AIR 1.8/MSL 1.1 and MTLB 2.2, with no extra header-extension ENDT.
It is not evidence for the macOS 15 compiler's current container format. This
reader continues to reject it explicitly until that separate layout is added
with its own fixture and tests.

## Format sources

- [Apple library loading documentation](https://developer.apple.com/documentation/metal/mtldevice/makelibrary%28url%3A%29)
  identifies `.metallib` as the input to library creation; it does not specify
  the binary tables.
- [LLVM bitcode wrapper specification](https://llvm.org/docs/BitCodeFormat.html#bitcode-wrapper-format)
  defines the wrapper fields and raw LLVM bitstream magic.
- [MetalLibraryArchive format research](https://github.com/YuAo/MetalLibraryArchive/blob/de573ba4a7b986bbecb3e4ce2464945d266a28e9/README.md)
  documents header fields, function tags, stage values and relative offsets.
  The parser was independently written against these facts and the actual SDL
  bytes; no implementation from that project was copied.
- The pinned MIT Cobalt64 checkout references an AIR extractor in its shader
  build shell script, but the extractor is absent from that checkout. It supplied
  no parser implementation. No NVIDIA plugin code was copied.
- SHA-256 uses [RustCrypto sha2 0.10.9](https://docs.rs/sha2/0.10.9/sha2/), pinned
  directly with all transitive versions and checksums in `Cargo.lock`.
