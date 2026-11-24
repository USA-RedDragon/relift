#!/bin/sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: build.sh OUT_DIR}
mkdir -p "$out"
target=aarch64-linux-musl
for dir in "$here"/*/; do
    [ -n "${ONLY_DYNAMIC:-}" ] && break
    name=$(basename "$dir")
    if [ -f "$dir/main.go" ]; then
        command -v go >/dev/null || continue
        (cd "$dir" && GOOS=linux GOARCH=arm64 CGO_ENABLED=0 GO111MODULE=off go build -trimpath -o "$out/$name.static" main.go)
        continue
    fi
    [ -f "$dir/dynamic_only" ] && continue
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
    gxx="${gcc%gcc}g++"
    for dir in "$here"/*/; do
        name=$(basename "$dir")
        [ -f "$dir/main.go" ] && continue
        [ -f "$dir/i386_only" ] && continue
        if [ -f "$dir/main.cpp" ]; then
            cc=$gxx
            src="$dir/main.cpp"
            libsrc="$dir/lib.cpp"
        else
            cc=$gcc
            src="$dir/main.c"
            libsrc="$dir/lib.c"
        fi
        flags="-O2 -I$dir"
        if [ -f "$libsrc" ]; then
            $cc $flags -shared -fPIC -o "$out/lib$name.so" "$libsrc"
            if [ -f "$dir/dlopen" ]; then
                link="-ldl"
            else
                link="-L$out -l$name"
            fi
            $cc $flags -o "$out/$name.dyn" "$src" $link "-Wl,-rpath,\$ORIGIN" -lm
        else
            $cc $flags -o "$out/$name.dyn" "$src" -lm
        fi
        [ -f "$dir/dynamic_only" ] && continue
        if [ -f "$dir/main.cpp" ]; then
            $gxx -static -O2 -o "$out/$name.gnu" "$dir/main.cpp"
        else
            $gcc -static -O2 -o "$out/$name.gnu" "$dir/main.c" -lm
        fi
    done
fi
