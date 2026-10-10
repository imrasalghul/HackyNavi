# Local translator changes

The original upstream identity, license and original-file SHA-256 values remain
in `source-manifest.json`. The translator remains LGPL-3.0-or-later; its license
and source are retained. Current build reports bind the patched source bytes.

HackyNavi changes only `metal2vulkan/src/tools.rs` and `src/lib.rs` for explicit
embedded compiler tools. `ToolPaths` canonicalizes trusted absolute executable
paths and owns the resulting names. New translation and validation entrypoints
carry an immutable reference to those paths through real AIR disassembly and
SPIR-V validation. They do not set environment variables, use thread-local
overrides, or fall back to PATH. Legacy standalone entrypoints retain their
original environment/tool search behavior.

Child processes now receive null stdin, and direct children are reaped after
timeout/failed waits before returning. This is execution of trusted installed
tools, not a sandbox for arbitrary programs or a guarantee about descendants
spawned by a replaced executable. Canonical paths do not pin file identities;
the embedding application owns the installation and must keep the executables
immutable through all shader and pipeline compilation calls.
