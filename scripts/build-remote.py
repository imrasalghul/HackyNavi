#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build with an existing SSH-accessible Mac; copy results home and remove staging.

Uploads only this project's sources, build scripts and pinned firmware. Does
not clone dependencies, install software, sign/load a kext or alter the host.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import io
import importlib.util
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def run(args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def snapshot(destination):
    run([sys.executable, str(ROOT / "userspace/compiler/verify_fixture.py"), "--check"])
    paths = [ROOT / name for name in ("LICENSE", "THIRD-PARTY-NOTICES.md",
             "scripts/build-kext.sh", "scripts/build-accelerator.sh",
             "scripts/embed-firmware.py", "firmware/manifest.json")]
    paths.append(ROOT / "userspace/transport/native_protocol.cpp")
    paths.append(ROOT / "userspace/compiler/verify_fixture.py")
    paths += sorted(p for p in (ROOT / "userspace/compiler/fixtures/apple-add-one").rglob("*") if p.is_file())
    for directory in ("include", "src", "kext"):
        paths += sorted(p for p in (ROOT / directory).rglob("*") if p.is_file())
    manifest = json.loads((ROOT / "firmware/manifest.json").read_text(encoding="utf-8"))
    for item in manifest["files"] + [manifest["license"]]:
        relative = Path(item["path"])
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError("Invalid firmware manifest path")
        path = ROOT / ".research/firmware" / relative
        data = path.read_bytes()
        if len(data) != item["bytes"] or hashlib.sha256(data).hexdigest() != item["sha256"]:
            raise ValueError(f"Pinned firmware verification failed: {path}")
        paths.append(path)
    report = {"files": [], "uncompressed_bytes": 0}
    with tarfile.open(destination, "w:gz") as archive:
        for path in paths:
            if path.is_symlink() or not path.resolve().is_relative_to(ROOT.resolve()):
                raise ValueError(f"Source must be a regular workspace file: {path}")
            data = path.read_bytes()
            if path.suffix == ".sh":
                data = data.replace(b"\r\n", b"\n")
            name = path.relative_to(ROOT).as_posix()
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = 0o644
            archive.addfile(info, io.BytesIO(data))
            report["uncompressed_bytes"] += len(data)
            report["files"].append({"path": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    report["archive_sha256"] = hashlib.sha256(destination.read_bytes()).hexdigest()
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="Existing SSH host, e.g. user@hostname")
    parser.add_argument("--no-agent", action="store_true", help="Use configured identity files without contacting the SSH agent")
    parser.add_argument("--output", type=Path, default=ROOT / "build/macos-native")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_.@-]+", args.host) or args.host.startswith("-"):
        parser.error("Use an SSH host alias or user@hostname")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any((output / name).exists() for name in
           ("build-report.json", "source.tar.gz", "artifact.tar.gz", "HackyNavi.kext",
            "accelerator-artifact.tar.gz", "HackyNaviAccelerator.kext")):
        parser.error("Output already contains evidence; choose a new --output path")
    source = output / "source.tar.gz"
    report = snapshot(source)
    report["build_started_utc"] = datetime.now(timezone.utc).isoformat()
    print(f"Source snapshot: {report['uncompressed_bytes']:,} bytes; archive: {source.stat().st_size:,} bytes", flush=True)
    agent_options = ["-o", "IdentityAgent=none"] if args.no_agent else []
    ssh = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", *agent_options, args.host]
    # Abort before staging if free capacity is below a modest development
    # reserve. One source snapshot and one build only; no accumulating cache.
    setup = 'set -eu; test "$(uname -s)" = Darwin; avail=$(df -Pk "$HOME" | tail -1 | awk \'{print $4}\'); test "$avail" -ge 1048576; mktemp -d "$HOME/Library/Caches/HackyNavi-build.XXXXXX"'
    remote = run(ssh + [setup], capture_output=True, text=True).stdout.strip()
    if not re.fullmatch(r"/Users/[^/\r\n]+/Library/Caches/HackyNavi-build\.[A-Za-z0-9]+", remote):
        raise ValueError(f"Unexpected remote staging path: {remote!r}")
    report["host"] = args.host
    report["ssh_agent_disabled"] = args.no_agent
    report["remote_stage"] = remote
    q = shlex.quote(remote)
    remote_job_started = remote_job_finished = False
    report_path = output / "build-report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    try:
        run(["scp", "-q", "-o", "BatchMode=yes", *agent_options, str(source), f"{args.host}:{remote}/source.tar.gz"])
        command = (f"set -eu; cd {q}; tar -xzf source.tar.gz; "
                   'sw_vers; xcrun --sdk macosx --show-sdk-version; '
                   'bash scripts/build-kext.sh "$PWD/output"; '
                   'bash scripts/build-accelerator.sh "$PWD/output"; '
                   'set +e; /usr/bin/kmutil print-diagnostics -z -p "$PWD/output/HackyNavi.kext"; '
                   'diagnostic_status=$?; '
                   '/usr/bin/kmutil libraries -p "$PWD/output/HackyNavi.kext" --undef-symbols; '
                   'libraries_status=$?; set -e; '
                   'printf "KMUTIL_DIAGNOSTIC_EXIT=%s\\nKMUTIL_LIBRARIES_EXIT=%s\\n" "$diagnostic_status" "$libraries_status"; '
                   'printf "HACKYNAVI_ACCELERATOR_DIAGNOSTICS_BEGIN\\n"; '
                   'set +e; /usr/bin/kmutil print-diagnostics -z -r "$PWD/output" -p "$PWD/output/HackyNaviAccelerator.kext"; '
                   'accelerator_diagnostic_status=$?; '
                   '/usr/bin/kmutil libraries -r "$PWD/output" -p "$PWD/output/HackyNaviAccelerator.kext" --undef-symbols; '
                   'accelerator_libraries_status=$?; set -e; '
                   'printf "ACCELERATOR_KMUTIL_DIAGNOSTIC_EXIT=%s\\nACCELERATOR_KMUTIL_LIBRARIES_EXIT=%s\\n" '
                   '"$accelerator_diagnostic_status" "$accelerator_libraries_status"; '
                   'du -sk .; tar -czf artifact.tar.gz -C output HackyNavi.kext; '
                   'tar -czf accelerator-artifact.tar.gz -C output HackyNaviAccelerator.kext')
        remote_job_started = True
        report["remote_job_started"] = True
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        result = subprocess.run(ssh + [command], capture_output=True, text=True)
        remote_job_finished = result.returncode >= 0 and result.returncode != 255
        (output / "build.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        # kmutil libraries currently prints every provider even with its
        # --undef-symbols option. Keep the full evidence in build.log.
        provider_pattern = r"^\s+(\S+) in (.+): ([A-Za-z0-9_.-]+) \(([0-9.]+)\)$"
        core_output, _, accelerator_output = result.stdout.partition("HACKYNAVI_ACCELERATOR_DIAGNOSTICS_BEGIN\n")
        provider_matches = re.findall(provider_pattern, core_output, re.MULTILINE)
        accelerator_matches = re.findall(provider_pattern, accelerator_output, re.MULTILINE)
        concise_output = "\n".join(line for line in result.stdout.splitlines()
                                   if not re.match(provider_pattern, line))
        print(concise_output, flush=True)
        print(result.stderr, end="", flush=True)
        report["build_exit_code"] = result.returncode
        diagnostic = re.search(r"^KMUTIL_DIAGNOSTIC_EXIT=(\d+)$", result.stdout, re.MULTILINE)
        report["no_load_diagnostic_exit_code"] = int(diagnostic.group(1)) if diagnostic else None
        libraries = re.search(r"^KMUTIL_LIBRARIES_EXIT=(\d+)$", result.stdout, re.MULTILINE)
        report["kernel_libraries_exit_code"] = int(libraries.group(1)) if libraries else None
        report["no_load_diagnostic_tool"] = "kmutil print-diagnostics"
        report["diagnostic_errors"] = [line.strip() for line in result.stdout.splitlines()
                                       if "Filesystem error:" in line or "not loadable" in line.lower()]
        report["kernel_symbol_providers"] = {name: {"collection": collection, "identifier": identifier, "version": version}
                                              for name, collection, identifier, version in provider_matches}
        report["accelerator_kernel_symbol_providers"] = {
            name: {"collection": collection, "identifier": identifier, "version": version}
            for name, collection, identifier, version in accelerator_matches}
        for key, marker in (("accelerator_no_load_diagnostic_exit_code", "ACCELERATOR_KMUTIL_DIAGNOSTIC_EXIT"),
                            ("accelerator_kernel_libraries_exit_code", "ACCELERATOR_KMUTIL_LIBRARIES_EXIT")):
            match = re.search(r"^" + marker + r"=(\d+)$", accelerator_output, re.MULTILINE)
            report[key] = int(match.group(1)) if match else None
        if result.returncode:
            raise subprocess.CalledProcessError(result.returncode, command)
        archive = output / "artifact.tar.gz"
        run(["scp", "-q", "-o", "BatchMode=yes", *agent_options, f"{args.host}:{remote}/artifact.tar.gz", str(archive)])
        with tarfile.open(archive, "r:gz") as bundle:
            # Require only the expected regular bundle files before extraction.
            for item in bundle.getmembers():
                target = (output / item.name).resolve()
                if (not target.is_relative_to(output / "HackyNavi.kext") or
                        not (item.isfile() or item.isdir())):
                    raise ValueError(f"Unexpected artifact member: {item.name}")
            bundle.extractall(output, filter="data")
        report["artifact_sha256"] = hashlib.sha256(archive.read_bytes()).hexdigest()
        specification = importlib.util.spec_from_file_location("inspect_kext", ROOT / "scripts/inspect-kext.py")
        inspector = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(inspector)
        report["native_bundle"] = inspector.inspect(output / "HackyNavi.kext")
        report["boot_collection_import_check"] = inspector.boot_injection_imports(
            report["native_bundle"], report["kernel_symbol_providers"])
        accelerator_archive = output / "accelerator-artifact.tar.gz"
        run(["scp", "-q", "-o", "BatchMode=yes", *agent_options,
             f"{args.host}:{remote}/accelerator-artifact.tar.gz", str(accelerator_archive)])
        with tarfile.open(accelerator_archive, "r:gz") as bundle:
            for item in bundle.getmembers():
                target = (output / item.name).resolve()
                if (not target.is_relative_to(output / "HackyNaviAccelerator.kext") or
                        not (item.isfile() or item.isdir())):
                    raise ValueError(f"Unexpected accelerator artifact member: {item.name}")
            bundle.extractall(output, filter="data")
        report["accelerator_artifact_sha256"] = hashlib.sha256(accelerator_archive.read_bytes()).hexdigest()
        report["accelerator_bundle"] = inspector.inspect(output / "HackyNaviAccelerator.kext", "HackyNaviAccelerator")
        report["accelerator_kernel_symbols_unresolved"] = sorted(
            set(report["accelerator_bundle"]["undefined_kernel_symbols"]) -
            report["accelerator_kernel_symbol_providers"].keys())
        report["accelerator_load_route"] = "late system/auxiliary collection; excluded from early OpenCore package"
        report["kernel_symbols_unresolved"] = sorted(set(report["native_bundle"]["undefined_kernel_symbols"]) -
                                                     report["kernel_symbol_providers"].keys())
        if report["kernel_symbols_unresolved"]:
            print(f"Kernel symbols not confirmed by host libraries: {report['kernel_symbols_unresolved']}", flush=True)
        else:
            print(f"All {len(report['kernel_symbol_providers'])} imported kernel symbols resolve on the build Mac.", flush=True)
        print(f"Native build copied to {output / 'HackyNavi.kext'}", flush=True)
    finally:
        report["build_finished_utc"] = datetime.now(timezone.utc).isoformat()
        report["remote_job_started"] = remote_job_started
        report["remote_job_finished"] = remote_job_finished
        if not remote_job_started or remote_job_finished:
            # Exact validated mktemp path; a finished SSH command establishes
            # that this build is no longer using the stage. Never race a live
            # remote compiler after a lost connection or local interruption.
            cleanup = subprocess.run(ssh + [f"rm -rf -- {q}; test ! -e {q}"], capture_output=True, text=True)
            report["remote_stage_removed"] = cleanup.returncode == 0
            if cleanup.returncode:
                print(f"Remote staging cleanup failed: {remote}\n{cleanup.stderr}", flush=True)
        else:
            report["remote_stage_removed"] = False
            report["remote_job_state"] = "unknown; inspect live processes before rerun or cleanup"
            print(f"Remote job state unknown; preserving stage: {remote}", flush=True)
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
