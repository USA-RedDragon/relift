#!/bin/sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: build.sh OUT_DIR}
mkdir -p "$out"
target=aarch64-linux-musl
for dir in "$here"/*/; do
    name=$(basename "$dir")
    if [ -f "$dir/main.cpp" ]; then
        cc="zig c++"
        src="$dir/main.cpp"
    else
        cc="zig cc"
        src="$dir/main.c"
    fi
    variants=static
    [ -f "$dir/variants" ] && variants="static $(cat "$dir/variants")"
    for v in $variants; do
        case $v in
        static) flags="-static" ;;
        pie) flags="-static -pie -fPIE" ;;
        *) echo "unknown variant $v" >&2; exit 1 ;;
        esac
        $cc -target $target $flags -O2 -fno-sanitize=undefined -o "$out/$name.$v" "$src" -lm
    done
done
