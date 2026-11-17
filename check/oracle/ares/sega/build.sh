#!/bin/sh
# Fetches ares at a pinned commit, applies patches/, and builds sega-oracle,
# the headless Master System, Game Gear and SG-1000 oracle. ares is ISC;
# the binary stays outside relift and runs as a separate process.
# Usage: build.sh [cache dir] [build dir]
# The cache dir (default ~/.cache/relift/sega-oracle) gets src/ (the patched
# ares tree), build/ and bin/sega-oracle.
set -eu
commit=0b928930af4cc59ed99e942996f0d3c815379388
url=https://github.com/ares-emulator/ares/archive/$commit.tar.gz
sha256=c98eced8748260611073d0165e2d491645e371660537714806664ece2abe465b
here=$(cd "$(dirname "$0")" && pwd)
cache=${1:-$HOME/.cache/relift/sega-oracle}
build=${2:-$cache/build}
src=$cache/src
cc=${CC:-clang}
cxx=${CXX:-clang++}
jobs=${JOBS:-4}

mkdir -p "$cache/bin"
tgz=$cache/ares-$commit.tar.gz
if ! echo "$sha256  $tgz" | sha256sum -c - >/dev/null 2>&1; then
    curl -fsSL -o "$tgz.part" "$url"
    echo "$sha256  $tgz.part" | sha256sum -c -
    mv "$tgz.part" "$tgz"
fi
rm -rf "$src"
mkdir -p "$src"
tar xzf "$tgz" -C "$src" --strip-components=1
for p in "$here"/patches/*.patch; do
    patch -s -d "$src" -p1 <"$p"
    echo "applied $(basename "$p")"
done

cmake -S "$here" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" -DARES_SOURCE="$src"
cmake --build "$build" -j "$jobs" --target sega-oracle
cp "$build/sega-oracle" "$cache/bin/sega-oracle"
echo "$cache/bin/sega-oracle"
