#!/bin/sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: run.sh OUT_DIR NAME VARIANT}
name=$2
variant=$3
dir="$here/$name"
set --
if [ -f "$dir/args" ]; then
    while IFS= read -r a; do set -- "$@" "$a"; done < "$dir/args"
fi
envs=
if [ -f "$dir/env" ]; then
    envs=$(cat "$dir/env")
fi
exec env $envs qemu-aarch64 "$out/$name.$variant" "$@"
