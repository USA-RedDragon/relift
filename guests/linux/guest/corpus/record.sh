#!/bin/sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: record.sh OUT_DIR}
lock=${RELIFT_ADB_LOCK:-${XDG_RUNTIME_DIR:-$out}/relift-adb.lock}
remote=/data/local/tmp/relift-elf
dev() { flock "$lock" adb "$@"; }
quote() { printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"; }
for dir in "$here"/*/; do
    name=$(basename "$dir")
    work=$remote/$name
    script=$out/$name.remote.sh
    {
        echo "cd $work"
        printf 'exec env TMPDIR=%s' "$work"
        if [ -f "$dir/env" ]; then
            while IFS= read -r e; do printf ' %s' "$(quote "$e")"; done < "$dir/env"
        fi
        printf ' ./prog'
        if [ -f "$dir/args" ]; then
            while IFS= read -r a; do printf ' %s' "$(quote "$a")"; done < "$dir/args"
        fi
        echo ' 2>/dev/null'
    } > "$script"
    dev shell "rm -rf $work; mkdir -p $work"
    dev push -q "$out/$name.static" "$work/prog" > /dev/null
    dev push -q "$script" "$work/run.sh" > /dev/null
    dev shell "chmod 755 $work/prog"
    flock "$lock" sh -c "adb shell 'sh $work/run.sh > $work/out; echo \$? > $work/code'"
    dev pull -q "$work/out" "$dir/expected.stdout" > /dev/null
    dev pull -q "$work/code" "$out/$name.code" > /dev/null
    tr -d '\r' < "$out/$name.code" > "$dir/expected.exit"
    dev shell "rm -rf $work"
done
dev shell "rm -rf $remote"
