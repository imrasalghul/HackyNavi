#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile real x86_64 Mach-O kext objects with Zig and MacKernelSDK; no loading."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SDK_URL = "https://github.com/acidanthera/MacKernelSDK"
TESTED_SDK_COMMIT = "3f750085caa17ec3a7880f11c11bf4f48cd6a164"


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, env=None):
    return subprocess.run([str(value) for value in command], cwd=ROOT,
                          env=env, capture_output=True, text=True, check=False)


def mach_object(path):
    """Verify compiler output as Mach-O, using Apple's public loader.h layout."""
    data = path.read_bytes()
    if len(data) < 32:
        raise ValueError(f"Truncated Mach-O object: {path}")
    magic, cpu, subtype, kind, commands, command_bytes, flags, _ = struct.unpack_from("<8I", data)
    if magic != 0xFEEDFACF or cpu != 0x01000007 or kind != 1:
        raise ValueError(f"Not a little-endian x86_64 MH_OBJECT: {path}")
    end = 32 + command_bytes
    if end > len(data):
        raise ValueError(f"Mach-O load commands exceed object: {path}")
    cursor = 32
    sections, symbols = [], {}
    minimum_os = None
    for _ in range(commands):
        if cursor + 8 > end:
            raise ValueError(f"Truncated Mach-O command: {path}")
        command, length = struct.unpack_from("<2I", data, cursor)
        if length < 8 or cursor + length > end:
            raise ValueError(f"Malformed Mach-O command size: {path}")
        if command == 0x19:  # LC_SEGMENT_64
            if length < 72:
                raise ValueError("Short LC_SEGMENT_64")
            count = struct.unpack_from("<I", data, cursor + 64)[0]
            if 72 + count * 80 > length:
                raise ValueError("Truncated Mach-O section array")
            for index in range(count):
                section = cursor + 72 + index * 80
                name = data[section:section + 16].split(b"\0", 1)[0].decode("ascii")
                sections.append(name)
        elif command == 2:  # LC_SYMTAB
            if length < 24:
                raise ValueError("Short LC_SYMTAB")
            offset, count, strings, string_bytes = struct.unpack_from("<4I", data, cursor + 8)
            if offset + count * 16 > len(data) or strings + string_bytes > len(data):
                raise ValueError("Mach-O symbol/string table exceeds object")
            for index in range(count):
                name_offset, symbol_type, section, _, value = struct.unpack_from(
                    "<IBBHQ", data, offset + index * 16)
                if name_offset >= string_bytes:
                    raise ValueError("Mach-O symbol name is outside string table")
                terminator = data.find(b"\0", strings + name_offset, strings + string_bytes)
                if terminator < 0:
                    raise ValueError("Unterminated Mach-O symbol name")
                name = data[strings + name_offset:terminator].decode("utf-8", errors="replace")
                if name in ("_kmod_info", "__realmain", "__antimain", "__start", "__stop"):
                    symbols[name] = {"defined": bool(symbol_type & 0x0E),
                                     "section": section, "value": value}
        elif command == 0x32:  # LC_BUILD_VERSION
            if length < 24:
                raise ValueError("Short LC_BUILD_VERSION")
            minimum_os = struct.unpack_from("<I", data, cursor + 12)[0]
        elif command == 0x24:  # LC_VERSION_MIN_MACOSX
            if length < 16:
                raise ValueError("Short LC_VERSION_MIN_MACOSX")
            minimum_os = struct.unpack_from("<I", data, cursor + 8)[0]
        cursor += length
    if cursor != end:
        raise ValueError("Mach-O command count does not match command size")
    return {"path": str(path), "bytes": len(data), "sha256": sha256(path),
            "format": "Mach-O 64-bit", "cpu": "x86_64", "cpu_subtype": subtype,
            "file_type": "MH_OBJECT", "flags": flags, "minimum_os_raw": minimum_os,
            "sections": sections, "module_symbols": symbols}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, default=ROOT / ".research" / "MacKernelSDK",
                        help="Existing Acidanthera MacKernelSDK checkout; nothing is downloaded")
    parser.add_argument("--compiler", help="Zig executable (tested with 0.17.0)")
    parser.add_argument("--output", type=Path, default=ROOT / "build" / "cross")
    parser.add_argument("--firmware-dir", type=Path, default=ROOT / ".research" / "firmware",
                        help="Root of the pinned AMD firmware and license inputs")
    parser.add_argument("--probe-link", action="store_true",
                        help="Record whether Zig accepts -kext; currently unsupported")
    args = parser.parse_args()
    sdk = args.sdk.resolve()
    output = args.output.resolve()
    headers = sdk / "Headers"
    for required in ("IOKit/IOService.h", "IOKit/pci/IOPCIDevice.h", "mach/kmod.h", "Availability.h"):
        if not (headers / required).is_file():
            parser.error(f"Missing real SDK header: {headers / required}. Obtain {SDK_URL} separately.")
    if "__ACIDANTHERA_MAC_SDK" not in (headers / "Availability.h").read_text(encoding="utf-8"):
        parser.error("Selected headers do not identify themselves as Acidanthera MacKernelSDK")
    compiler = args.compiler or shutil.which("zig")
    if not compiler:
        local = sorted((ROOT / ".tools").glob("zig-*/zig.exe"))
        if local:
            compiler = str(local[-1])
    if not compiler:
        parser.error("No Zig compiler found. Pass --compiler PATH; nothing is installed automatically.")
    compiler = str(Path(compiler).resolve()) if Path(compiler).exists() else compiler
    version = run([compiler, "version"])
    if version.returncode:
        raise RuntimeError(f"Cannot query Zig version: {version.stderr.strip()}")
    revision = run(["git", "-C", sdk, "rev-parse", "HEAD"])
    dirty = run(["git", "-C", sdk, "status", "--porcelain"])
    if revision.returncode or dirty.returncode:
        parser.error("SDK must be a Git checkout so its revision and local changes can be recorded")
    env = os.environ.copy()
    env.setdefault("ZIG_LOCAL_CACHE_DIR", str(ROOT / ".tools" / "zig-local-cache"))
    env.setdefault("ZIG_GLOBAL_CACHE_DIR", str(ROOT / ".tools" / "zig-global-cache"))
    output.mkdir(parents=True, exist_ok=True)
    firmware_output = output / "firmware"
    firmware_command = [sys.executable, ROOT / "scripts" / "embed-firmware.py",
                        "--input-dir", args.firmware_dir.resolve(), "--output-dir", firmware_output]
    firmware_result = run(firmware_command)
    if firmware_result.returncode:
        raise RuntimeError(firmware_result.stdout + firmware_result.stderr)
    print(firmware_result.stdout.strip(), flush=True)
    flags = ["c++", "-target", "x86_64-macos.15.0", "-std=c++14", "-mkernel", "-fapple-kext",
             "-DKERNEL", "-DKERNEL_EXTENSION", "-nostdinc", "-nostdlibinc",
             "-I", str(headers), "-I", str(ROOT / "include"),
             "-fno-exceptions", "-fno-rtti", "-fno-builtin", "-fno-common", "-fno-stack-protector",
             "-mno-red-zone", "-mno-mmx", "-mno-sse", "-mno-sse2", "-Wall", "-Wextra",
             "-Werror", "-Wno-unused-parameter", "-O2"]
    # The normal include path must precede Zig's bundled user-space Darwin
    # headers. -nostdlibinc avoids ffs/Debugger conflicts with kernel declarations.
    report = {"schema_version": 1, "scope": "compile-only kernel-header check",
              "compiler": compiler, "compiler_version": version.stdout.strip(),
              "sdk_url": SDK_URL, "sdk_commit": revision.stdout.strip(),
              "sdk_has_local_changes": bool(dirty.stdout.strip()),
              "tested_sdk_commit": TESTED_SDK_COMMIT,
              "target": "x86_64-macos.15.0", "kext_bundle_created": False,
              "runtime_tested": False, "objects": [], "commands": [],
              "firmware_manifest": json.loads((firmware_output / "FirmwareManifest.json").read_text(encoding="utf-8")),
              "limitations": ["MacKernelSDK is based on macOS 11 SDK headers with project modifications.",
                              "Compilation does not establish macOS 15.8.1 ABI or loader compatibility.",
                              "MH_OBJECT files are not a loadable kext; nothing is installed or signed."]}
    object_paths = []
    sources = ("kext/HackyNavi/HackyNavi.cpp", "kext/HackyNavi/HackyNaviDevice.cpp",
               "kext/HackyNavi/HackyNaviDMABuffer.cpp",
               "kext/HackyNavi/HackyNaviGARTStorage.cpp",
               "kext/HackyNavi/HackyNaviGARTController.cpp",
               "kext/HackyNavi/HackyNaviSDMAQueue.cpp",
               "kext/HackyNavi/HackyNaviFirmwareController.cpp",
               "kext/HackyNavi/HackyNaviGraphicsQueue.cpp",
               "kext/HackyNavi/HackyNaviDisplayController.cpp",
               "kext/HackyNavi/HackyNaviEngineController.cpp",
               "kext/HackyNavi/HackyNaviResetController.cpp",
               "kext/HackyNavi/HackyNaviAtomController.cpp",
               "src/edid.cpp",
               "src/edid_acquire.cpp",
               "src/otg.cpp",
               "src/gfx_compute.cpp", "src/hdmi_link.cpp", "src/atom_vm.cpp",
               "src/rom.cpp", "src/discovery.cpp", "src/device_discovery.cpp",
               "src/ip_layout.cpp", "src/memory_layout.cpp", "src/gpu_vm.cpp",
               "src/vram_allocator.cpp", "src/vram_transfer.cpp", "src/gart.cpp", "src/hdp.cpp",
               "src/vram_reservations.cpp", "src/boot_display.cpp", "src/boot_display_stop.cpp", "src/bootstrap_quiesce.cpp",
               "src/mode1_reset.cpp", "src/nbio.cpp",
               "src/sdma.cpp", "src/sdma_queue.cpp", "src/firmware.cpp",
               "src/psp.cpp", "src/sdma_firmware.cpp",
               "src/psp_ring.cpp", "src/gfx_firmware.cpp", "src/dp_aux.cpp",
               "src/hdmi_ddc.cpp", "src/smu.cpp", "src/gfx_init.cpp", "src/gfx_queue.cpp",
               "src/dmcub.cpp", "src/dmcub_firmware.cpp",
               "src/runtime_rom.cpp", "src/cold_aux.cpp", "src/cold_dma.cpp", "src/dio_link.cpp",
               "src/dp_training.cpp",
               "kext/HackyNavi/Module.cpp", str(firmware_output / "FirmwareResources.cpp"))
    for relative in sources:
        source = ROOT / relative
        destination = output / (source.stem + ".o")
        command = [compiler] + flags + ["-c", str(source), "-o", str(destination)]
        report["commands"].append(command)
        print(f"Compiling {relative}", flush=True)
        result = run(command, env=env)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        item = mach_object(destination)
        item["source"] = relative
        item["source_sha256"] = sha256(source)
        report["objects"].append(item)
        object_paths.append(destination)
    objects_by_source = {item["source"]: item for item in report["objects"]}
    module_symbols = objects_by_source["kext/HackyNavi/Module.cpp"]["module_symbols"]
    for symbol in ("_kmod_info", "__realmain", "__antimain"):
        if not module_symbols.get(symbol, {}).get("defined"):
            raise RuntimeError(f"Required module definition missing: {symbol}")
    for relative in ("kext/HackyNavi/HackyNavi.cpp", "kext/HackyNavi/HackyNaviDevice.cpp",
                     "kext/HackyNavi/HackyNaviGARTController.cpp", "kext/HackyNavi/HackyNaviSDMAQueue.cpp",
                     "kext/HackyNavi/HackyNaviFirmwareController.cpp",
                     "kext/HackyNavi/HackyNaviGraphicsQueue.cpp",
                     "kext/HackyNavi/HackyNaviDisplayController.cpp",
                     "kext/HackyNavi/HackyNaviEngineController.cpp",
                     "kext/HackyNavi/HackyNaviResetController.cpp",
                     "kext/HackyNavi/HackyNaviAtomController.cpp"):
        if "__mod_init_func" not in objects_by_source[relative]["sections"]:
            raise RuntimeError(f"IOKit class object has no C++ static-initializer section: {relative}")
    if args.probe_link:
        archive = sdk / "Library" / "x86_64" / "libkmod.a"
        if not archive.is_file():
            raise RuntimeError(f"Missing SDK kernel archive: {archive}")
        command = [compiler, "c++", "-target", "x86_64-macos.15.0", "-fapple-kext",
                   "-nostdlib", "-Wl,-kext"] + [str(path) for path in object_paths] + [
                   str(archive), "-o", str(output / "HackyNavi.link-probe")]
        result = run(command, env=env)
        report["link_probe"] = {"command": command, "exit_code": result.returncode,
                                "stdout": result.stdout, "stderr": result.stderr,
                                "note": "A successful future link probe still requires independent kext validation."}
    report_path = output / "cross-check-report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Verified {len(object_paths)} x86_64 Mach-O object files. Report: {report_path}")
    if args.probe_link and report["link_probe"]["exit_code"]:
        print("Link probe failed: " + report["link_probe"]["stderr"].strip())
    print("Compile-only result. No loadable kext, macOS runtime test, or installation.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, struct.error) as error:
        print(f"Cross-check failed: {error}", file=sys.stderr)
        sys.exit(1)
