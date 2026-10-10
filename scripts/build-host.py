#!/usr/bin/env python3
"""Build and test the portable C++ components without CMake or extra Python packages."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", help="Path to clang++, g++, or zig (executable only)")
    parser.add_argument("--rom", type=Path, help="Also inspect a ROM and check JSON output")
    parser.add_argument("--discovery", type=Path, help="Also inspect an IP discovery binary")
    args = parser.parse_args()
    compiler = args.compiler or os.environ.get("CXX")
    if not compiler:
        compiler = next((shutil.which(name) for name in ("clang++", "g++", "zig")
                         if shutil.which(name)), None)
    if not compiler:
        local = sorted((ROOT / ".tools").glob("zig-*/zig.exe"))
        if local:
            compiler = str(local[-1])
    if not compiler:
        parser.error("No C++ compiler found. Supply --compiler PATH or use the CMake build.")
    output = ROOT / "build" / "host"
    output.mkdir(parents=True, exist_ok=True)
    command = [compiler]
    env = os.environ.copy()
    if Path(compiler).stem.lower() == "zig":
        command.append("c++")
        env.setdefault("ZIG_LOCAL_CACHE_DIR", str(ROOT / ".tools" / "zig-local-cache"))
        env.setdefault("ZIG_GLOBAL_CACHE_DIR", str(ROOT / ".tools" / "zig-global-cache"))
    flags = ["-std=c++11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-O1", "-g",
             "-fno-exceptions", "-fno-rtti", "-I", str(ROOT / "include")]
    suffix = ".exe" if os.name == "nt" else ""
    targets = (
        ("otg-tests", ["src/discovery.cpp", "src/otg.cpp", "tests/otg_tests.cpp"]),
        ("gfx-compute-tests", ["src/gfx_compute.cpp", "tests/gfx_compute_tests.cpp"]),
        ("atom-vm-tests", ["src/atom_vm.cpp", "tests/atom_vm_tests.cpp"]),
        ("edid-tests", ["src/edid.cpp", "tests/edid_tests.cpp"]),
        ("edid-acquire-tests", ["src/discovery.cpp", "src/dp_aux.cpp", "src/hdmi_ddc.cpp",
                                "src/edid.cpp", "src/edid_acquire.cpp", "tests/edid_acquire_tests.cpp"]),
        ("hdmi-link-tests", ["src/discovery.cpp", "src/rom.cpp", "src/dmcub.cpp",
                             "src/dio_link.cpp", "src/hdmi_ddc.cpp", "src/hdmi_link.cpp",
                             "tests/hdmi_link_tests.cpp"]),
        ("hackynavi-rom", ["src/rom.cpp", "tools/rom_inspect.cpp"]),
        ("rom-tests", ["src/rom.cpp", "tests/rom_tests.cpp"]),
        ("hackynavi-discovery", ["src/discovery.cpp", "tools/discovery_inspect.cpp"]),
        ("discovery-tests", ["src/discovery.cpp", "tests/discovery_tests.cpp"]),
        ("device-discovery-tests", ["src/device_discovery.cpp", "tests/device_discovery_tests.cpp"]),
        ("ip-layout-tests", ["src/discovery.cpp", "src/ip_layout.cpp", "tests/ip_layout_tests.cpp"]),
        ("memory-layout-tests", ["src/memory_layout.cpp", "tests/memory_layout_tests.cpp"]),
        ("gpu-vm-tests", ["src/gpu_vm.cpp", "tests/gpu_vm_tests.cpp"]),
        ("vram-allocator-tests", ["src/memory_layout.cpp", "src/vram_allocator.cpp",
                                  "tests/vram_allocator_tests.cpp"]),
        ("vram-reservations-tests", ["src/memory_layout.cpp", "src/vram_allocator.cpp",
                                     "src/vram_reservations.cpp", "tests/vram_reservations_tests.cpp"]),
        ("boot-display-tests", ["src/discovery.cpp", "src/memory_layout.cpp",
                               "src/boot_display.cpp", "tests/boot_display_tests.cpp"]),
        ("boot-display-stop-tests", ["src/discovery.cpp", "src/memory_layout.cpp",
                                    "src/boot_display.cpp", "src/boot_display_stop.cpp", "tests/boot_display_stop_tests.cpp"]),
        ("bootstrap-quiesce-tests", ["src/discovery.cpp", "src/ip_layout.cpp",
                                    "src/bootstrap_quiesce.cpp", "tests/bootstrap_quiesce_tests.cpp"]),
        ("mode1-reset-tests", ["src/discovery.cpp", "src/ip_layout.cpp",
                              "src/mode1_reset.cpp", "tests/mode1_reset_tests.cpp"]),
        ("nbio-tests", ["src/discovery.cpp", "src/nbio.cpp", "tests/nbio_tests.cpp"]),
        ("vram-transfer-tests", ["src/memory_layout.cpp", "src/vram_allocator.cpp",
                                 "src/vram_transfer.cpp", "tests/vram_transfer_tests.cpp"]),
        ("gart-tests", ["src/discovery.cpp", "src/ip_layout.cpp", "src/memory_layout.cpp",
                        "src/gart.cpp", "tests/gart_tests.cpp"]),
        ("hdp-tests", ["src/discovery.cpp", "src/hdp.cpp", "tests/hdp_tests.cpp"]),
        ("hackynavi-firmware", ["src/rom.cpp", "src/firmware.cpp", "tools/firmware_inspect.cpp"]),
        ("firmware-tests", ["src/rom.cpp", "src/firmware.cpp", "tests/firmware_tests.cpp"]),
        ("sdma-tests", ["src/rom.cpp", "src/sdma.cpp", "tests/sdma_tests.cpp"]),
        ("sdma-queue-tests", ["src/rom.cpp", "src/sdma.cpp", "src/discovery.cpp",
                              "src/ip_layout.cpp", "src/sdma_queue.cpp", "tests/sdma_queue_tests.cpp"]),
        ("psp-tests", ["src/rom.cpp", "src/firmware.cpp", "src/discovery.cpp",
                       "src/ip_layout.cpp", "src/memory_layout.cpp", "src/vram_allocator.cpp",
                       "src/vram_transfer.cpp", "src/psp.cpp", "tests/psp_tests.cpp"]),
        ("sdma-firmware-tests", ["src/rom.cpp", "src/firmware.cpp", "src/discovery.cpp",
                       "src/ip_layout.cpp", "src/memory_layout.cpp", "src/vram_allocator.cpp",
                       "src/vram_transfer.cpp", "src/hdp.cpp", "src/sdma_firmware.cpp",
                       "tests/sdma_firmware_tests.cpp"]),
        ("psp-ring-tests", ["src/rom.cpp", "src/firmware.cpp", "src/discovery.cpp",
                       "src/ip_layout.cpp", "src/memory_layout.cpp", "src/vram_allocator.cpp",
                       "src/vram_transfer.cpp", "src/psp.cpp", "src/psp_ring.cpp", "tests/psp_ring_tests.cpp"]),
        ("gfx-firmware-tests", ["src/rom.cpp", "src/firmware.cpp", "src/discovery.cpp",
                       "src/ip_layout.cpp", "src/memory_layout.cpp", "src/vram_allocator.cpp",
                       "src/vram_transfer.cpp", "src/hdp.cpp", "src/psp.cpp", "src/psp_ring.cpp",
                       "src/gfx_firmware.cpp", "tests/gfx_firmware_tests.cpp"]),
        ("dp-aux-tests", ["src/discovery.cpp", "src/dp_aux.cpp", "tests/dp_aux_tests.cpp"]),
        ("hdmi-ddc-tests", ["src/discovery.cpp", "src/hdmi_ddc.cpp", "tests/hdmi_ddc_tests.cpp"]),
        ("dmcub-tests", ["src/discovery.cpp", "src/rom.cpp", "src/dmcub.cpp", "tests/dmcub_tests.cpp"]),
        ("runtime-rom-tests", ["src/discovery.cpp", "src/rom.cpp", "src/nbio.cpp", "src/runtime_rom.cpp", "tests/runtime_rom_tests.cpp"]),
        ("cold-aux-tests", ["src/discovery.cpp", "src/cold_aux.cpp", "tests/cold_aux_tests.cpp"]),
        ("cold-dma-tests", ["src/discovery.cpp", "src/cold_dma.cpp", "tests/cold_dma_tests.cpp"]),
        ("dio-link-tests", ["src/discovery.cpp", "src/rom.cpp", "src/dmcub.cpp", "src/dio_link.cpp", "tests/dio_link_tests.cpp"]),
        ("dp-training-tests", ["src/discovery.cpp", "src/rom.cpp", "src/dmcub.cpp", "src/dio_link.cpp", "src/dp_aux.cpp", "src/dp_training.cpp", "tests/dp_training_tests.cpp"]),
        ("dmcub-firmware-tests", ["src/rom.cpp", "src/firmware.cpp", "src/discovery.cpp",
                       "src/ip_layout.cpp", "src/memory_layout.cpp", "src/vram_allocator.cpp",
                       "src/vram_transfer.cpp", "src/hdp.cpp", "src/psp.cpp", "src/psp_ring.cpp",
                       "src/dmcub.cpp", "src/dmcub_firmware.cpp", "tests/dmcub_firmware_tests.cpp"]),
        ("smu-tests", ["src/rom.cpp", "src/firmware.cpp", "src/discovery.cpp",
                       "src/ip_layout.cpp", "src/memory_layout.cpp", "src/vram_allocator.cpp",
                       "src/vram_transfer.cpp", "src/hdp.cpp", "src/psp.cpp", "src/psp_ring.cpp",
                       "src/gfx_firmware.cpp", "src/smu.cpp", "tests/smu_tests.cpp"]),
        ("gfx-init-tests", ["src/rom.cpp", "src/firmware.cpp", "src/discovery.cpp",
                       "src/ip_layout.cpp", "src/memory_layout.cpp", "src/vram_allocator.cpp",
                       "src/vram_transfer.cpp", "src/hdp.cpp", "src/psp.cpp", "src/psp_ring.cpp",
                       "src/gfx_firmware.cpp", "src/smu.cpp", "src/gfx_init.cpp", "tests/gfx_init_tests.cpp"]),
        ("gfx-queue-tests", ["src/rom.cpp", "src/firmware.cpp", "src/discovery.cpp",
                       "src/ip_layout.cpp", "src/memory_layout.cpp", "src/vram_allocator.cpp",
                       "src/vram_transfer.cpp", "src/hdp.cpp", "src/psp.cpp", "src/psp_ring.cpp",
                       "src/gfx_firmware.cpp", "src/smu.cpp", "src/gfx_init.cpp", "src/gfx_queue.cpp",
                       "tests/gfx_queue_tests.cpp"]),
    )
    for name, sources in targets:
        print(f"Building {name}", flush=True)
        subprocess.run(command + flags + [str(ROOT / source) for source in sources] + [
                       "-o", str(output / (name + suffix))], check=True, env=env)
    for name in ("rom-tests", "discovery-tests", "device-discovery-tests", "ip-layout-tests",
                 "gfx-compute-tests", "atom-vm-tests", "hdmi-link-tests", "edid-tests", "edid-acquire-tests", "otg-tests",
                 "memory-layout-tests", "gpu-vm-tests", "vram-allocator-tests", "vram-transfer-tests", "gart-tests",
                 "vram-reservations-tests", "boot-display-tests", "boot-display-stop-tests", "bootstrap-quiesce-tests",
                 "mode1-reset-tests", "nbio-tests",
                 "sdma-queue-tests", "psp-tests", "sdma-firmware-tests", "psp-ring-tests", "gfx-firmware-tests", "dp-aux-tests",
                 "hdmi-ddc-tests", "smu-tests", "gfx-init-tests", "gfx-queue-tests",
                 "dmcub-tests", "dmcub-firmware-tests", "runtime-rom-tests", "cold-aux-tests", "cold-dma-tests", "dio-link-tests", "dp-training-tests",
                 "hdp-tests", "firmware-tests", "sdma-tests"):
        subprocess.run([str(output / (name + suffix))], check=True)
    test_env = env.copy()
    test_env["HACKYNAVI_DISCOVERY_TOOL"] = str(output / ("hackynavi-discovery" + suffix))
    subprocess.run([sys.executable, "-m", "unittest", "discover", "-s", str(ROOT / "tests"),
                    "-p", "test_*.py"], check=True, cwd=ROOT, env=test_env)
    if args.rom:
        import json
        result = subprocess.run([str(output / ("hackynavi-rom" + suffix)), str(args.rom.resolve())],
                                check=True, capture_output=True, text=True)
        report = json.loads(result.stdout)
        report_path = output / "rom-report.json"
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"Inspected {len(report['images'])} PCI images; report: {report_path}")
    if args.discovery:
        import json
        result = subprocess.run([str(output / ("hackynavi-discovery" + suffix)),
                                 str(args.discovery.resolve())], check=True,
                                capture_output=True, text=True)
        report = json.loads(result.stdout)
        report_path = output / "discovery-report.json"
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"Discovery report: {report_path}")
    print(f"Host build and tests passed. Executables: {output}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"Build failed: {exc}", file=sys.stderr)
        sys.exit(1)
