#!/bin/sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: build.sh OUT_DIR}
mkdir -p "$out"
target=aarch64-linux-musl
for dir in "$here"/*/; do
    name=$(basename "$dir")
    if [ -f "$dir/main.go" ]; then
        command -v go >/dev/null || continue
        (cd "$dir" && GOOS=linux GOARCH=arm64 CGO_ENABLED=0 GO111MODULE=off go build -trimpath -o "$out/$name.static" main.go)
        continue
    fi
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

gcc=${AARCH64_GCC:-}
if [ -n "$gcc" ]; then
    for dir in "$here"/*/; do
        name=$(basename "$dir")
        [ -f "$dir/main.go" ] && continue
        if [ -f "$dir/main.cpp" ]; then
            "${gcc%gcc}g++" -static -O2 -o "$out/$name.gnu" "$dir/main.cpp"
        else
            "$gcc" -static -O2 -o "$out/$name.gnu" "$dir/main.c" -lm
        fi
    done
fi
