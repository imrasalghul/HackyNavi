#!/bin/bash
# SPDX-License-Identifier: MIT
# Build only: no signing, installation, bootloader edits, or hardware access.
set -euo pipefail

fail() { printf 'HackyNavi build: %s\n' "$*" >&2; exit 1; }
[[ "$(uname -s)" == Darwin ]] || fail 'macOS with an Apple SDK is required; this kext cannot be built on the Windows development host.'
[[ $# -le 1 ]] || fail 'Usage: scripts/build-kext.sh [output-directory]'
command -v xcrun >/dev/null 2>&1 || fail 'xcrun is missing. Install/select Xcode with the macOS SDK and Kernel.framework headers.'
command -v python3 >/dev/null 2>&1 || fail 'python3 is required to verify and embed the pinned AMD firmware.'

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
python3 "$repo_dir/userspace/compiler/verify_fixture.py" --check
output_dir="${1:-$repo_dir/build/macos}"
sdk_dir="$(xcrun --sdk macosx --show-sdk-path)" || fail 'No macOS SDK is selected. Select an Xcode installation that includes the macOS SDK.'
sdk_version="$(xcrun --sdk macosx --show-sdk-version)" || fail 'Cannot determine the SDK version.'
sdk_major="${sdk_version%%.*}"
[[ "$sdk_major" =~ ^[0-9]+$ ]] && (( sdk_major >= 15 )) || fail "macOS SDK 15 or newer is required; found $sdk_version. The runtime target is 15.8.1, deployment baseline 15.0."
kernel_headers="$sdk_dir/System/Library/Frameworks/Kernel.framework/Headers"
[[ -f "$kernel_headers/IOKit/IOService.h" && -f "$kernel_headers/IOKit/pci/IOPCIDevice.h" ]] || fail "Kernel.framework IOKit/PCI headers are missing in $sdk_dir. Command Line Tools alone may omit them; use a full Xcode SDK with these headers."
for library in libkmodc++.a libkmod.a; do
    [[ -f "$sdk_dir/usr/lib/$library" ]] || fail "Required kernel support archive missing: $sdk_dir/usr/lib/$library"
done
# Recent Xcode versions ship the kernel compiler runtime with Clang rather
# than in the SDK. Select only the actual kernel archive, never libSystem.
kernel_runtime="$sdk_dir/usr/lib/libcc_kext.a"
if [[ ! -f "$kernel_runtime" ]]; then
    clang_resources="$(xcrun --sdk macosx clang --print-resource-dir)"
    kernel_runtime="$clang_resources/lib/darwin/libclang_rt.cc_kext.a"
fi
[[ -f "$kernel_runtime" ]] || fail "Kernel compiler runtime archive missing: $kernel_runtime"
xcrun lipo "$kernel_runtime" -verify_arch x86_64

# Cross-building on an Apple silicon Mac is allowed, but the result is Intel
# only. No exact 15.8.1 SDK is required or assumed to exist.
stage_dir="$(mktemp -d "${TMPDIR:-/tmp}/hackynavi-kext.XXXXXX")"
trap 'rm -rf -- "$stage_dir"' EXIT
bundle_dir="$stage_dir/HackyNavi.kext"
mkdir -p "$bundle_dir/Contents/MacOS" "$bundle_dir/Contents/Resources"
firmware_dir="${HACKYNAVI_FIRMWARE_DIR:-$repo_dir/.research/firmware}"
python3 "$repo_dir/scripts/embed-firmware.py" --input-dir "$firmware_dir" --output-dir "$stage_dir/firmware"
flags=(
    -arch x86_64 -isysroot "$sdk_dir" -mmacosx-version-min=15.0
    -std=c++14 -mkernel -fapple-kext -DKERNEL -DKERNEL_EXTENSION
    -nostdinc -isystem "$kernel_headers" -I"$repo_dir/include"
    -fno-exceptions -fno-rtti -fno-builtin -fno-common -fno-stack-protector
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2
    # This target intentionally uses the macOS kernel PCI/DMA APIs. SDK26
    # deprecates them in favor of DriverKit, which is a different driver ABI.
    -Wall -Wextra -Werror -Wno-unused-parameter -Wno-deprecated-declarations -O2
)
sources=(
    "$repo_dir/kext/HackyNavi/HackyNavi.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviDevice.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviDeviceEngines.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviClientSession.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviUserClient.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviClientMemory.cpp"
    "$repo_dir/userspace/transport/native_protocol.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviDMABuffer.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviVMContext.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviVMProduct.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviVMGraphics.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviVMCompletion.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviGARTStorage.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviGARTController.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviGARTTopology.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviGARTGraphicsTopology.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviSDMAQueue.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviFirmwareController.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviFirmwareTA.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviFirmwareClocks.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviGraphicsQueue.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviGraphicsQueueReuse.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviDisplayController.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviDisplayClockControl.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviDisplayFirmwareClocks.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviEngineController.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviResetController.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviAtomController.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviLifecycleController.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviFunctionGroup.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviBootConsole.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviBootBackendCapture.cpp"
    "$repo_dir/kext/HackyNavi/HackyNaviScanoutSurface.cpp"
    "$repo_dir/src/hubp_aperture.cpp"
    "$repo_dir/src/gfx_compute.cpp"
    "$repo_dir/src/isolated_compute.cpp"
    "$repo_dir/src/gfx_aco_fixture.cpp"
    "$repo_dir/src/gfx11_compute_product.cpp"
    "$repo_dir/src/gfx11_dispatch_product.cpp"
    "$repo_dir/src/gfx11_graphics_barrier.cpp"
    "$repo_dir/src/gfx11_graphics_product.cpp"
    "$repo_dir/src/gfx11_graphics_draw.cpp"
    "$repo_dir/src/gfx11_graphics_memory.cpp"
    "$repo_dir/src/gfx11_color_target.cpp"
    "$repo_dir/src/gfx11_color_topology.cpp"
    "$repo_dir/src/gfx11_graphics_topology.cpp"
    "$repo_dir/src/hdmi_frl.cpp"
    "$repo_dir/src/hpo_frl.cpp"
    "$repo_dir/src/hdmi_link.cpp"
    "$repo_dir/src/atom_vm.cpp"
    "$repo_dir/src/edid.cpp"
    "$repo_dir/src/edid_acquire.cpp"
    "$repo_dir/src/otg.cpp"
    "$repo_dir/src/vpg.cpp"
    "$repo_dir/src/avi.cpp"
    "$repo_dir/src/hdmi_stream.cpp"
    "$repo_dir/src/dp_stream.cpp"
    "$repo_dir/src/display_clock.cpp"
    "$repo_dir/src/display_bandwidth.cpp"
    "$repo_dir/src/display_smu.cpp"
    "$repo_dir/src/display_model_input.cpp"
    "$repo_dir/src/display_dentist.cpp"
    "$repo_dir/src/display_idle.cpp"
    "$repo_dir/src/display_power.cpp"
    "$repo_dir/src/hubp_scanout.cpp"
    "$repo_dir/src/pixel_pipeline.cpp"
    "$repo_dir/src/rom.cpp"
    "$repo_dir/src/discovery.cpp"
    "$repo_dir/src/device_discovery.cpp"
    "$repo_dir/src/ip_layout.cpp"
    "$repo_dir/src/memory_layout.cpp"
    "$repo_dir/src/gpu_vm.cpp"
    "$repo_dir/src/vm_context.cpp"
    "$repo_dir/src/vm_page_tree.cpp"
    "$repo_dir/src/vram_allocator.cpp"
    "$repo_dir/src/vram_reservations.cpp"
    "$repo_dir/src/boot_display.cpp"
    "$repo_dir/src/boot_display_context.cpp"
    "$repo_dir/src/boot_display_stop.cpp"
    "$repo_dir/src/boot_display_backend.cpp"
    "$repo_dir/src/bootstrap_quiesce.cpp"
    "$repo_dir/src/mode1_reset.cpp"
    "$repo_dir/src/nbio.cpp"
    "$repo_dir/src/sdma.cpp"
    "$repo_dir/src/sdma_queue.cpp"
    "$repo_dir/src/firmware.cpp"
    "$repo_dir/src/psp.cpp"
    "$repo_dir/src/sdma_firmware.cpp"
    "$repo_dir/src/psp_ring.cpp"
    "$repo_dir/src/psp_ta.cpp"
    "$repo_dir/src/psp_display_ta.cpp"
    "$repo_dir/src/gfx_firmware.cpp"
    "$repo_dir/src/dp_aux.cpp"
    "$repo_dir/src/hdmi_ddc.cpp"
    "$repo_dir/src/dmcub.cpp"
    "$repo_dir/src/dmcub_firmware.cpp"
    "$repo_dir/src/runtime_rom.cpp"
    "$repo_dir/src/cold_aux.cpp"
    "$repo_dir/src/audio_quiesce.cpp"
    "$repo_dir/src/early_media.cpp"
    "$repo_dir/src/efi_console.cpp"
    "$repo_dir/src/cold_dma.cpp"
    "$repo_dir/src/dio_link.cpp"
    "$repo_dir/src/dp_training.cpp"
    "$repo_dir/src/smu.cpp"
    "$repo_dir/src/smu_clock.cpp"
    "$repo_dir/src/gfx_init.cpp"
    "$repo_dir/src/gfx_queue.cpp"
    "$repo_dir/src/vram_transfer.cpp"
    "$repo_dir/src/gart.cpp"
    "$repo_dir/src/hdp.cpp"
    "$repo_dir/kext/HackyNavi/Module.cpp"
    "$stage_dir/firmware/FirmwareResources.cpp"
)
objects=()
for source in "${sources[@]}"; do
    object="$stage_dir/$(basename "${source%.cpp}").o"
    xcrun --sdk macosx clang++ "${flags[@]}" -c "$source" -o "$object"
    objects+=("$object")
done

binary="$bundle_dir/Contents/MacOS/HackyNavi"
xcrun --sdk macosx clang++ -arch x86_64 -isysroot "$sdk_dir" \
    -mmacosx-version-min=15.0 -fapple-kext -nostdlib -Wl,-kext \
    "${objects[@]}" -L"$sdk_dir/usr/lib" -lkmodc++ -lkmod "$kernel_runtime" \
    -o "$binary"
cp "$repo_dir/kext/HackyNavi/Info.plist" "$bundle_dir/Contents/Info.plist"
cp "$repo_dir/LICENSE" "$bundle_dir/Contents/Resources/LICENSE"
cp "$repo_dir/THIRD-PARTY-NOTICES.md" "$bundle_dir/Contents/Resources/THIRD-PARTY-NOTICES.md"
cp "$stage_dir/firmware/LICENSE.amdgpu" "$bundle_dir/Contents/Resources/LICENSE.amdgpu"
cp "$stage_dir/firmware/FirmwareManifest.json" "$bundle_dir/Contents/Resources/FirmwareManifest.json"
plutil -lint "$bundle_dir/Contents/Info.plist"
xcrun lipo "$binary" -verify_arch x86_64
# Mach-O prefixes C symbol names with another underscore. Require platform
# start/stop wrappers and module metadata to be defined, not unresolved.
# The kext linker may internalize entry points addressed by kmod_info; they
# need not be exported globals to be valid definitions.
xcrun nm -U "$binary" > "$stage_dir/symbols.txt"
for symbol in __start __stop _kmod_info; do
    grep -Eq "[[:space:]]${symbol}$" "$stage_dir/symbols.txt" || fail "Required defined symbol missing: $symbol"
done
mkdir -p "$output_dir/HackyNavi.kext/Contents/MacOS" "$output_dir/HackyNavi.kext/Contents/Resources"
cp "$binary" "$output_dir/HackyNavi.kext/Contents/MacOS/HackyNavi"
cp "$bundle_dir/Contents/Info.plist" "$output_dir/HackyNavi.kext/Contents/Info.plist"
cp "$bundle_dir/Contents/Resources/LICENSE" "$output_dir/HackyNavi.kext/Contents/Resources/LICENSE"
cp "$bundle_dir/Contents/Resources/THIRD-PARTY-NOTICES.md" "$output_dir/HackyNavi.kext/Contents/Resources/THIRD-PARTY-NOTICES.md"
cp "$bundle_dir/Contents/Resources/LICENSE.amdgpu" "$output_dir/HackyNavi.kext/Contents/Resources/LICENSE.amdgpu"
cp "$bundle_dir/Contents/Resources/FirmwareManifest.json" "$output_dir/HackyNavi.kext/Contents/Resources/FirmwareManifest.json"
printf 'Built unsigned x86_64 HackyNavi kext: %s/HackyNavi.kext\n' "$output_dir"
printf 'SDK: %s; deployment baseline: macOS 15.0; intended runtime: macOS 15.8.1.\n' "$sdk_version"
printf 'Build success does not validate loading or GPU behavior. No installation was performed.\n'
