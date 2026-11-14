#!/bin/sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d "${TMPDIR:-$HOME/.cache}/ehbin.XXXXXX")
llvm-mc -triple=aarch64 -filetype=obj -o "$out/eh.o" "$here/eh.s"
llvm-objcopy -O binary --only-section=.text "$out/eh.o" "$here/eh.bin"
rm -rf "$out"
