#!/bin/sh
# Builds the native oracle harness for AArch64 Linux (run under
# qemu-aarch64 or on an AArch64 Linux host) and AArch64 macOS (an Apple
# silicon Mac over ssh). Usage: build.sh <zig> <out-dir>
set -eu
zig=$1
out=$2
src=$(dirname "$0")/harness.c
mkdir -p "$out"
flags="-std=c11 -O2 -Wall -Wextra -Werror"
"$zig" cc -target aarch64-linux-musl $flags -static -o "$out/harness-linux" "$src"
"$zig" cc -target aarch64-macos $flags -o "$out/harness-macos" "$src"
