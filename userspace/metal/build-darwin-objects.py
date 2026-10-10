#!/usr/bin/env python3
"""Cross-compile a Rust static library for native linking on macOS, with evidence.

Uses the workspace's existing Rust installation and x86_64-apple-darwin std
target. Does not contact or install anything on the Mac.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def main():
    local = ROOT / ".tools/userspace"
    cargo = local / "cargo/bin/cargo.exe"
    if not cargo.is_file():
        raise RuntimeError("This Windows helper requires the existing workspace Rust toolchain")
    env = os.environ.copy()
    env["RUSTUP_HOME"] = str(local / "rustup")
    env["CARGO_HOME"] = str(local / "cargo")
    env["PATH"] = str(cargo.parent) + os.pathsep + env.get("PATH", "")
    env["MACOSX_DEPLOYMENT_TARGET"] = "15.0"
    env["CARGO_TARGET_DIR"] = str(ROOT / "build/metal-compiler")
    for key in ("CARGO_ENCODED_RUSTFLAGS", "RUSTFLAGS"):
        env.pop(key, None)
    paths = [Path(__file__).resolve(), HERE / "Cargo.toml", HERE / "Cargo.lock"]
    paths += sorted((HERE / "src").rglob("*.rs"))
    paths += [HERE / "vendor/metal2vulkan/Cargo.toml"]
    paths += sorted((HERE / "vendor/metal2vulkan/src").rglob("*.rs"))
    sources = {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    command = [str(cargo), "rustc", "--locked", "--offline", "--manifest-path", str(HERE / "Cargo.toml"),
               "--target", "x86_64-apple-darwin", "--lib", "--crate-type", "staticlib"]
    output = ROOT / "build/metal-compiler"
    output.mkdir(parents=True, exist_ok=True)
    report = {"claim": "Rust static objects cross-compiled on Windows for macOS 15; not a native Mac Rust build",
              "argv": command, "source_sha256": sources, "complete": False,
              "hardware_executed": False, "metal_device_registered": False}
    report_path = output / "darwin-objects-report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    result = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    print(result.stdout, end="", flush=True)
    report.update({"exit_code": result.returncode,
                   "cargo_version": subprocess.check_output([str(cargo), "--version"], env=env, text=True).strip(),
                   "rustc_version": subprocess.check_output([str(cargo.parent / 'rustc.exe'), "-Vv"], env=env, text=True).strip(),
                   "log": result.stdout})
    report["snapshot_matches_worktree"] = all(
        hashlib.sha256((ROOT / name).read_bytes()).hexdigest() == digest for name, digest in sources.items())
    archive = output / "x86_64-apple-darwin/debug/libhackynavi_metal_compiler.a"
    if result.returncode == 0:
        report["artifact"] = {"path": archive.relative_to(ROOT).as_posix(), "bytes": archive.stat().st_size,
                              "sha256": hashlib.sha256(archive.read_bytes()).hexdigest()}
        llvm_ar = local / "src/clang+llvm-20.1.8-x86_64-pc-windows-msvc/bin/llvm-ar.exe"
        members = subprocess.check_output([str(llvm_ar), "t", str(archive)], text=True).splitlines()
        report["archive_member_count"] = len(members)
        report["hash_dependency_members"] = [name for name in members if name.startswith(("sha2-", "digest-", "crypto_common-", "block_buffer-"))]
        report["sha2_objects_present"] = any(name.startswith("sha2-") for name in members)
    report["complete"] = True
    report["success"] = (result.returncode == 0 and report["snapshot_matches_worktree"]
                         and report.get("sha2_objects_present", False))
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0 if report["success"] else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as error:
        print(error, file=sys.stderr)
        sys.exit(1)
