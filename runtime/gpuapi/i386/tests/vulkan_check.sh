#!/bin/sh
# Checks the generated i386 layouts with an i386 compiler, builds the Vulkan scenario natively for i386 (the oracle, against the
# system's i386 libvulkan) and on the host through the generated Vulkan thunks,
# runs both on the ICD VK_ICD (default: lavapipe when both builds have it, else NVIDIA) and compares their reports.
#
# Usage: vulkan_check.sh <out-dir>. Needs zig (ZIG overrides), a host C
# compiler (CC, default cc), host and i386 libvulkan.so.1 (LIB32, default
# /usr/lib32) and a kernel that runs i386 programs.
set -eu
out=${1:?usage: vulkan_check.sh <out-dir>}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
zig=${ZIG:-zig}
cc=${CC:-cc}
lib32=${LIB32:-/usr/lib32}
mkdir -p "$out"

"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -o "$out/vulkan_heaps_test" "$here/vulkan_heaps_test.c"
"$out/vulkan_heaps_test"

"$zig" cc -target x86-linux-gnu -std=c11 -c -o "$out/vki_layout.o" "$root/runtime/gpuapi/vk/gen/i386/vki_layout.c"
"$zig" cc -target x86-linux-gnu -O2 -o "$out/vulkan_native" "$here/vulkan_native.c" -ldl -Wl,-rpath,"$lib32"
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -I"$root/runtime/core/include" -I"$root/runtime/core" -o "$out/vulkan_harness" \
    "$here/vulkan_harness.c" "$here/../tg.c" "$here/hostlib_stub.c" "$here/../vulkan_hand.c" "$root/runtime/gpuapi/vk/gen/i386/vki.c" -ldl -lpthread

icd=${VK_ICD:-}
if [ -z "$icd" ]; then
    for f in /usr/share/vulkan/icd.d/lvp_icd.*.json; do
        [ -f "$f" ] && icd=${icd:+$icd:}$f
    done
    if [ -z "$icd" ] && [ -e "$lib32/libvulkan_lvp.so" ]; then
        icd=/usr/share/vulkan/icd.d/lvp_icd.json
    fi
    [ -n "$icd" ] || icd=/usr/share/vulkan/icd.d/nvidia_icd.json
fi
run() {
    env VK_ICD_FILENAMES="$icd" VK_LOADER_LAYERS_DISABLE='*' "$@"
}
run env LD_LIBRARY_PATH="$lib32" "$out/vulkan_native" > "$out/vulkan_native.txt" 2>&1
run "$out/vulkan_harness" > "$out/vulkan_thunks.txt" 2>&1
if cmp -s "$out/vulkan_native.txt" "$out/vulkan_thunks.txt"; then
    echo "vulkan: thunks match native i386 ($(wc -l < "$out/vulkan_native.txt") lines)"
else
    diff "$out/vulkan_native.txt" "$out/vulkan_thunks.txt" || true
    echo "vulkan: thunks differ from native i386" >&2
    exit 1
fi
