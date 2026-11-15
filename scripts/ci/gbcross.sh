#!/usr/bin/env bash
# Cross-build the runtime and a Game Boy program for one relift host target,
# check the release, and run it where the host can (windows-x64 under wine).
# Usage: gbcross.sh TARGET
. "$(dirname "$0")/lib.sh"

target=${1:?usage: gbcross.sh TARGET}
run=()
[ "$target" = windows-x64 ] && run=(--run)
cargo build --locked -p relift
python3 -I runtime/tools/cross/gbcross.py --target "$target" \
  --relift "${CARGO_TARGET_DIR:-target}/debug/relift" --work "$CI_WORK/cross-$target" "${run[@]}"
