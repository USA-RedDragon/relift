#!/usr/bin/env bash
# The workspace tests alone, for hosts that run no other checks (macOS).
. "$(dirname "$0")/lib.sh"

cargo test --workspace --locked
