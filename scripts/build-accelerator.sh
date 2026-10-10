#!/bin/bash
# SPDX-License-Identifier: MIT
# Build the late graphics-family client. No installation or loading.
set -euo pipefail
fail() { printf 'HackyNavi accelerator build: %s\n' "$*" >&2; exit 1; }
[[ "$(uname -s)" == Darwin ]] || fail 'An Apple macOS SDK is required.'
[[ $# -le 1 ]] || fail 'Usage: scripts/build-accelerator.sh [output-directory]'
repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
output_dir="${1:-$repo_dir/build/macos}"
sdk_dir="$(xcrun --sdk macosx --show-sdk-path)"
kernel_headers="$sdk_dir/System/Library/Frameworks/Kernel.framework/Headers"
[[ -f "$kernel_headers/IOKit/graphics/IOAccelerator.h" ]] || fail 'Kernel graphics headers missing.'
kernel_runtime="$sdk_dir/usr/lib/libcc_kext.a"
if [[ ! -f "$kernel_runtime" ]]; then
    clang_resources="$(xcrun --sdk macosx clang --print-resource-dir)"
    kernel_runtime="$clang_resources/lib/darwin/libclang_rt.cc_kext.a"
fi
[[ -f "$kernel_runtime" ]] || fail 'Kernel compiler runtime missing.'
xcrun lipo "$kernel_runtime" -verify_arch x86_64
stage_dir="$(mktemp -d "${TMPDIR:-/tmp}/hackynavi-accelerator.XXXXXX")"
trap 'rm -rf -- "$stage_dir"' EXIT
bundle_dir="$stage_dir/HackyNaviAccelerator.kext"
mkdir -p "$bundle_dir/Contents/MacOS" "$bundle_dir/Contents/Resources"
flags=(
    -arch x86_64 -isysroot "$sdk_dir" -mmacosx-version-min=15.0
    -std=c++14 -mkernel -fapple-kext -DKERNEL -DKERNEL_EXTENSION
    -nostdinc -isystem "$kernel_headers" -I"$repo_dir/include"
    -fno-exceptions -fno-rtti -fno-builtin -fno-common -fno-stack-protector
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2
    -Wall -Wextra -Werror -Wno-unused-parameter -Wno-deprecated-declarations
    -Wframe-larger-than=2048 -O2
)
sources=(
    "$repo_dir/kext/HackyNaviAccelerator/HackyNaviAccelerator.cpp"
    "$repo_dir/kext/HackyNaviAccelerator/Module.cpp"
)
objects=()
for source in "${sources[@]}"; do
    object="$stage_dir/$(basename "${source%.cpp}").o"
    xcrun --sdk macosx clang++ "${flags[@]}" -c "$source" -o "$object"
    objects+=("$object")
done
binary="$bundle_dir/Contents/MacOS/HackyNaviAccelerator"
xcrun --sdk macosx clang++ -arch x86_64 -isysroot "$sdk_dir" \
    -mmacosx-version-min=15.0 -fapple-kext -nostdlib -Wl,-kext \
    "${objects[@]}" -L"$sdk_dir/usr/lib" -lkmodc++ -lkmod "$kernel_runtime" -o "$binary"
cp "$repo_dir/kext/HackyNaviAccelerator/Info.plist" "$bundle_dir/Contents/Info.plist"
cp "$repo_dir/LICENSE" "$bundle_dir/Contents/Resources/LICENSE"
plutil -lint "$bundle_dir/Contents/Info.plist"
xcrun lipo "$binary" -verify_arch x86_64
mkdir -p "$output_dir/HackyNaviAccelerator.kext/Contents/MacOS" \
    "$output_dir/HackyNaviAccelerator.kext/Contents/Resources"
cp "$binary" "$output_dir/HackyNaviAccelerator.kext/Contents/MacOS/HackyNaviAccelerator"
cp "$bundle_dir/Contents/Info.plist" "$output_dir/HackyNaviAccelerator.kext/Contents/Info.plist"
cp "$repo_dir/LICENSE" "$output_dir/HackyNaviAccelerator.kext/Contents/Resources/LICENSE"
printf 'Built unsigned late accelerator: %s/HackyNaviAccelerator.kext\n' "$output_dir"
printf 'Requires the real IOGraphicsFamily provider; do not inject this bundle into BootKC.\n'
