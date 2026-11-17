#!/usr/bin/env bash
# The Game Boy devices crate's tests, in the release profile.
. "$(dirname "$0")/lib.sh"

cargo test --manifest-path guests/gb/devices/Cargo.toml --release --locked
