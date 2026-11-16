#!/bin/sh
set -eu
if [ $# -ne 1 ]; then
    echo "usage: cpus/z80/gen/regen.sh SAIL_LIB" >&2
    exit 2
fi
out=cpus/z80/src/lift/generated.rs
target=${CARGO_TARGET_DIR:-target}
cp cpus/z80/gen/placeholder.rs "$out"
cargo build --locked -q -p relift-cpu-z80-gen
"$target/debug/relift-cpu-z80-gen" gen "$1" cpus/z80/sail/z80.sail > "$out.new"
mv "$out.new" "$out"
cargo fmt -p relift-cpu-z80
