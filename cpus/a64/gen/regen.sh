#!/bin/sh
set -eu
if [ $# -ne 2 ]; then
    echo "usage: cpus/a64/gen/regen.sh SAIL_LIB MODEL_SRC" >&2
    exit 2
fi
out=cpus/a64/src/lift/sail/generated
target=${CARGO_TARGET_DIR:-target}
classes="int ldst excl branch system simd_ldst_multi simd_ldst_multi_post simd_ldst_single simd_same simd_same_ext simd_shift simd_shift_narrow simd_shift_scalar simd_misc simd_elem simd_elem_sat simd_other fp"
for class in $classes; do
    cp cpus/a64/gen/placeholder.rs "$out/$class.rs"
done
cargo build --locked -q -p relift-cpu-a64-gen
if [ "$("$target/debug/relift-cpu-a64-gen" classes | tr '\n' ' ')" != "$classes " ]; then
    echo "regen.sh: the class list differs from the generator's" >&2
    exit 1
fi
for class in $classes; do
    "$target/debug/relift-cpu-a64-gen" gen "$1" "$2" cpus/a64/gen/forms.txt "$class" > "$out/$class.rs.new"
    mv "$out/$class.rs.new" "$out/$class.rs"
done
cargo fmt -p relift-cpu-a64
