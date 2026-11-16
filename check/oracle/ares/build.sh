#!/bin/sh
# Fetches ares at a pinned commit, applies patches/*.patch and builds the
# headless Nintendo 64 oracle (n64-oracle) against the n64 core. ares is ISC:
# the binary runs as a separate process. Usage: build.sh [cache dir] [build dir]
# The cache dir (default ~/.cache/relift/n64-ares) gets src/ares and
# bin/n64-oracle.
set -eu
repo=https://github.com/ares-emulator/ares
commit=0b928930af4cc59ed99e942996f0d3c815379388
here=$(cd "$(dirname "$0")" && pwd)
cache=${1:-$HOME/.cache/relift/n64-ares}
build=${2:-$cache/build}
src=$cache/src/ares
cc=${CC:-clang}
cxx=${CXX:-clang++}
jobs=${JOBS:-4}

mkdir -p "$cache/bin"
rm -rf "$src"
mkdir -p "$src"
git -C "$src" init -q
git -C "$src" remote add origin "$repo"
git -C "$src" fetch -q --depth 1 origin "$commit"
git -C "$src" -c advice.detachedHead=false checkout -q FETCH_HEAD
test "$(git -C "$src" rev-parse HEAD)" = "$commit"

for p in "$here"/patches/*.patch; do
    [ -e "$p" ] || continue
    git -C "$src" apply --whitespace=nowarn "$p"
    echo "applied $(basename "$p")"
done

cmake -S "$here" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" -DARES_SOURCE="$src"
cmake --build "$build" -j "$jobs" --target n64-oracle
cp "$build/n64-oracle" "$cache/bin/n64-oracle"
echo "$cache/bin/n64-oracle"
