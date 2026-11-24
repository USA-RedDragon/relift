#!/usr/bin/env bash
# Install the pinned zig (ZIG_VERSION in lib.sh) on a hosted Linux x86-64
# runner and put it on PATH for the later steps of the job.
# Usage: install-zig.sh
. "$(dirname "$0")/lib.sh"

name=zig-x86_64-linux-$ZIG_VERSION
# The archive is checked against ZIG_SHA256, so a community mirror is as safe
# as ziglang.org; a slow or stalled one is dropped for the next after 3 minutes.
fetched=
for base in "https://ziglang.org/download/$ZIG_VERSION" "https://pkg.machengine.org/zig/$ZIG_VERSION" \
  "https://zigmirror.hryx.net/zig/$ZIG_VERSION"; do
  if (fetch "$base/$name.tar.xz" "$ZIG_SHA256" "$CI_WORK/$name.tar.xz"); then
    fetched=1
    break
  fi
  echo "install-zig: $base failed or was too slow" >&2
done
[ -n "$fetched" ] || { echo "install-zig: no mirror gave zig $ZIG_VERSION" >&2; exit 1; }
rm -rf "$CI_WORK/$name"
tar -xJf "$CI_WORK/$name.tar.xz" -C "$CI_WORK"
[ -z "${GITHUB_PATH:-}" ] || echo "$CI_WORK/$name" >>"$GITHUB_PATH"
"$CI_WORK/$name/zig" version
