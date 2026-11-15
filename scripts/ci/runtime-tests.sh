#!/usr/bin/env bash
# Build the C runtime and run its tests with the default layout, simulated
# 16 KiB host pages (Linux), the 36-bit layout, and the gdb frame filter when
# gdb is present.
. "$(dirname "$0")/lib.sh"

jobs=${CI_JOBS:-2}
make -C runtime -j"$jobs" CC=clang
make -C runtime test CC=clang
if [ "$(uname -s)" = Linux ]; then
  RELIFT_HOST_PAGE=16384 make -C runtime test CC=clang
fi
RELIFT_ADDRESS_BITS=36 make -C runtime test CC=clang
if [ "$(uname -s)" = Linux ]; then
  if command -v gdb >/dev/null; then
    make -C runtime gdb-test CC=clang
  elif [ -n "${CI:-}" ]; then
    echo "error: gdb is missing" >&2
    exit 1
  else
    echo "skipped: gdb frame filter test, no gdb"
  fi
fi
