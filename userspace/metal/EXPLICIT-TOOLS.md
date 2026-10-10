# Explicit compiler tools for the embedded driver

`hn_shader_compile_with_tools` adds an explicit-tools path while preserving the
existing shader ABI version and all existing exported functions. The descriptor
is `HNShaderTools` version 1, with an exact `struct_size` and two required
UTF-8 absolute paths: the actual AIR disassembler and SPIRV-Tools validator.
Its x86_64 C layout is 24 bytes. A null, incompatible, empty, relative,
nonregular, nonexistent, or nonexecutable path fails with an owned error result.
Unix executable permission checks precede launching; ordinary launch failures
still return the actual diagnostic. Both paths are required for every input
format, including LLVM text.

Rust canonicalizes and copies both names into an immutable per-compile context.
That context reaches the actual AIR reader, the validator's identity and invalid
module probes, the translator's constructed-module validation, and the adapter's
final validation. It is retained by value in the successful shader. Later
`hn_pipeline_build` uses the same retained validator before passing actual SPIR-V
to the separately supplied absolute ACO executable. Caller path strings and the
descriptor may be destroyed as soon as compilation returns.

There is no environment mutation, thread-local override, PATH search, or
fallback to environment-selected tools in this path. Different contexts may
compile concurrently. The legacy `hn_shader_compile` entrypoint still supports
standalone callers that configure the environment before starting threads.
Compiler transformation/debug flags in the upstream translator are separate
from tool selection; this change does not remove those existing controls.

The C++ runtime exposes `Shader::Tools` and an explicit `Shader::compile`
overload. Embedded callers pass their own validated bundle's `apple-air-dis` and
`spirv-val` helper paths. The Library/Device layer owns bundle containment checks;
the generic compiler API does not pretend every absolute path belongs to a
bundle. No process-global configuration is required when a plugin is loaded
after the application's threads have started.

## Lifetime and installation contract

The compiler directly launches the exact canonical executable path with an
argument vector and null stdin, without a shell. Existing timeouts remain;
direct children are killed/reaped on timeout or failed waits. This API runs
trusted installed tools and is not a sandbox for arbitrary executables or their
descendants. Paths are copied, but executable file descriptors, code signatures,
and file identities are not pinned. The caller must own the installation and
keep those executables immutable throughout shader and pipeline compilation.
The canonicalization-to-launch interval is therefore an explicit installation
ownership prerequisite, not a claimed TOCTOU protection.

Ordinary failure and panic paths return the existing owned error-result form:
nonempty diagnostic, zero code, empty reflection, and disposal by
`hn_shader_result_free`. Existing raw C pointer readability/lifetime requirements
still apply. No partially translated shader is published on tool failure.

## Local evidence

`build/metal-explicit-tools-local/report.json` records 23 passing Rust tests,
94 actual explicit-tools C ABI checks, and the legacy C ABI regression. The new
suite starts a child with deliberately invalid PATH and both tool environment
overrides before creating threads. It compiles actual Apple AIR, the captured
macOS15 metallib, and authored affine IR, then overwrites caller paths/input and
builds real full-64/tail-3 ACO variants. Four overlapping host calls verify that
different contexts remain independent. Wrong real tool binaries are rejected;
no fake validator or generated success-only executable is used.

`runtime-report.json` records 27 passing checks against the actual C++ overload,
retained shader tools, owned ACO products, and error/output preservation with
the same deliberately unusable environment. Neither test opens a Context,
instantiates a Metal device, or submits GPU work.

## Native evidence

`build/metal-explicit-tools-native-final-2/report.json` records the real x86_64
Darwin library link and 22 successful native commands. The exact exported
Mach-O surface contains all previous 31 functions plus
`hn_shader_compile_with_tools`. The 94 explicit-tools checks passed on the Mac,
and the actual C++ overload/lifetime harness passed 27 checks under
AddressSanitizer and UndefinedBehaviorSanitizer with unusable environment tool
paths. Existing compiler, owned-library and actual macOS15 metallib checks also
passed. LeakSanitizer is disabled because the measured Apple runtime does not
support it; the earlier pre-main runner failure is preserved separately.

`post-export-verification.json` binds the input archive, compiler sources,
exported library/helper hashes and exact dynamic export trie. Peak observed
stage size was 159,225,026 bytes, and the remote stage was removed after confirmed
process termination and artifact export. This is native compiler and ownership
evidence; it does not establish Metal device loading or GPU execution.
