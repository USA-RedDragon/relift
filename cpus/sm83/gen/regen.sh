#!/bin/sh
set -eu
if [ $# -ne 1 ]; then
    echo "usage: cpus/sm83/gen/regen.sh SAIL_LIB" >&2
    exit 2
fi
out=cpus/sm83/src/lift/sail/generated.rs
target=${CARGO_TARGET_DIR:-target}
cp cpus/sm83/gen/placeholder.rs "$out"
cargo build --locked -q -p relift-cpu-sm83-gen
"$target/debug/relift-cpu-sm83-gen" gen "$1" cpus/sm83/sail/sm83.sail > "$out.new"
mv "$out.new" "$out"
cargo fmt -p relift-cpu-sm83
