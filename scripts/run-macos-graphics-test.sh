#!/bin/sh
# SPDX-License-Identifier: MIT
# Target-only graphics exercise. Installation/boot preparation is separate.
set -u

usage() {
    printf '%s\n' 'Usage: sh run-macos-graphics-test.sh --run --registry DECIMAL_ID --tests /absolute/Tests --output /absolute/new-directory' >&2
    exit 2
}
[ "$#" -eq 7 ] || usage
[ "$1" = --run ] && [ "$2" = --registry ] && [ "$4" = --tests ] && [ "$6" = --output ] || usage
registry=$3
tests=$5
output=$7
case "$registry" in ''|0|0*|*[!0-9]*) usage ;; esac
case "$tests" in /*) ;; *) usage ;; esac
case "$output" in /*) ;; *) usage ;; esac
[ "$(/usr/bin/uname -s)" = Darwin ] && [ "$(/usr/bin/uname -m)" = x86_64 ] || usage
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && /bin/pwd) || exit 2
collector="$script_dir/collect-macos-diagnostics.sh"
[ -f "$collector" ] && [ -d "$tests" ] || exit 2
umask 077
# No merged results from earlier runs, and no deletion on failure.
/bin/mkdir "$output" || exit 2
output=$(cd "$output" && /bin/pwd) || exit 2
tests=$(cd "$tests" && /bin/pwd) || exit 2
printf 'registry=%s\ntests=%s\n' "$registry" "$tests" > "$output/selection.txt"
printf '%s\n' 'Compute, constant triangle, fragment-buffer and vertex-buffer tests are pending.' > "$output/test-status.txt"
attempted=' '

# Collection process completion and individual query success are separate.
# This reports a missing summary as unknown; it never turns it into success.
record_collection() {
    collection_name=$1
    collection_exit=$2
    printf '%s-collection exit=%s\n' "$collection_name" "$collection_exit" >> "$output/test-status.txt"
    if [ -f "$output/$collection_name/collection-summary.txt" ]; then
        /usr/bin/sed "s/^/$collection_name-collection /" "$output/$collection_name/collection-summary.txt" >> "$output/test-status.txt"
    else
        printf '%s-collection completeness=unknown (summary missing)\n' "$collection_name" >> "$output/test-status.txt"
    fi
}

finish() {
    result=$?
    trap - 0
    printf 'graphics-attempt exit=%s\n' "$result" >> "$output/test-status.txt"
    /bin/sh "$collector" "$output/after" > "$output/after-collection.txt" 2>&1
    collected=$?
    record_collection after "$collected"
    for pending in compute triangle fragment-buffer vertex-buffer; do
        case "$attempted" in *" $pending "*) ;; *)
            printf '%s NOT_RUN: prerequisite failure or an earlier test stopped this run\n' "$pending" >> "$output/test-status.txt" ;;
        esac
    done
    if [ "$collected" -ne 0 ] && [ "$result" -eq 0 ]; then result=1; fi
    printf 'runner exit=%s\n' "$result" >> "$output/test-status.txt"
    printf 'Graphics evidence: %s (exit %s)\n' "$output" "$result"
    exit "$result"
}
trap finish 0
trap 'exit 130' INT
trap 'exit 143' TERM

/bin/sh "$collector" "$output/before" > "$output/before-collection.txt" 2>&1
collected=$?
record_collection before "$collected"
[ "$collected" -eq 0 ] || exit 1
# A registry hint is only a precondition. Each compiled tool independently
# finds the exact ID through public Metal and checks the HNMetalDevice class.
device="$output/before/device.plist"
accelerator="$output/before/accelerator.plist"
read_property() { /usr/libexec/PlistBuddy -c "Print :0:$2" "$1" 2>/dev/null; }
owner=$(read_property "$device" IORegistryEntryID)
generation=$(read_property "$device" EngineOperationGeneration)
case "$owner:$generation" in *[!0-9:]*|:*|*:|0:*|*:0) exit 1 ;; esac
if /usr/libexec/PlistBuddy -c 'Print :1' "$device" >/dev/null 2>&1 ||
   /usr/libexec/PlistBuddy -c 'Print :1' "$accelerator" >/dev/null 2>&1 ||
   [ "$(read_property "$device" EngineStartupVerified)" != true ] ||
   [ "$(read_property "$device" EngineActive)" != true ] ||
   [ "$(read_property "$accelerator" IORegistryEntryID)" != "$registry" ] ||
   [ "$(read_property "$accelerator" HackyNaviOwnerRegistryID)" != "$owner" ]; then
    printf '%s\n' 'NOT RUN: matching single live engine/accelerator was not established; inspect before/.' >> "$output/test-status.txt"
    exit 1
fi
printf 'owner=%s\noperation_generation=%s\n' "$owner" "$generation" >> "$output/selection.txt"

# Check every executable/fixture before opening any GPU device. The final
# release additionally binds this manifest and the installed bundle by hash.
/usr/bin/plutil -convert xml1 -o "$output/tests-manifest.plist" "$tests/manifest.json" > "$output/manifest-conversion.txt" 2>&1 || exit 1
for name in hackynavi-metal-compute-smoke hackynavi-metal-render-smoke \
    hackynavi-metal-render-buffer-smoke hackynavi-metal-render-vertex-buffer-smoke \
    add_one.metallib graphics.metallib fragment-buffer.metallib vertex-buffer.metallib; do
    [ -f "$tests/$name" ] && [ ! -L "$tests/$name" ] || exit 1
    expected=$(/usr/libexec/PlistBuddy -c "Print :$name:sha256" "$output/tests-manifest.plist") || exit 1
    actual=$(/usr/bin/shasum -a 256 "$tests/$name") || exit 1
    actual=${actual%% *}
    [ "$actual" = "$expected" ] || exit 1
    printf '%s  %s\n' "$actual" "$name" >> "$output/test-input-sha256.txt"
    case "$name" in hackynavi-metal-*) [ -x "$tests/$name" ] || exit 1 ;; esac
done

run_test() {
    name=$1
    shift
    attempted="$attempted$name "
    printf '%s started %s\n' "$name" "$(/bin/date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$output/test-status.txt"
    printf '%s\n' "$@" > "$output/$name.arguments"
    "$@" > "$output/$name.stdout" 2> "$output/$name.stderr"
    result=$?
    printf '%s exit=%s finished %s\n' "$name" "$result" "$(/bin/date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$output/test-status.txt"
    # Capture owner generation/retirement after each submission test, including
    # failure, so later startup/shutdown activity cannot overwrite this stage.
    /usr/sbin/ioreg -a -r -c HackyNaviDevice -l > "$output/$name.device.plist" 2> "$output/$name.device.stderr"
    snapshot_result=$?
    printf '%s.device exit=%s\n' "$name" "$snapshot_result" >> "$output/test-status.txt"
    # Preserve the first failed GPU attempt. Further submissions into a
    # failed/recovering owner do not provide independent successful evidence.
    [ "$result" -eq 0 ] || exit "$result"
    [ "$snapshot_result" -eq 0 ] &&
        [ "$(read_property "$output/$name.device.plist" IORegistryEntryID)" = "$owner" ] &&
        [ "$(read_property "$output/$name.device.plist" EngineOperationGeneration)" = "$generation" ] &&
        [ "$(read_property "$output/$name.device.plist" EngineStartupVerified)" = true ] &&
        [ "$(read_property "$output/$name.device.plist" EngineActive)" = true ] || exit 1
}
run_test compute "$tests/hackynavi-metal-compute-smoke" --run --registry "$registry" --metallib "$tests/add_one.metallib"
run_test triangle "$tests/hackynavi-metal-render-smoke" --run --registry "$registry" --metallib "$tests/graphics.metallib"
run_test fragment-buffer "$tests/hackynavi-metal-render-buffer-smoke" --run --registry "$registry" \
    --vertex-metallib "$tests/graphics.metallib" --fragment-metallib "$tests/fragment-buffer.metallib"
run_test vertex-buffer "$tests/hackynavi-metal-render-vertex-buffer-smoke" --run --registry "$registry" \
    --vertex-metallib "$tests/vertex-buffer.metallib" --constant-metallib "$tests/graphics.metallib" --fragment-metallib "$tests/fragment-buffer.metallib"
printf '%s\n' 'All four public Metal tools returned success. This does not establish desktop acceleration.' >> "$output/test-status.txt"
exit 0
