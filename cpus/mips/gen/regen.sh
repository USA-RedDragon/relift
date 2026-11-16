#!/bin/sh
set -eu
if [ $# -ne 2 ]; then
    echo "usage: cpus/mips/gen/regen.sh SAIL_LIB MODEL_SRC" >&2
    exit 2
fi
target=${CARGO_TARGET_DIR:-target}
profiles="vr4300 r3000a"
classes() {
    case $1 in
    vr4300) echo "int ldst branch system cop1" ;;
    r3000a) echo "int" ;;
    esac
}
for p in $profiles; do
    for class in $(classes "$p"); do
        if [ "$p" = vr4300 ]; then
            cp cpus/mips/gen/placeholder.rs "cpus/mips/src/$p/lift/generated/$class.rs"
        else
            sed "s/crate::/crate::$p::/" cpus/mips/gen/placeholder.rs > "cpus/mips/src/$p/lift/generated/$class.rs"
        fi
    done
done
cargo build --locked -q -p relift-cpu-mips-gen
for p in $profiles; do
    out=cpus/mips/src/$p/lift/generated
    for f in "decode:cpus/mips/src/$p/decode/generated.rs" $(for c in $(classes "$p"); do printf '%s ' "$c:$out/$c.rs"; done); do
        "$target/debug/relift-cpu-mips-gen" gen "$1" "$2" "cpus/mips/gen/$p.sail" "${f%%:*}" > "${f#*:}.new"
        mv "${f#*:}.new" "${f#*:}"
    done
done
cargo fmt -p relift-cpu-mips
