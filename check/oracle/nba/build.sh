#!/bin/sh
# Fetches NanoBoyAdvance at a pinned commit and builds the headless GBA
# oracle (nba-oracle) against its core library. NanoBoyAdvance is
# GPL-3.0-or-later: the binary stays outside relift and runs as a separate
# process. Usage: build.sh [cache dir] [build dir]
# The cache dir (default ~/.cache/relift/gba-oracle) gets src/nba and
# bin/nba-oracle; patches in RELIFT_NBA_PATCHES (a directory of *.patch,
# applied in name order) are applied to the checkout first.
set -eu
repo=https://codeberg.org/nba-emu/NanoBoyAdvance
commit=e8482c1efc6cde3f553e8170a51abea71cee426e
here=$(cd "$(dirname "$0")" && pwd)
cache=${1:-$HOME/.cache/relift/gba-oracle}
build=${2:-$cache/build}
src=$cache/src/nba
cc=${CC:-clang}
cxx=${CXX:-clang++}
jobs=${JOBS:-8}

mkdir -p "$cache/bin"
rm -rf "$src"
mkdir -p "$src"
git -C "$src" init -q
git -C "$src" remote add origin "$repo"
git -C "$src" fetch -q --depth 1 origin "$commit"
git -C "$src" -c advice.detachedHead=false checkout -q FETCH_HEAD
test "$(git -C "$src" rev-parse HEAD)" = "$commit"

if [ -n "${RELIFT_NBA_PATCHES:-}" ]; then
    for p in "$RELIFT_NBA_PATCHES"/*.patch; do
        [ -e "$p" ] || continue
        git -C "$src" apply --whitespace=nowarn "$p"
        echo "applied $(basename "$p")"
    done
fi

cmake -S "$here" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" -DNBA_SOURCE="$src"
cmake --build "$build" -j "$jobs" --target nba-oracle
cp "$build/nba-oracle" "$cache/bin/nba-oracle"
echo "$cache/bin/nba-oracle"
