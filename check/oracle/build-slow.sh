#!/bin/sh
# Builds the slow-path engine (slow.c) for this host from the runtime's FP
# sources. Usage: build-slow.sh <out-dir> [cc]
set -eu
out=$1
cc=${2:-clang}
here=$(dirname "$0")
rt=$here/../../runtime
mkdir -p "$out"
case $("$cc" -dumpmachine) in x86_64*) arch=-mcx16 ;; *) arch= ;; esac
"$cc" -std=c11 -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -DRL_BUILDING_RUNTIME $arch \
    -I"$rt/core/include" -I"$rt/core" -I"$rt/cpus/arm64/include" -I"$rt/cpus/arm64" \
    -o "$out/slow" "$here/slow.c" "$rt/cpus/arm64/fp.c" "$rt/cpus/arm64/fp_slow.c" -lm
