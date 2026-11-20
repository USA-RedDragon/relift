#!/bin/sh
# Lifts, builds and runs corpus programs with relift and compares what they
# print and return with the recorded expected output.
#
#   check.sh OUT_DIR WORK_DIR [NAME[.VARIANT]...]
#
# OUT_DIR holds the programs build.sh made; WORK_DIR gets one project per
# program. RELIFT names the relift binary. Every program needs a bit of
# time to compile, so a name list picks some. Exit status 1 when any differs.
set -u
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: check.sh OUT_DIR WORK_DIR [NAME[.VARIANT]...]}
work=${2:?usage: check.sh OUT_DIR WORK_DIR [NAME[.VARIANT]...]}
shift 2
relift=${RELIFT:-relift}
mkdir -p "$work"
if [ $# -eq 0 ]; then
    list=$(ls "$out" | grep -E '\.(static|pie)$')
else
    list=$*
fi
failed=0
for b in $list; do
    name=${b%.*}
    dir=$here/$name
    proj=$work/$b
    mkdir -p "$proj"
    if ! (cd "$proj" && rm -f game.toml && "$relift" import "$out/$b" >import.log 2>&1 && "$relift" build ${RELIFT_BUILD_FLAGS:-} >build.log 2>&1); then
        echo "FAIL $b: build (see $proj/build.log)"
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
        echo "ok   $b"
    else
        echo "FAIL $b: exit $code, expected $(cat "$dir/expected.exit"); diff $proj/stdout $dir/expected.stdout"
        failed=1
    fi
done
exit $failed
