#!/usr/bin/env bash
# The Android host layer (runtime/host/android_*.c) compiles for android-arm64
# with the NDK under -Wall -Werror, against the pinned SDL3 headers. The cross
# jobs build programs without SDL3, so they never compile these files: a call
# to a function nothing defines reached main that way. Syntax-only, no link.
# Usage: android-host-check.sh
. "$(dirname "$0")/lib.sh"

ndk=
for c in "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}" "${ANDROID_NDK_LATEST_HOME:-}" /opt/android-ndk; do
  [ -n "$c" ] && [ -x "$c/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" ] && { ndk=$c; break; }
done
if [ -z "$ndk" ]; then
  ndk=$(ls -d /opt/android-sdk/ndk/*/ 2>/dev/null | sort -V | tail -1)
  ndk=${ndk%/}
fi
[ -x "$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" ] || {
  echo "android-host-check: no Android NDK (set ANDROID_NDK_HOME)" >&2
  exit 1
}
clang="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/clang"

ver=$(sed -n 's/^SDL_VERSION = "\(.*\)"/\1/p' runtime/tools/cross/android_deps.py)
sha=$(sed -n 's/^SDL_SHA256 = "\(.*\)"/\1/p' runtime/tools/cross/android_deps.py)
tgz="$CI_WORK/SDL3-$ver.tar.gz"
fetch "https://github.com/libsdl-org/SDL/releases/download/release-$ver/SDL3-$ver.tar.gz" "$sha" "$tgz"
sdl="$CI_WORK/sdl3-headers-$ver"
if [ ! -f "$sdl/include/SDL3/SDL.h" ]; then
  rm -rf "$sdl"
  mkdir -p "$sdl"
  tar -xzf "$tgz" -C "$sdl" --strip-components=1 "SDL3-$ver/include"
fi

fail=0
for f in runtime/host/android_sdl3.c runtime/host/android_display.c; do
  if "$clang" --target=aarch64-linux-android30 -std=c11 -fsyntax-only -Wall -Wextra -Werror \
    -Wno-unused-parameter -D_GNU_SOURCE \
    -Iruntime/core/include -Iruntime/core -Iruntime/host -Iruntime/guests/switch -I"$sdl/include" "$f"; then
    echo "ok   $f"
  else
    echo "FAIL $f"
    fail=1
  fi
done
exit $fail
