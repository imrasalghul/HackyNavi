#!/usr/bin/env python3
"""Build/test the real compiler with workspace tools or explicit tool paths.

This script does not install software, modify OpenCore, or load a kext.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def tool(name, explicit, candidate):
    path = explicit or (str(candidate) if candidate.is_file() else shutil.which(name))
    if not path or not Path(path).is_file():
        raise RuntimeError(f"Missing real {name}; supply its explicit tool argument")
    return str(Path(path).resolve())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cargo")
    parser.add_argument("--llvm-as")
    parser.add_argument("--llvm-dis")
    parser.add_argument("--spirv-val")
    parser.add_argument("--vulkan-test", action="store_true",
                        help="also dispatch through installed Vulkan ICD on physical 1002:744c")
    args = parser.parse_args()
    local = ROOT / ".tools/userspace"
    llvm = local / "src/clang+llvm-20.1.8-x86_64-pc-windows-msvc/bin"
    env = os.environ.copy()
    cargo = tool("cargo", args.cargo, local / "cargo/bin/cargo.exe")
    llvm_as = tool("llvm-as", args.llvm_as, llvm / "llvm-as.exe")
    llvm_dis = tool("llvm-dis", args.llvm_dis, llvm / "llvm-dis.exe")
    spirv_val = tool("spirv-val", args.spirv_val, local / "build-spirv-tools/tools/spirv-val.exe")
    target = ROOT / "build/metal-compiler"
    target.mkdir(parents=True, exist_ok=True)
    if (local / "rustup").is_dir() and Path(cargo).is_relative_to(local):
        env["RUSTUP_HOME"] = str(local / "rustup")
        env["CARGO_HOME"] = str(local / "cargo")
        gcc = local / "rustup/toolchains/1.90.0-x86_64-pc-windows-gnu/lib/rustlib/x86_64-pc-windows-gnu/bin/self-contained"
        env["PATH"] = str(gcc) + os.pathsep + env.get("PATH", "")
    env["PATH"] = str(Path(cargo).parent) + os.pathsep + env.get("PATH", "")
    env["CARGO_TARGET_DIR"] = str(target)
    env["METAL2VULKAN_LLVM_DIS"] = llvm_dis
    env["HACKYNAVI_LLVM_AS"] = llvm_as
    env["METAL2VULKAN_SPIRV_VAL"] = spirv_val
    if os.name == "nt" and (llvm / "llvm-dlltool.exe").is_file():
        # Rust's bundled GNU dlltool requires an external assembler. LLVM's
        # genuine implementation generates the import library directly.
        env["CARGO_ENCODED_RUSTFLAGS"] = "\x1f".join(["-C", f"dlltool={llvm / 'llvm-dlltool.exe'}"])
    report = {"scope": "AIR shader compiler only; not a macOS Metal device or kext execution test",
              "vulkan_test_requested": args.vulkan_test, "commands": [], "tools": {}, "sources": {}}
    for name, path in [("cargo", cargo), ("llvm-as", llvm_as), ("llvm-dis", llvm_dis), ("spirv-val", spirv_val)]:
        version = subprocess.run([path, "--version"], env=env, text=True, capture_output=True, check=True).stdout
        report["tools"][name] = {"path": path, "sha256": hashlib.sha256(Path(path).read_bytes()).hexdigest(), "version": version.strip()}
    for path in sorted(HERE.rglob("*")):
        if path.is_file() and "target" not in path.relative_to(HERE).parts:
            report["sources"][path.relative_to(ROOT).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    commands = [[cargo, "build", "--locked", "--manifest-path", str(HERE / "Cargo.toml")],
                [cargo, "test", "--locked", "--manifest-path", str(HERE / "Cargo.toml")]]
    if args.vulkan_test:
        commands[-1] += ["--features", "vulkan-test"]
    commands[-1] += ["--", "--nocapture"]
    library_name = ("hackynavi_metal_compiler.dll" if os.name == "nt" else
                    "libhackynavi_metal_compiler.dylib" if sys.platform == "darwin" else
                    "libhackynavi_metal_compiler.so")
    commands.append([sys.executable, str(HERE / "tests/check_c_abi.py"), str(target / "debug" / library_name)])
    log_path = target / "test.log"
    with log_path.open("w", encoding="utf-8") as log:
        for command in commands:
            result = subprocess.run(command, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            print(result.stdout, end="", flush=True)
            log.write(result.stdout)
            report["commands"].append({"argv": command, "exit_code": result.returncode})
            if result.returncode:
                break
    report["success"] = all(c["exit_code"] == 0 for c in report["commands"]) and len(report["commands"]) == len(commands)
    report["log_sha256"] = hashlib.sha256(log_path.read_bytes()).hexdigest()
    report["artifacts"] = {
        path.relative_to(ROOT).as_posix(): {"bytes": path.stat().st_size, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
        for path in sorted((target / "debug").glob("*"))
        if path.is_file() and path.suffix in (".exe", ".dll", ".dylib", ".so")
    }
    (target / "test-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0 if report["success"] else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
