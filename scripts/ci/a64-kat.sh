#!/usr/bin/env bash
# The A64 known answers recorded from the Thor (cpus/arm64/tests/kat): every
# case on the IR interpreter and as emitted C compiled with clang. No
# hardware is needed; `relift-check kat record` makes the files.
. "$(dirname "$0")/lib.sh"

cargo test --release --locked -p relift-cpu-arm64 --test kat
cargo test --release --locked -p relift --test emit_a64_kat
