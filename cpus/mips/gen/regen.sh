#!/bin/sh
set -eu
if [ $# -ne 2 ]; then
    echo "usage: cpus/mips/gen/regen.sh SAIL_LIB MODEL_SRC" >&2
    exit 2
fi
target=${CARGO_TARGET_DIR:-target}
out=cpus/mips/src/lift/generated
classes="int ldst branch system cop1"
for class in $classes; do
    cp cpus/mips/gen/placeholder.rs "$out/$class.rs"
done
cargo build --locked -q -p relift-cpu-mips-gen
for f in decode:cpus/mips/src/decode/generated.rs $(for c in $classes; do printf '%s ' "$c:$out/$c.rs"; done); do
    "$target/debug/relift-cpu-mips-gen" gen "$1" "$2" cpus/mips/gen/vr4300.sail "${f%%:*}" > "${f#*:}.new"
    mv "${f#*:}.new" "${f#*:}"
done
cargo fmt -p relift-cpu-mips
