#!/usr/bin/env bash
# Every workspace test except the relift crate's (tests-relift.sh and
# tests-relift-e2e.sh run those). tests-gb-devices.sh runs the Game Boy devices
# crate's.
. "$(dirname "$0")/lib.sh"

# The SPIR-V the GPU translator writes is checked by the pinned spirv-val,
# never skipped.
spirv_val=$(spirv_val_dir)
PATH="$spirv_val:$PATH" RELIFT_SPIRV_VAL_REQUIRE=1 cargo test --workspace --exclude relift --locked
