#!/bin/sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d "${TMPDIR:-$HOME/.cache}/retbin.XXXXXX")
llvm-mc -triple=aarch64 -filetype=obj -o "$out/ret.o" "$here/ret.s"
llvm-objcopy -O binary --only-section=.text "$out/ret.o" "$here/ret.bin"
rm -rf "$out"
