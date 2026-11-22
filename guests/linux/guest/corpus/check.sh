#!/bin/sh
# Lifts, builds and runs corpus programs with relift and compares what they
# print and return with the recorded expected output.
#
#   check.sh OUT_DIR WORK_DIR [NAME[.VARIANT]...]
#
# OUT_DIR holds the programs build.sh made; WORK_DIR gets one project per
# program. RELIFT names the relift binary. Every program is built and run in
# each mode of RELIFT_CHECK_MODES (default: auto trampoline cold): auto lets the
# build pick direct returns when it proves them safe, trampoline forces
# every return through dispatch (emit.trampoline); cold adds the cold copy
# (emit.cold). The output must be the same in all. Every program needs a
# bit of time to compile, so a name list picks some. A `.dyn` program
# imports with SYSROOT, the arm64 root file system or toolchain sysroot that
# holds its libraries (default: the sysroot of AARCH64_GCC,
# aarch64-linux-gnu-gcc), plus the program's `etc` directory when it has
# one. Exit status 1 when any differs.
set -u
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: check.sh OUT_DIR WORK_DIR [NAME[.VARIANT]...]}
work=${2:?usage: check.sh OUT_DIR WORK_DIR [NAME[.VARIANT]...]}
shift 2
relift=${RELIFT:-relift}
sysroot=${SYSROOT:-$("${AARCH64_GCC:-aarch64-linux-gnu-gcc}" -print-sysroot 2>/dev/null || true)}
mkdir -p "$work"
if [ $# -eq 0 ]; then
    list=$(ls "$out" | grep -E '\.(static|pie|gnu|dyn)$')
else
    list=$*
fi
modes=${RELIFT_CHECK_MODES:-auto trampoline cold}
failed=0
for mode in $modes; do
for b in $list; do
    name=${b%.*}
    dir=$here/$name
    proj=$work/$b
    tag=$b
    forced=false
    cold=false
    case $mode in
    trampoline) forced=true ;;
    cold) cold=true ;;
    esac
    if [ "$mode" != auto ]; then
        proj=$work/$b.$mode
        tag="$b ($mode)"
    fi
    mkdir -p "$proj"
    flags=
    case $b in
    *.dyn)
        sys=$sysroot
        if [ -d "$dir/etc" ]; then
            sys=$work/sysroot-$name
            if [ ! -d "$sys" ]; then
                mkdir -p "$sys"
                cp -a "$sysroot/lib" "$sysroot/lib64" "$sys/"
                cp -a "$dir/etc" "$sys/etc"
            fi
        fi
        flags="--sysroot $sys"
        ;;
    esac
    if ! (cd "$proj" && rm -f relift.toml && "$relift" import $flags "$out/$b" >import.log 2>&1 && RELIFT_EMIT_TRAMPOLINE=$forced RELIFT_EMIT_COLD=$cold "$relift" build ${RELIFT_BUILD_FLAGS:-} >build.log 2>&1); then
        echo "FAIL $tag: build (see $proj/build.log)"
        failed=1
        continue
    fi
    exe=$(sed -n 's/^  output: //p' "$proj/build.log" | tail -n 1)
    set --
    if [ -f "$dir/args" ]; then
        while IFS= read -r a; do set -- "$@" "$a"; done <"$dir/args"
    fi
    envs=
    [ -f "$dir/env" ] && envs=$(cat "$dir/env")
    tmp=$proj/tmp
    rm -rf "$tmp"
    mkdir -p "$tmp"
    (cd "$tmp" && env TMPDIR="$tmp" $envs "$exe" "$@" >"$proj/stdout" 2>"$proj/stderr")
    code=$?
    if cmp -s "$proj/stdout" "$dir/expected.stdout" && [ "$code" = "$(cat "$dir/expected.exit")" ]; then
        echo "ok   $tag"
    else
        echo "FAIL $tag: exit $code, expected $(cat "$dir/expected.exit"); diff $proj/stdout $dir/expected.stdout"
        failed=1
    fi
done
done
exit $failed
