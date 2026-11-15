#!/bin/sh
set -eu
if [ $# -ne 2 ]; then
    echo "usage: cpus/arm32/gen/regen.sh SAIL_LIB MODEL_SRC" >&2
    exit 2
fi
target=${CARGO_TARGET_DIR:-target}
out=cpus/arm32/src/lift/generated
for class in a32 t16; do
    cp cpus/arm32/gen/placeholder.rs "$out/$class.rs"
done
cargo build --locked -q -p relift-cpu-arm32-gen
for f in decode:cpus/arm32/src/decode/generated.rs a32:$out/a32.rs t16:$out/t16.rs; do
    "$target/debug/relift-cpu-arm32-gen" gen "$1" "$2" cpus/arm32/gen/armv4t.sail "${f%%:*}" > "${f#*:}.new"
    mv "${f#*:}.new" "${f#*:}"
done
cargo fmt -p relift-cpu-arm32
