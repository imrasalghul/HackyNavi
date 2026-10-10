#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Snapshot and check the isolated compute path on an Intel Mac; never load a kext."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import shlex
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
KERNEL_SOURCES = [
    "kext/HackyNavi/HackyNaviDMABuffer.cpp",
    "kext/HackyNavi/HackyNaviEngineController.cpp",
    "kext/HackyNavi/HackyNaviGARTController.cpp",
    "kext/HackyNavi/HackyNaviVMContext.cpp",
    "kext/HackyNavi/HackyNaviVMCompletion.cpp",
    "kext/HackyNavi/HackyNaviGraphicsQueue.cpp",
    "kext/HackyNavi/HackyNaviGraphicsQueueReuse.cpp",
    "src/isolated_compute.cpp", "src/gfx_queue.cpp", "src/gfx_compute.cpp",
]
TEST_SOURCES = {
    "isolated-compute-tests": ["src/gfx_compute.cpp", "src/isolated_compute.cpp",
                               "tests/isolated_compute_tests.cpp"],
    "gfx-queue-tests": ["src/" + name + ".cpp" for name in (
        "rom", "firmware", "discovery", "ip_layout", "memory_layout", "vram_allocator",
        "vram_transfer", "hdp", "psp", "psp_ring", "gfx_firmware", "smu", "gfx_init", "gfx_queue")]
        + ["tests/gfx_queue_tests.cpp"],
}


