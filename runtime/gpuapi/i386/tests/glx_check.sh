#!/bin/sh
# Builds the GLX scenario (the OpenGL one over GLX) for i386 (the oracle, against the
# system's i386 libGL and libEGL) and on the host through the generated GL and the GLX hand
# thunks, runs both on Mesa's llvmpipe on a private Xvfb (or $DISPLAY when
# XVFB=0) and compares their reports.
#
# Usage: glx_check.sh <out-dir>. Needs zig (ZIG overrides), a host C compiler
# (CC, default cc), i386 and host libGL, libEGL and Mesa (LIB32, default
# /usr/lib32) and a kernel that runs i386 programs. The context comes from
# EGL through its thunks, so this checks the EGL thunks too.
set -eu
out=${1:?usage: glx_check.sh <out-dir>}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../../.." && pwd)
zig=${ZIG:-zig}
cc=${CC:-cc}
lib32=${LIB32:-/usr/lib32}
mkdir -p "$out"

"$zig" cc -target x86-linux-gnu -O2 -o "$out/glx_native" "$here/glx_native.c" -ldl -Wl,-rpath,"$lib32"
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -I"$root/runtime/core/include" -I"$root/runtime/core" -o "$out/glx_harness" \
    "$here/glx_harness.c" "$here/../tg.c" "$here/hostlib_stub.c" "$here/../gl_hand.c" "$here/../glx_hand.c" "$root/runtime/gpuapi/gl/gen/i386/gl.c" -ldl -lpthread

vendor=
for d in /usr/share/glvnd/egl_vendor.d /etc/glvnd/egl_vendor.d; do
    for f in "$d"/*mesa*.json; do
        [ -f "$f" ] && vendor=$f && break 2
    done
done
[ -n "$vendor" ] || { echo "glx: no Mesa EGL vendor file" >&2; exit 1; }
if [ "${XVFB:-1}" = 1 ]; then
    n=97
    while [ -e "/tmp/.X11-unix/X$n" ]; do n=$((n + 1)); done
    # MIT-SHM is off: llvmpipe's software presentation sends ShmPutImage requests
    # that Xvfb rejects for pbuffers (BadMatch at exit).
    Xvfb ":$n" -screen 0 320x240x24 -nolisten tcp -extension MIT-SHM >/dev/null 2>&1 &
    xvfb=$!
    trap 'kill "$xvfb" 2>/dev/null' EXIT
    for _ in $(seq 50); do [ -e "/tmp/.X11-unix/X$n" ] && break; sleep 0.1; done
    export DISPLAY=":$n"
fi
run() {
    env __EGL_VENDOR_LIBRARY_FILENAMES="$vendor" \
        LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe MESA_SHADER_CACHE_DISABLE=1 "$@"
}
run env LD_LIBRARY_PATH="$lib32" "$out/glx_native" > "$out/glx_native.txt"
run "$out/glx_harness" > "$out/glx_thunks.txt"
if cmp -s "$out/glx_native.txt" "$out/glx_thunks.txt"; then
    echo "glx: thunks match native i386 ($(wc -l < "$out/glx_native.txt") lines)"
else
    diff "$out/glx_native.txt" "$out/glx_thunks.txt" || true
    echo "glx: thunks differ from native i386" >&2
    exit 1
fi
