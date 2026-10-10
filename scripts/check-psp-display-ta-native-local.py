#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real kernel-header compilation of native display-TA and reset ownership.

This is compilation and a frozen source record, not an IOKit runtime test.
Use check-psp-display-ta-local.py separately for the executable wire/fault tests.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ["kext/HackyNavi/" + name + ".cpp" for name in (
    "HackyNaviFirmwareController", "HackyNaviFirmwareTA",
    "HackyNaviGARTController", "HackyNaviDisplayController")]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    if out.exists():
        parser.error("Use a fresh evidence directory")
    out.mkdir(parents=True)
    compiler = ROOT / ".tools/zig-x86_64-windows-0.17.0/zig.exe"
    base = [compiler, "c++", "-target", "x86_64-macos.15.0", "-std=c++14",
            "-mkernel", "-fapple-kext", "-DKERNEL", "-DKERNEL_EXTENSION",
            "-nostdinc", "-nostdlibinc", "-I.research/MacKernelSDK/Headers", "-Iinclude",
            "-fno-exceptions", "-fno-rtti", "-fno-builtin", "-fno-common",
            "-fno-stack-protector", "-mno-red-zone", "-mno-mmx", "-mno-sse", "-mno-sse2",
            "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
            "-Wframe-larger-than=2048", "-O2", "-fstack-usage"]
    report = {"success": False, "target_executed": False, "remote_executed": False,
              "gpu_executed": False, "compiler_sha256": sha(compiler),
              "validation_script_sha256": sha(Path(__file__)), "commands": [],
              "sources": {p: sha(ROOT / p) for p in SOURCES}, "dependencies": {},
              "artifacts": {}, "stack_usage": {},
              "scope": "Real x86_64 macOS kernel objects; no native object instantiation, linking, or hardware execution"}

    def run(command):
        command = [str(p) for p in command]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=120)
        report["commands"].append({"args": command, "exit_code": result.returncode,
                                   "stdout": result.stdout, "stderr": result.stderr})
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def dependencies(path):
        names = path.read_text().replace("\\\n", " ").split(":", 1)[1].split()
        return {name: sha(ROOT / name) for name in names}

    try:
        # Freeze all dependency closures before any object compilation so a
        # header edit between translation units cannot quietly mix epochs.
        for source in SOURCES:
            name = Path(source).stem
            dep = out / (name + "-before.d")
            run([*base, "-M", "-MT", "kernel.o", "-MF", dep, source])
            report["dependencies"][source] = dependencies(dep)
        for source in SOURCES:
            name = Path(source).stem
            obj, stack, dep = (out / (name + suffix) for suffix in (".o", ".su", "-compiled.d"))
            run([*base, "-Xclang", "-stack-usage-file", "-Xclang", stack,
                 "-MD", "-MT", "kernel.o", "-MF", dep, "-c", source, "-o", obj])
            assert report["dependencies"][source] == dependencies(dep)
            frames = [int(line.split("\t")[-2]) for line in stack.read_text().splitlines() if line]
            assert frames and max(frames) <= 2048
            report["stack_usage"][name] = {"maximum_bytes": max(frames), "sha256": sha(stack)}
            # The compiler must actually emit an x86_64 little-endian MH_OBJECT.
            import struct
            header = struct.unpack_from("<4I", obj.read_bytes())
            assert header[0] == 0xFEEDFACF and header[1] == 0x01000007 and header[3] == 1
            report["artifacts"][obj.name] = {"bytes": obj.stat().st_size, "sha256": sha(obj)}
        report["success"] = True
    finally:
        report["inputs_unchanged"] = all(sha(ROOT / p) == h for p, h in report["sources"].items())
        report["dependencies_unchanged"] = all(sha(ROOT / p) == h
            for closure in report["dependencies"].values() for p, h in closure.items())
        report["success"] &= report["inputs_unchanged"] and report["dependencies_unchanged"]
        (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    if not report["success"]:
        raise RuntimeError("Source changed or compilation failed")
    print(json.dumps({"success": True, "report": str(out / "report.json"),
                      "sha256": sha(out / "report.json"), "stack_usage": report["stack_usage"]}))


if __name__ == "__main__":
    main()
