#!/usr/bin/env bash
# Import, build and run the cases of runtime/guests/gb/devtests/ci_cases.toml.
# Then scripts/ci/gdb-gb.sh stops a Game Boy program with gdb, with the same relift.
. "$(dirname "$0")/lib.sh"

target=${CARGO_TARGET_DIR:-target}
cargo build --locked -p relift
python3 -I runtime/guests/gb/devtests/ci_corpus.py --relift "$target/debug/relift" \
  --roms "$CI_WORK/gb-roms" --fetch --work "$CI_WORK/gb-work" --jobs "${CI_JOBS:-4}"
scripts/ci/gdb-gb.sh "$target/debug/relift"
