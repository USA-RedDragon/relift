#!/usr/bin/env bash
# Core names no guest OS or CPU; generated Vulkan types hold the guest layout
# on every host target, and the i386 layouts hold under an i386 compiler.
. "$(dirname "$0")/lib.sh"

make -C runtime charter
for t in x86_64-linux-gnu aarch64-linux-gnu x86_64-w64-windows-gnu aarch64-w64-windows-gnu x86_64-apple-macos aarch64-apple-macos; do
  echo "$t"
  clang --target="$t" -ffreestanding -std=c11 -Wall -Wextra -Wpedantic -Werror -fsyntax-only -x c \
    runtime/gpuapi/vk/gen/vkg_types.h
done
echo i386-linux-gnu
clang --target=i386-linux-gnu -ffreestanding -std=c11 -Wall -Wextra -Wpedantic -Werror -fsyntax-only -x c \
  runtime/gpuapi/vk/gen/i386/vki_layout.c
