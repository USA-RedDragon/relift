#!/bin/sh
set -eu
if [ $# -ne 2 ]; then
    echo "usage: cpus/arm32/gen/regen.sh SAIL_LIB MODEL_SRC" >&2
    exit 2
fi
target=${CARGO_TARGET_DIR:-target}
out=cpus/arm32/src/lift/generated
for class in a32 t16 a32_v5te t16_v5te; do
    cp cpus/arm32/gen/placeholder.rs "$out/$class.rs"
done
cargo build --locked -q -p relift-cpu-arm32-gen
g=cpus/arm32/gen
for f in "$g/armv4t.sail,$g/armv5te.sail:decode:cpus/arm32/src/decode/generated.rs" \
    "$g/armv4t.sail:a32:$out/a32.rs" "$g/armv4t.sail:t16:$out/t16.rs" \
    "$g/armv5te.sail:a32:$out/a32_v5te.rs" "$g/armv5te.sail:t16:$out/t16_v5te.rs"; do
    overlay=${f%%:*}
    rest=${f#*:}
    "$target/debug/relift-cpu-arm32-gen" gen "$1" "$2" "$overlay" "${rest%%:*}" > "${rest#*:}.new"
    mv "${rest#*:}.new" "${rest#*:}"
done
cargo fmt -p relift-cpu-arm32
