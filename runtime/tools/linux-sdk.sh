#!/bin/sh
# Builds SDL3 and libopus as shared libraries against an old glibc, into the
# sdk folder of relift's cache, so that `relift build` makes AppImages that
# run on distributions older than this machine.
#
#   linux-sdk.sh [CACHE_DIR]
#
# CACHE_DIR defaults to relift's: $XDG_CACHE_HOME/relift or ~/.cache/relift.
# Needs zig (0.17 or newer), cmake, curl, tar, sha256sum, and the development
# headers of the display and audio servers SDL3 loads when it runs (X11,
# Wayland, ALSA, PulseAudio, libudev, D-Bus; any that is missing is left
# out). The sources are pinned by hash and fetched into CACHE_DIR/sdk-src.
#
# Result, per library: CACHE_DIR/sdk/<lib>/<arch>-linux/{include,lib}, and
# the glibc the libraries were built for in CACHE_DIR/sdk/sdl3/<arch>-linux/glibc.
# relift builds Linux releases with `zig cc` for that glibc when it finds
# the file.

set -eu

GLIBC=2.28
SDL3_VERSION=3.4.18
SDL3_URL=https://github.com/libsdl-org/SDL/releases/download/release-$SDL3_VERSION/SDL3-$SDL3_VERSION.tar.gz
SDL3_SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
OPUS_VERSION=1.6.1
OPUS_URL=https://downloads.xiph.org/releases/opus/opus-$OPUS_VERSION.tar.gz
OPUS_SHA256=6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1

cache=${1:-${XDG_CACHE_HOME:-$HOME/.cache}/relift}
arch=$(uname -m)
case $arch in
x86_64 | aarch64) ;;
*)
    echo "linux-sdk.sh: $arch is not supported" >&2
    exit 1
    ;;
esac

for tool in zig cmake curl tar sha256sum; do
    command -v "$tool" >/dev/null || {
        echo "linux-sdk.sh: $tool is not on PATH" >&2
        exit 1
    }
done

src=$cache/sdk-src
work=$cache/sdk-work/$arch
mkdir -p "$src" "$work"

fetch() { # name url sha256
    file=$src/$(basename "$2")
    if [ ! -f "$file" ]; then
        curl -fsSL -o "$file.part" "$2"
        mv "$file.part" "$file"
    fi
    echo "$3  $file" | sha256sum -c --quiet - || {
        echo "linux-sdk.sh: $file does not match its pinned hash" >&2
        exit 1
    }
}

fetch sdl3 "$SDL3_URL" "$SDL3_SHA256"
fetch opus "$OPUS_URL" "$OPUS_SHA256"

cc=$work/zigcc
cat >"$cc" <<EOF
#!/bin/sh
exec zig cc -target $arch-linux-gnu.$GLIBC "\$@"
EOF
chmod +x "$cc"
cxx=$work/zigcxx
cat >"$cxx" <<EOF
#!/bin/sh
exec zig c++ -target $arch-linux-gnu.$GLIBC "\$@"
EOF
chmod +x "$cxx"
ar=$work/zigar
printf '#!/bin/sh\nexec zig ar "$@"\n' >"$ar"
chmod +x "$ar"
ranlib=$work/zigranlib
printf '#!/bin/sh\nexec zig ranlib "$@"\n' >"$ranlib"
chmod +x "$ranlib"

build() { # name tarball dir extra-cmake-args...
    name=$1 tarball=$2 dir=$3
    shift 3
    prefix=$cache/sdk/$name/$arch-linux
    rm -rf "$work/$name-src" "$work/$name-build" "$prefix"
    mkdir -p "$work/$name-src"
    tar -xzf "$tarball" -C "$work/$name-src" --strip-components=1
    cmake -S "$work/$name-src" -B "$work/$name-build" -G "${CMAKE_GENERATOR:-Unix Makefiles}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" \
        -DCMAKE_AR="$ar" -DCMAKE_RANLIB="$ranlib" \
        -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR="$arch" \
        -DCMAKE_C_FLAGS="-idirafter /usr/include -idirafter /usr/include/$arch-linux-gnu" \
        -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
        -DBUILD_SHARED_LIBS=ON "$@"
    cmake --build "$work/$name-build" --parallel "$(nproc)"
    cmake --install "$work/$name-build"
}

build sdl3 "$src/SDL3-$SDL3_VERSION.tar.gz" . \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_INSTALL_TESTS=OFF \
    -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF
build opus "$src/opus-$OPUS_VERSION.tar.gz" . \
    -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF -DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF \
    -DOPUS_INSTALL_CMAKE_CONFIG_MODULE=OFF
echo "$GLIBC" >"$cache/sdk/sdl3/$arch-linux/glibc"
rm -rf "$work"
echo "linux-sdk.sh: SDL3 $SDL3_VERSION and opus $OPUS_VERSION for glibc $GLIBC in $cache/sdk"
