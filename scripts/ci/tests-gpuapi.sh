#!/usr/bin/env bash
# The i386 GPU-API forwarding (runtime/gpuapi/i386) against native i386
# libraries: OpenGL over EGL, OpenGL over GLX on a private Xvfb, and Vulkan
# on lavapipe, each scenario built natively for i386 with zig and on the
# host through the generated thunks, with the two reports compared. Without
# RELIFT_REQUIRE_NATIVE, a machine that lacks zig or the i386 libraries
# skips the check it cannot run.
. "$(dirname "$0")/lib.sh"

work="$CI_WORK/gpuapi"
rm -rf "$work"
mkdir -p "$work"
lib32=${LIB32:-/usr/lib32}
[ -d "$lib32" ] || lib32=/usr/lib/i386-linux-gnu
export LIB32=$lib32

have() {
  command -v "${ZIG:-zig}" >/dev/null && [ -e "$lib32/$1" ]
}

for check in gl:libEGL.so.1 glx:libGL.so.1 vulkan:libvulkan.so.1; do
  name=${check%%:*}
  lib=${check#*:}
  if ! have "$lib"; then
    if [ -n "${RELIFT_REQUIRE_NATIVE:-}" ]; then
      echo "gpuapi: $name needs zig and i386 $lib" >&2
      exit 1
    fi
    echo "gpuapi: $name skipped (no zig or no i386 $lib)"
    continue
  fi
  sh runtime/gpuapi/i386/tests/${name}_check.sh "$work/$name"
done
