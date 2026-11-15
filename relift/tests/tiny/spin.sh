#!/bin/sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d "${TMPDIR:-$HOME/.cache}/spinbin.XXXXXX")
llvm-mc -triple=aarch64 -filetype=obj -o "$out/spin.o" "$here/spin.s"
llvm-objcopy -O binary --only-section=.text "$out/spin.o" "$here/spin.bin"
rm -rf "$out"
