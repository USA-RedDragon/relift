#!/bin/sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: record.sh OUT_DIR [NAME...]}
shift
lock=${RELIFT_ADB_LOCK:-${XDG_RUNTIME_DIR:-$out}/relift-adb.lock}
remote=/data/local/tmp/relift-elf
sysroot=${SYSROOT:-$("${AARCH64_GCC:-aarch64-linux-gnu-gcc}" -print-sysroot 2>/dev/null || true)}
dev() { flock "$lock" adb "$@"; }
quote() { printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"; }
if [ $# -eq 0 ]; then
    set -- $(for d in "$here"/*/; do basename "$d"; done)
fi
for name in "$@"; do
    dir=$here/$name
    dynamic=
    [ -f "$dir/dynamic_only" ] && dynamic=1
    [ -n "$dynamic" ] || [ -f "$out/$name.static" ] || continue
    work=$remote/$name
    script=$out/$name.remote.sh
    {
        echo "cd $work"
        printf 'exec env TMPDIR=%s' "$work"
        if [ -f "$dir/env" ]; then
            while IFS= read -r e; do printf ' %s' "$(quote "$e")"; done < "$dir/env"
        fi
        [ -f "$dir/gconv" ] && printf ' GCONV_PATH=%s/gconv' "$work"
        if [ -d "$dir/etc" ]; then
            printf ' chroot %s/root /prog' "$work"
        elif [ -n "$dynamic" ]; then
            printf ' ./lib/ld-linux-aarch64.so.1 --library-path %s/lib:%s ./prog' "$work" "$work"
        else
            printf ' ./prog'
        fi
        if [ -f "$dir/args" ]; then
            while IFS= read -r a; do printf ' %s' "$(quote "$a")"; done < "$dir/args"
        fi
        echo ' 2>/dev/null'
    } > "$script"
    dev shell "rm -rf $work; mkdir -p $work"
    if [ -d "$dir/etc" ]; then
        dev shell "mkdir -p $work/root/lib $work/root/etc"
        for lib in ld-linux-aarch64.so.1 libc.so.6 libm.so.6 libgcc_s.so.1 libstdc++.so.6 libnss_files.so.2 libnss_dns.so.2; do
            dev push -q "$sysroot/lib/$lib" "$work/root/lib/$lib" > /dev/null
        done
        dev push -q "$dir/etc/." "$work/root/etc" > /dev/null
        dev push -q "$out/$name.dyn" "$work/root/prog" > /dev/null
        dev shell "chmod 755 $work/root/prog"
    elif [ -n "$dynamic" ]; then
        dev shell "mkdir -p $work/lib"
        for lib in ld-linux-aarch64.so.1 libc.so.6 libm.so.6 libgcc_s.so.1 libstdc++.so.6; do
            dev push -q "$sysroot/lib/$lib" "$work/lib/$lib" > /dev/null
        done
        for so in "$out"/lib"$name".so; do
            [ -f "$so" ] && dev push -q "$so" "$work/$(basename "$so")" > /dev/null
        done
        [ -f "$dir/gconv" ] && dev push -q "$sysroot/lib/gconv" "$work/gconv" > /dev/null
        dev push -q "$out/$name.dyn" "$work/prog" > /dev/null
    else
        dev push -q "$out/$name.static" "$work/prog" > /dev/null
    fi
    dev push -q "$script" "$work/run.sh" > /dev/null
    dev shell "chmod 755 $work/prog 2>/dev/null; true"
    if [ -d "$dir/etc" ]; then
        dev shell "su -c 'sh $work/run.sh > $work/out; echo \$? > $work/code'"
    else
        dev shell "sh $work/run.sh > $work/out; echo \$? > $work/code"
    fi
    dev pull -q "$work/out" "$dir/expected.stdout" > /dev/null
    dev pull -q "$work/code" "$out/$name.code" > /dev/null
    tr -d '\r' < "$out/$name.code" > "$dir/expected.exit"
    dev shell "rm -rf $work"
done
dev shell "rm -rf $remote"
