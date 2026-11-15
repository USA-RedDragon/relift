#!/bin/sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
dest=${1:-$here}
out=$(mktemp -d "${TMPDIR:-$HOME/.cache}/pacbin.XXXXXX")
build() {
    clang --target=aarch64-none-elf -march=armv8.3-a -mbranch-protection="$1" \
        -O2 -ffreestanding -fno-builtin -fno-pic -fno-omit-frame-pointer \
        -fno-asynchronous-unwind-tables -fno-unwind-tables -nostdlib \
        -fuse-ld=lld -Wl,-T,"$here/pac.ld" -Wl,--no-relax -o "$out/$2.elf" "$here/pac.c"
    llvm-objcopy -O binary --only-section=.text --only-section=.rodata "$out/$2.elf" "$dest/$2.bin"
}
build pac-ret+b-key pac
build pac-ret+leaf+b-key pac_leaf
rm -rf "$out"
