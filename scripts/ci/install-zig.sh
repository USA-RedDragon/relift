#!/usr/bin/env bash
# Install the pinned zig (ZIG_VERSION in lib.sh) on a hosted Linux x86-64
# runner and put it on PATH for the later steps of the job.
# Usage: install-zig.sh
. "$(dirname "$0")/lib.sh"

name=zig-x86_64-linux-$ZIG_VERSION
fetch "https://ziglang.org/download/$ZIG_VERSION/$name.tar.xz" "$ZIG_SHA256" "$CI_WORK/$name.tar.xz"
rm -rf "$CI_WORK/$name"
tar -xJf "$CI_WORK/$name.tar.xz" -C "$CI_WORK"
[ -z "${GITHUB_PATH:-}" ] || echo "$CI_WORK/$name" >>"$GITHUB_PATH"
"$CI_WORK/$name/zig" version
