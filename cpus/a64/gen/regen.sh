#!/bin/sh
set -eu
if [ $# -ne 2 ]; then
    echo "usage: cpus/a64/gen/regen.sh SAIL_LIB MODEL_SRC" >&2
    exit 2
fi
out=cpus/a64/src/lift/sail/generated
target=${CARGO_TARGET_DIR:-target}
for class in int ldst excl branch system simd fp; do
    cp cpus/a64/gen/placeholder.rs "$out/$class.rs"
done
cargo build --locked -q -p relift-cpu-a64-gen
for class in int ldst excl branch system simd fp; do
    "$target/debug/relift-cpu-a64-gen" gen "$1" "$2" cpus/a64/gen/forms.txt "$class" > "$out/$class.rs.new"
    mv "$out/$class.rs.new" "$out/$class.rs"
done
cargo fmt -p relift-cpu-a64