def snapshot(include_tests=True):
    pending = KERNEL_SOURCES + ([name for names in TEST_SOURCES.values() for name in names] if include_tests else [])
    captured = {}
    while pending:
        name = pending.pop()
        if name in captured:
            continue
        path = ROOT / name
        data = path.read_bytes()
        captured[name] = data
        for delimiter, include in re.findall(r'^\s*#include\s+(["<])([^">]+)[">]', data.decode("utf-8"), re.M):
            if delimiter == "<" and not include.startswith("hackynavi/"):
                continue
            found = next((p for p in (path.parent / include, ROOT / "include" / include) if p.is_file()), None)
            if found is None:
                raise RuntimeError(f"Missing local include {include} in {name}")
            pending.append(found.resolve().relative_to(ROOT).as_posix())
    if sum(map(len, captured.values())) > 2 * 1024 * 1024:
        raise RuntimeError("Refusing unexpectedly large source stage")
    return captured


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="victor@mini")
    parser.add_argument("--no-agent", action="store_true", help="Use configured identity files without contacting the SSH agent")
    parser.add_argument("--output", type=Path, default=ROOT / "build/native-submission")
    parser.add_argument("--kernel-only", action="store_true", help="Compile the native owners without repeating unchanged protocol test suites")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_.@-]+", args.host) or args.host.startswith("-"):
        parser.error("Invalid SSH host")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if (output / "report.json").exists():
        parser.error("Output already contains a report; choose a new path to preserve evidence")
    test_sources = {} if args.kernel_only else TEST_SOURCES
    captured = snapshot(not args.kernel_only)
    hashes = {name: hashlib.sha256(data).hexdigest() for name, data in sorted(captured.items())}
    archive = output / "source.tar.gz"
    with tarfile.open(archive, "w:gz") as tar:
        entries = dict(captured)
        entries["source.sha256"] = "".join(f"{digest}  {name}\n" for name, digest in hashes.items()).encode()
        for name, data in sorted(entries.items()):
            entry = tarfile.TarInfo(name)
            entry.size = len(data)
            tar.addfile(entry, io.BytesIO(data))
    agent_options = ["-o", "IdentityAgent=none"] if args.no_agent else []
    ssh = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", *agent_options, args.host]
    report = {"source_sha256": hashes, "source_bytes": sum(map(len, captured.values())),
              "kernel_sources": KERNEL_SOURCES, "test_sources": test_sources,
              "kernel_loaded": False, "runtime_gpu_tested": False}
    remote = None
    remote_job_started = remote_job_finished = False
    try:
        prepare = """set -eu
test "$(uname -s)" = Darwin
test "$(uname -m)" = x86_64
df -Pk "$HOME"
avail=$(df -Pk "$HOME" | tail -1 | awk '{print $4}')
test "$avail" -ge 1048576
mktemp -d "$HOME/Library/Caches/HackyNavi-submission.XXXXXX"
"""
        # Binary stdin preserves LF on Windows; text mode would send CRLF to
        # the remote POSIX shell (including an invalid `set -eu\r` option).
        result = subprocess.run(ssh + ["sh -s"], input=prepare.encode(), capture_output=True)
        report["prepare_stdout"] = result.stdout.decode("utf-8", errors="replace")
        report["prepare_stderr"] = result.stderr.decode("utf-8", errors="replace")
        if result.returncode:
            report["exit_code"] = result.returncode
            raise RuntimeError(report["prepare_stderr"] or "Remote preparation failed")
        remote = report["prepare_stdout"].strip().splitlines()[-1]
        if not re.fullmatch(r"/Users/[^/\r\n]+/Library/Caches/HackyNavi-submission\.[A-Za-z0-9]+", remote):
            raise RuntimeError("Unexpected remote stage path")
        report["storage_before"] = report["prepare_stdout"]
        report["remote_stage"] = remote
        subprocess.run(["scp", "-q", "-o", "BatchMode=yes", *agent_options, str(archive), args.host + ":" + remote + "/source.tar.gz"], check=True)
        commands = ["set -eu", "cd " + shlex.quote(remote), "tar -xzf source.tar.gz",
                    "shasum -a 256 -c source.sha256", "sw_vers", "clang++ --version",
                    "xcrun --sdk macosx --show-sdk-version"]
        for name, sources in test_sources.items():
            commands.append("clang++ -arch x86_64 -mmacosx-version-min=15.0 -std=c++11 "
                            "-O1 -g -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti "
                            "-fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude "
                            + " ".join(sources) + " -o " + name)
            commands.append("ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 ./" + name)
        commands.append('sdk_dir=$(xcrun --sdk macosx --show-sdk-path)')
        for name in KERNEL_SOURCES:
            commands.append('clang++ -arch x86_64 -isysroot "$sdk_dir" -mmacosx-version-min=15.0 '
                            '-std=c++14 -mkernel -fapple-kext -DKERNEL -DKERNEL_EXTENSION -nostdinc '
                            '-isystem "$sdk_dir/System/Library/Frameworks/Kernel.framework/Headers" '
                            '-Iinclude -fno-exceptions -fno-rtti -fno-builtin -fno-common '
                            '-fno-stack-protector -mno-red-zone -mno-mmx -mno-sse -mno-sse2 '
                            '-Wall -Wextra -Werror -Wframe-larger-than=2048 -Wno-unused-parameter -Wno-deprecated-declarations '
                            '-O2 -c ' + name + ' -o ' + Path(name).stem + '.o')
        commands += ["file *.o", "shasum -a 256 *.o", "du -sk ."]
        report["commands"] = commands
        # Preserve the exact snapshot and report before starting the bounded
        # job; a lost connection is not evidence that the job has stopped.
        (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        remote_job_started = True
        result = subprocess.run(ssh + ["sh -s"], input=("\n".join(commands) + "\n").encode(), capture_output=True)
        remote_job_finished = result.returncode >= 0 and result.returncode != 255
        log = (result.stdout + result.stderr).decode("utf-8", errors="replace")
        (output / "run.log").write_text(log, encoding="utf-8")
        print(log, flush=True)
        report["exit_code"] = result.returncode
    finally:
        # Delete only the exact mktemp path whose parent/prefix were checked.
        if (remote and (not remote_job_started or remote_job_finished) and
                re.fullmatch(r"/Users/[^/\r\n]+/Library/Caches/HackyNavi-submission\.[A-Za-z0-9]+", remote)):
            quoted = shlex.quote(remote)
            result = subprocess.run(ssh + ["rm -rf -- " + quoted + "; test ! -e " + quoted], capture_output=True, text=True)
            report["remote_stage_removed"] = result.returncode == 0
        elif remote:
            # An SSH transport failure can leave clang running. Preserve its
            # stage for explicit process inspection instead of racing cleanup.
            report["remote_stage_removed"] = False
            report["remote_job_state"] = "unknown; inspect before rerun or cleanup"
        report["snapshot_matches_worktree"] = all((ROOT / name).read_bytes() == data for name, data in captured.items())
        (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    return report.get("exit_code", 1)


if __name__ == "__main__":
    raise SystemExit(main())
