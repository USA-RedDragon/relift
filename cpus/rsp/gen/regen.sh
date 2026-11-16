#!/bin/sh
set -eu
if [ $# -ne 2 ]; then
    echo "usage: cpus/rsp/gen/regen.sh SAIL_LIB MODEL_SRC" >&2
    exit 2
fi
target=${CARGO_TARGET_DIR:-target}
out=cpus/rsp/src/lift/generated
classes="int ldst branch system vector vmem"
for class in $classes; do
    cp cpus/rsp/gen/placeholder.rs "$out/$class.rs"
done
cargo build --locked -q -p relift-cpu-rsp-gen
for f in decode:cpus/rsp/src/decode/generated.rs $(for c in $classes; do printf '%s ' "$c:$out/$c.rs"; done); do
    "$target/debug/relift-cpu-rsp-gen" gen "$1" "$2" cpus/rsp/gen/rsp.sail "${f%%:*}" > "${f#*:}.new"
    mv "${f#*:}.new" "${f#*:}"
done
cargo fmt -p relift-cpu-rsp
