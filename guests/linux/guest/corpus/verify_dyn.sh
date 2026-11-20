#!/bin/sh
# Runs every `.dyn` build of OUT_DIR on the shared arm64 test device through
# the sysroot's dynamic loader and compares what it prints and returns with
# the recorded expected output, to show the dynamic builds behave as the
# static ones did. Takes the same lock, device and sysroot variables as
# record.sh, and removes what it pushed.
set -u
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: verify_dyn.sh OUT_DIR}
lock=${RELIFT_ADB_LOCK:-${XDG_RUNTIME_DIR:-$out}/relift-adb.lock}
remote=/data/local/tmp/relift-elf
sysroot=${SYSROOT:-$("${AARCH64_GCC:-aarch64-linux-gnu-gcc}" -print-sysroot 2>/dev/null || true)}
dev() { flock "$lock" adb "$@"; }
quote() { printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"; }
failed=0
for prog in "$out"/*.dyn; do
    name=$(basename "$prog" .dyn)
    dir=$here/$name
    [ -d "$dir/etc" ] && continue
    work=$remote/$name
    script=$out/$name.remote.sh
    {
        echo "cd $work"
        printf 'exec env TMPDIR=%s' "$work"
        if [ -f "$dir/env" ]; then
            while IFS= read -r e; do printf ' %s' "$(quote "$e")"; done < "$dir/env"
        fi
        [ -f "$dir/gconv" ] && printf ' GCONV_PATH=%s/gconv' "$work"
        printf ' ./lib/ld-linux-aarch64.so.1 --library-path %s/lib:%s ./prog' "$work" "$work"
        if [ -f "$dir/args" ]; then
            while IFS= read -r a; do printf ' %s' "$(quote "$a")"; done < "$dir/args"
        fi
        echo ' 2>/dev/null'
    } > "$script"
    dev shell "rm -rf $work; mkdir -p $work/lib"
    for lib in ld-linux-aarch64.so.1 libc.so.6 libm.so.6 libgcc_s.so.1 libstdc++.so.6; do
        dev push -q "$sysroot/lib/$lib" "$work/lib/$lib" > /dev/null
    done
    for so in "$out"/lib"$name".so; do
        [ -f "$so" ] && dev push -q "$so" "$work/$(basename "$so")" > /dev/null
    done
    [ -f "$dir/gconv" ] && dev push -q "$sysroot/lib/gconv" "$work/gconv" > /dev/null
    dev push -q "$prog" "$work/prog" > /dev/null
    dev push -q "$script" "$work/run.sh" > /dev/null
    dev shell "chmod 755 $work/prog"
    flock "$lock" sh -c "adb shell 'sh $work/run.sh > $work/out; echo \$? > $work/code'"
    dev pull -q "$work/out" "$out/$name.dyn.out" > /dev/null
    dev pull -q "$work/code" "$out/$name.dyn.code" > /dev/null
    dev shell "rm -rf $work"
    if cmp -s "$out/$name.dyn.out" "$dir/expected.stdout" && [ "$(tr -d '\r' < "$out/$name.dyn.code")" = "$(cat "$dir/expected.exit")" ]; then
        echo "ok   $name"
    else
        echo "FAIL $name: device output differs from the recorded one"
        failed=1
    fi
done
dev shell "rm -rf $remote"
exit $failed
