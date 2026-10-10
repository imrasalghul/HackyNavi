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
    subprocess.run([sys.executable, str(ROOT / "userspace/compiler/verify_fixture.py"), "--check"], check=True)
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
        ("native-abi-tests", ["userspace/transport/native_protocol.cpp", "tests/native_abi_tests.cpp"]),
        ("retained-result-budget-tests", ["tests/retained_result_budget_tests.cpp"]),
        ("display-power-tests", ["src/discovery.cpp", "src/display_power.cpp", "tests/display_power_tests.cpp"]),
        ("hubp-scanout-tests", ["src/discovery.cpp", "src/memory_layout.cpp", "src/vram_allocator.cpp", "src/hubp_scanout.cpp", "tests/hubp_scanout_tests.cpp"]),
        ("pixel-pipeline-tests", ["src/discovery.cpp", "src/pixel_pipeline.cpp", "tests/pixel_pipeline_tests.cpp"]),
        ("display-idle-tests", ["src/discovery.cpp", "src/display_idle.cpp", "tests/display_idle_tests.cpp"]),
        ("display-smu-tests", ["src/rom.cpp", "src/discovery.cpp", "src/ip_layout.cpp", "src/display_model_input.cpp", "src/display_dentist.cpp", "src/display_smu.cpp", "tests/display_smu_tests.cpp"]),
        ("display-dpm-tests", ["src/rom.cpp", "src/discovery.cpp", "src/ip_layout.cpp", "src/display_model_input.cpp", "src/display_dentist.cpp", "src/display_smu.cpp", "tests/display_dpm_tests.cpp"]),
        ("display-model-input-tests", ["src/rom.cpp", "src/discovery.cpp", "src/ip_layout.cpp", "src/display_model_input.cpp", "tests/display_model_input_tests.cpp"]),
        ("display-dentist-tests", ["src/rom.cpp", "src/discovery.cpp", "src/ip_layout.cpp", "src/display_model_input.cpp", "src/display_dentist.cpp", "tests/display_dentist_tests.cpp"]),
        ("display-clock-tests", ["src/discovery.cpp", "src/display_clock.cpp", "tests/display_clock_tests.cpp"]),
        ("display-bandwidth-tests", ["src/discovery.cpp", "src/display_bandwidth.cpp", "tests/display_bandwidth_tests.cpp"]),
        ("hdmi-stream-tests", ["src/discovery.cpp", "src/otg.cpp", "src/vpg.cpp", "src/avi.cpp", "src/hdmi_stream.cpp", "tests/hdmi_stream_tests.cpp"]),
        ("dp-stream-tests", ["src/discovery.cpp", "src/dp_stream.cpp", "tests/dp_stream_tests.cpp"]),
        ("avi-tests", ["src/avi.cpp", "tests/avi_tests.cpp"]),
        ("vpg-tests", ["src/discovery.cpp", "src/vpg.cpp", "tests/vpg_tests.cpp"]),
        ("otg-tests", ["src/discovery.cpp", "src/otg.cpp", "tests/otg_tests.cpp"]),
        ("gfx-compute-tests", ["src/gfx_compute.cpp", "tests/gfx_compute_tests.cpp"]),
        ("isolated-compute-tests", ["src/gfx_compute.cpp", "src/isolated_compute.cpp", "tests/isolated_compute_tests.cpp"]),
        ("gfx-aco-fixture-tests", ["src/gfx_aco_fixture.cpp", "tests/gfx_aco_fixture_tests.cpp"]),
        ("gfx11-compute-product-tests", ["src/gfx11_compute_product.cpp", "tests/gfx11_compute_product_tests.cpp"]),
        ("gfx11-dispatch-product-tests", ["src/gfx11_compute_product.cpp", "src/gfx11_dispatch_product.cpp",
                                          "tests/gfx11_dispatch_product_tests.cpp"]),
        ("gfx11-graphics-barrier-tests", ["src/gfx11_graphics_barrier.cpp", "tests/gfx11_graphics_barrier_tests.cpp"]),
        ("gfx11-graphics-product-tests", ["src/gfx11_graphics_product.cpp", "tests/gfx11_graphics_product_tests.cpp"]),
        ("gfx11-fragment-buffer-tests", ["src/gfx11_graphics_product.cpp", "tests/gfx11_fragment_buffer_tests.cpp"]),
        ("gfx11-graphics-draw-tests", ["src/gfx11_graphics_product.cpp", "src/gfx11_color_target.cpp", "src/gfx11_graphics_barrier.cpp", "src/gfx11_graphics_draw.cpp", "tests/gfx11_graphics_draw_tests.cpp"]),
        ("gfx11-buffer-draw-tests", ["src/gfx11_graphics_product.cpp", "src/gfx11_color_target.cpp", "src/gfx11_graphics_barrier.cpp", "src/gfx11_graphics_draw.cpp", "tests/gfx11_buffer_draw_tests.cpp"]),
        ("gfx11-vertex-buffer-tests", ["src/gfx11_graphics_product.cpp", "tests/gfx11_vertex_buffer_tests.cpp"]),
        ("gfx11-vertex-buffer-draw-tests", ["src/gfx11_graphics_product.cpp", "src/gfx11_color_target.cpp", "src/gfx11_graphics_barrier.cpp", "src/gfx11_graphics_draw.cpp", "tests/gfx11_vertex_buffer_draw_tests.cpp"]),
        ("gfx11-graphics-memory-tests", ["src/gfx11_graphics_memory.cpp", "tests/gfx11_graphics_memory_tests.cpp"]),
        ("gfx11-graphics-topology-tests", ["src/discovery.cpp", "src/ip_layout.cpp", "src/gfx11_graphics_topology.cpp", "tests/gfx11_graphics_topology_tests.cpp"]),
        ("gfx11-color-target-tests", ["src/gfx11_color_target.cpp", "tests/gfx11_color_target_tests.cpp"]),
        ("gfx11-color-topology-tests", ["src/discovery.cpp", "src/ip_layout.cpp", "src/nbio.cpp", "src/gfx11_color_target.cpp", "src/gfx11_color_topology.cpp", "tests/gfx11_color_topology_tests.cpp"]),
        ("hdmi-frl-tests", ["src/hdmi_frl.cpp", "tests/hdmi_frl_tests.cpp"]),
        ("hpo-frl-tests", ["src/discovery.cpp", "src/hpo_frl.cpp", "tests/hpo_frl_tests.cpp"]),
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
        ("vm-context-tests", ["src/discovery.cpp", "src/ip_layout.cpp", "src/vm_context.cpp", "tests/vm_context_tests.cpp"]),
        ("vm-page-tree-tests", ["src/gpu_vm.cpp", "src/vm_page_tree.cpp", "tests/vm_page_tree_tests.cpp"]),
        ("vram-allocator-tests", ["src/memory_layout.cpp", "src/vram_allocator.cpp",
                                  "tests/vram_allocator_tests.cpp"]),
        ("vram-reservations-tests", ["src/memory_layout.cpp", "src/vram_allocator.cpp",
                                     "src/vram_reservations.cpp", "tests/vram_reservations_tests.cpp"]),
        ("boot-display-tests", ["src/discovery.cpp", "src/memory_layout.cpp",
                               "src/boot_display.cpp", "tests/boot_display_tests.cpp"]),
        ("boot-display-context-tests", ["src/discovery.cpp", "src/memory_layout.cpp",
                               "src/boot_display.cpp", "src/boot_display_context.cpp", "tests/boot_display_context_tests.cpp"]),
        ("boot-display-stop-tests", ["src/discovery.cpp", "src/memory_layout.cpp",
                                    "src/boot_display.cpp", "src/boot_display_stop.cpp", "tests/boot_display_stop_tests.cpp"]),
        ("boot-display-backend-tests", ["src/discovery.cpp", "src/rom.cpp", "src/dmcub.cpp",
                                       "src/boot_display_backend.cpp", "tests/boot_display_backend_tests.cpp"]),
        ("boot-backend-access-tests", ["tests/boot_backend_access_tests.cpp"]),
        ("boot-hdmi-stream-tests", ["src/discovery.cpp", "src/rom.cpp", "src/dmcub.cpp",
                                  "src/boot_display_backend.cpp", "src/boot_hdmi_stream.cpp", "tests/boot_hdmi_stream_tests.cpp"]),
        ("boot-opp-blank-tests", ["src/discovery.cpp", "src/rom.cpp", "src/dmcub.cpp",
                                "src/boot_display_backend.cpp", "src/boot_opp_blank.cpp", "tests/boot_opp_blank_tests.cpp"]),
        ("boot-backend-stop-tests", ["src/discovery.cpp", "src/rom.cpp", "src/dmcub.cpp",
                                   "src/memory_layout.cpp", "src/vram_allocator.cpp", "src/boot_display.cpp",
                                   "src/boot_display_stop.cpp", "src/boot_display_backend.cpp",
                                   "src/boot_backend_stop.cpp", "tests/boot_backend_stop_tests.cpp"]),
        ("bootstrap-guard-inventory-tests", ["src/discovery.cpp", "src/ip_layout.cpp", "src/bootstrap_quiesce.cpp", "tests/bootstrap_guard_inventory_tests.cpp"]),
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
        ("psp-ta-tests", ["src/rom.cpp", "src/firmware.cpp", "src/discovery.cpp",
                       "src/ip_layout.cpp", "src/memory_layout.cpp", "src/vram_allocator.cpp",
                       "src/vram_transfer.cpp", "src/psp.cpp", "src/psp_ring.cpp", "src/psp_ta.cpp", "tests/psp_ta_tests.cpp"]),
        ("psp-display-ta-tests", ["src/rom.cpp", "src/firmware.cpp", "src/discovery.cpp",
                       "src/ip_layout.cpp", "src/memory_layout.cpp", "src/vram_allocator.cpp",
                       "src/vram_transfer.cpp", "src/psp.cpp", "src/psp_ring.cpp", "src/psp_display_ta.cpp", "tests/psp_display_ta_tests.cpp"]),
        ("gfx-firmware-tests", ["src/rom.cpp", "src/firmware.cpp", "src/discovery.cpp",
                       "src/ip_layout.cpp", "src/memory_layout.cpp", "src/vram_allocator.cpp",
                       "src/vram_transfer.cpp", "src/hdp.cpp", "src/psp.cpp", "src/psp_ring.cpp",
                       "src/gfx_firmware.cpp", "tests/gfx_firmware_tests.cpp"]),
        ("dp-aux-tests", ["src/discovery.cpp", "src/dp_aux.cpp", "tests/dp_aux_tests.cpp"]),
        ("hdmi-ddc-tests", ["src/discovery.cpp", "src/hdmi_ddc.cpp", "tests/hdmi_ddc_tests.cpp"]),
        ("dmcub-tests", ["src/discovery.cpp", "src/rom.cpp", "src/dmcub.cpp", "tests/dmcub_tests.cpp"]),
        ("runtime-rom-tests", ["src/discovery.cpp", "src/rom.cpp", "src/nbio.cpp", "src/runtime_rom.cpp", "tests/runtime_rom_tests.cpp"]),
        ("cold-aux-tests", ["src/discovery.cpp", "src/cold_aux.cpp", "tests/cold_aux_tests.cpp"]),
        ("audio-quiesce-tests", ["src/audio_quiesce.cpp", "tests/audio_quiesce_tests.cpp"]),
        ("efi-console-tests", ["src/memory_layout.cpp", "src/efi_console.cpp", "tests/efi_console_tests.cpp"]),
        ("hubp-aperture-tests", ["src/discovery.cpp", "src/memory_layout.cpp", "src/vram_allocator.cpp",
                                "src/hubp_scanout.cpp", "src/hubp_aperture.cpp", "tests/hubp_aperture_tests.cpp"]),
        ("early-media-tests", ["src/discovery.cpp", "src/ip_layout.cpp", "src/bootstrap_quiesce.cpp",
                               "src/cold_aux.cpp", "src/early_media.cpp", "tests/early_media_tests.cpp"]),
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
        ("smu-clock-tests", ["src/rom.cpp", "src/firmware.cpp", "src/discovery.cpp",
                       "src/ip_layout.cpp", "src/memory_layout.cpp", "src/vram_allocator.cpp",
                       "src/vram_transfer.cpp", "src/hdp.cpp", "src/psp.cpp", "src/psp_ring.cpp",
                       "src/gfx_firmware.cpp", "src/smu.cpp", "src/smu_clock.cpp", "tests/smu_clock_tests.cpp"]),
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
    display_ta_header = ROOT / ".research/inherited-hdmi/hdcp_psp.h"
    for name, sources in targets:
        if name == "psp-display-ta-tests" and not display_ta_header.is_file():
            print("Skipping PSP display TA ABI test: pinned Linux hdcp_psp.h is absent", flush=True)
            continue
        print(f"Building {name}", flush=True)
        thread_flags = ["-pthread"] if name == "retained-result-budget-tests" and os.name != "nt" else []
        oracle_flags = ["-I", str(display_ta_header.parent)] if name == "psp-display-ta-tests" else []
        subprocess.run(command + flags + thread_flags + oracle_flags + [str(ROOT / source) for source in sources] + [
                       "-o", str(output / (name + suffix))], check=True, env=env)
    for name in ("native-abi-tests", "rom-tests", "discovery-tests", "device-discovery-tests", "ip-layout-tests",
                 "retained-result-budget-tests", "display-model-input-tests", "display-dentist-tests",
                 "gfx-aco-fixture-tests", "gfx11-compute-product-tests", "gfx11-dispatch-product-tests",
                 "gfx11-graphics-barrier-tests", "gfx11-graphics-product-tests", "gfx11-fragment-buffer-tests",
                 "gfx11-graphics-draw-tests", "gfx11-buffer-draw-tests", "gfx11-graphics-memory-tests",
                 "gfx11-vertex-buffer-tests", "gfx11-vertex-buffer-draw-tests",
                 "gfx11-graphics-topology-tests", "gfx11-color-target-tests", "gfx11-color-topology-tests",
                 "audio-quiesce-tests", "early-media-tests", "efi-console-tests", "hubp-aperture-tests", "isolated-compute-tests", "hdmi-frl-tests", "hpo-frl-tests", "display-bandwidth-tests",
                 "gfx-compute-tests", "atom-vm-tests", "hdmi-link-tests", "edid-tests", "edid-acquire-tests", "otg-tests", "vpg-tests", "avi-tests", "hdmi-stream-tests", "dp-stream-tests", "display-clock-tests", "display-smu-tests", "display-dpm-tests", "display-idle-tests", "display-power-tests", "hubp-scanout-tests", "pixel-pipeline-tests",
                 "memory-layout-tests", "gpu-vm-tests", "vm-context-tests", "vm-page-tree-tests", "vram-allocator-tests", "vram-transfer-tests", "gart-tests",
                 "vram-reservations-tests", "boot-display-tests", "boot-display-context-tests", "boot-display-stop-tests", "boot-display-backend-tests", "boot-backend-access-tests", "boot-opp-blank-tests", "boot-backend-stop-tests", "boot-hdmi-stream-tests", "bootstrap-quiesce-tests", "bootstrap-guard-inventory-tests",
                 "mode1-reset-tests", "nbio-tests",
                 "sdma-queue-tests", "psp-tests", "sdma-firmware-tests", "psp-ring-tests", "psp-ta-tests", "psp-display-ta-tests", "gfx-firmware-tests", "dp-aux-tests",
                 "hdmi-ddc-tests", "smu-tests", "smu-clock-tests", "gfx-init-tests", "gfx-queue-tests",
                 "dmcub-tests", "dmcub-firmware-tests", "runtime-rom-tests", "cold-aux-tests", "cold-dma-tests", "dio-link-tests", "dp-training-tests",
                 "hdp-tests", "firmware-tests", "sdma-tests"):
        if name == "psp-display-ta-tests" and not display_ta_header.is_file():
            continue
        test_args = [str(output / (name + suffix))]
        ta_image = ROOT / ".research/firmware/amdgpu/psp_13_0_0_ta.bin"
        if name == "psp-ta-tests" and ta_image.is_file():
            test_args.append(str(ta_image))
        subprocess.run(test_args, check=True)
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
