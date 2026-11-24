#!/bin/sh
# Builds the OpenGL scenario natively for i386 (the oracle, against the
# system's i386 libGL and libEGL) and on the host through the generated GL
# thunks, runs both on Mesa's llvmpipe and compares their reports.
#
# Usage: gl_check.sh <out-dir>. Needs zig (ZIG overrides), a host C compiler
# (CC, default cc), i386 and host libGL, libEGL and Mesa (LIB32, default
# /usr/lib32) and a kernel that runs i386 programs. The context comes from
# EGL through its thunks, so this checks the EGL thunks too.
set -eu
out=${1:?usage: gl_check.sh <out-dir>}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../../.." && pwd)
zig=${ZIG:-zig}
cc=${CC:-cc}
lib32=${LIB32:-/usr/lib32}
mkdir -p "$out"

"$zig" cc -target x86-linux-gnu -O2 -o "$out/gl_native" "$here/gl_native.c" -ldl -Wl,-rpath,"$lib32"
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -I"$root/runtime/core/include" -I"$root/runtime/core" -o "$out/gl_harness" \
    "$here/gl_harness.c" "$here/../tg.c" "$here/hostlib_stub.c" "$here/../gl_hand.c" "$here/../glx_hand.c" "$here/../egl_hand.c" "$root/runtime/gpuapi/gl/gen/i386/gl.c" "$root/runtime/gpuapi/egl/gen/i386/egl.c" -ldl -lpthread

vendor=
for d in /usr/share/glvnd/egl_vendor.d /etc/glvnd/egl_vendor.d; do
    for f in "$d"/*mesa*.json; do
        [ -f "$f" ] && vendor=$f && break 2
    done
done
[ -n "$vendor" ] || { echo "gl: no Mesa EGL vendor file" >&2; exit 1; }
run() {
    env __EGL_VENDOR_LIBRARY_FILENAMES="$vendor" EGL_PLATFORM=surfaceless \
        LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe MESA_SHADER_CACHE_DISABLE=1 "$@"
}
run env LD_LIBRARY_PATH="$lib32" "$out/gl_native" > "$out/gl_native.txt"
run "$out/gl_harness" > "$out/gl_thunks.txt"
if cmp -s "$out/gl_native.txt" "$out/gl_thunks.txt"; then
    echo "gl: thunks match native i386 ($(wc -l < "$out/gl_native.txt") lines)"
else
    diff "$out/gl_native.txt" "$out/gl_thunks.txt" || true
    echo "gl: thunks differ from native i386" >&2
    exit 1
fi
