#!/usr/bin/env bash
# The on-device shader translator (gpus/xlate): builds the static
# library, links the C runner tests/c/rlx_run.c against it through rlx.h only,
# and checks that the tables and modules it makes for the SM86 KAT programs on
# every captured device are byte for byte what relift-gpu-table makes in Rust.
. "$(dirname "$0")/lib.sh"

crate=gpus/xlate
work=${CI_WORK:-$PWD/target/ci-work}/gpu-xlate
target=${CARGO_TARGET_DIR:-$PWD/target}/gpu-xlate
rm -rf "$work"
mkdir -p "$work"

cargo build --manifest-path $crate/Cargo.toml --release --locked --target-dir "$target"
cargo run --manifest-path $crate/Cargo.toml --release --locked --target-dir "$target" -q --example kat -- "$work/kat"
${CC:-clang} -O2 -std=c11 -Wall -Wextra -Werror -I$crate/include $crate/tests/c/rlx_run.c \
  "$target/release/librelift_gpu_xlate.a" -lpthread -ldl -lm -o "$work/rlx_run"

checked=0
for dev in "$work"/kat/*/; do
  dev=${dev%/}
  [ -d "$dev/rust" ] || continue
  "$work/rlx_run" tables "$work/kat/programs" "$dev/device.json" "$dev/c" 3
  cmp -s "$dev/rust/table.rlspv" "$dev/c/table.rlspv" || { echo "table.rlspv differs for $dev"; exit 1; }
  for f in table.rlspv table.rlspi table.stamp table.device failed.txt; do
    cmp "$dev/rust/$f" "$dev/c/$f"
  done
  for spv in "$dev"/one/*.spv; do
    key=$(basename "$spv" .spv)
    "$work/rlx_run" one "$dev/device.json" "$work/kat/programs/programs/$key.bin" "$dev/c-$key.spv" >/dev/null
    cmp "$spv" "$dev/c-$key.spv"
    checked=$((checked + 1))
  done
  "$work/rlx_run" stop "$work/kat/programs" "$dev/device.json" "$dev/stopped"
  [ ! -e "$dev/stopped/table.rlspv" ] || { echo "a stopped run wrote a table for $dev"; exit 1; }
done
[ "$checked" -gt 0 ] || { echo "no module was compared"; exit 1; }
echo "gpu-xlate: $checked modules and every device's tables match the Rust translator"
