#!/bin/sh
# Fetches ares at a pinned commit and builds z80-step, the one-instruction
# stepper over ares' Z80 core, psg-step, the sample stepper over its SN76489,
# and ym2413-step, the sample stepper over its YM2413,
# with clang++. ares is ISC; the binary stays
# outside relift and runs as a separate process. Usage: build-step.sh [cache dir]
# The cache dir (default ~/.cache/relift/ares-oracle) gets src/ (the ares
# tree), build/ and bin/z80-step, bin/psg-step and bin/ym2413-step.
set -eu
commit=0b928930af4cc59ed99e942996f0d3c815379388
url=https://github.com/ares-emulator/ares/archive/$commit.tar.gz
sha256=c98eced8748260611073d0165e2d491645e371660537714806664ece2abe465b
here=$(cd "$(dirname "$0")" && pwd)
cache=${1:-$HOME/.cache/relift/ares-oracle}
cxx=${CXX:-clang++}
src=$cache/src

mkdir -p "$cache/bin" "$cache/build"
if [ ! -f "$src/.commit-$commit" ]; then
    tgz=$cache/ares-$commit.tar.gz
    if ! echo "$sha256  $tgz" | sha256sum -c - >/dev/null 2>&1; then
        curl -fsSL -o "$tgz.part" "$url"
        echo "$sha256  $tgz.part" | sha256sum -c -
        mv "$tgz.part" "$tgz"
    fi
    rm -rf "$src"
    mkdir -p "$src"
    tar xzf "$tgz" -C "$src" --strip-components=1
    touch "$src/.commit-$commit"
fi

flags="-std=c++20 -O1 -w -fno-strict-aliasing -fwrapv -fno-char8_t -DPROFILE_PERFORMANCE -DBUILD_RELEASE -DBUILD_LOCAL -I$src/ares -I$src -I$src/nall -I$src/thirdparty -I$here"
if [ ! -f "$cache/build/nall.o" ]; then
    # shellcheck disable=SC2086
    $cxx $flags -DSLJIT_HAVE_CONFIG_PRE=1 -DSLJIT_HAVE_CONFIG_POST=1 -c "$src/nall/nall/nall.cpp" -o "$cache/build/nall.o"
fi
# shellcheck disable=SC2086
$cxx $flags "$here/z80-step.cpp" "$src/ares/component/processor/z80/z80.cpp" "$here/ares.gen.cpp" \
    "$cache/build/nall.o" -o "$cache/bin/z80-step" -lpthread
# shellcheck disable=SC2086
$cxx $flags "$here/psg-step.cpp" "$src/ares/component/audio/sn76489/sn76489.cpp" "$here/ares.gen.cpp" \
    "$cache/build/nall.o" -o "$cache/bin/psg-step" -lpthread
# shellcheck disable=SC2086
$cxx $flags "$here/ym2413-step.cpp" "$src/ares/component/audio/ym2413/ym2413.cpp" "$here/ares.gen.cpp" \
    "$cache/build/nall.o" -o "$cache/bin/ym2413-step" -lpthread
echo "$cache/bin/z80-step"
echo "$cache/bin/psg-step"
echo "$cache/bin/ym2413-step"
