#!/usr/bin/env bash
# The relift crate's end-to-end test targets (every tests/e2e*.rs).
. "$(dirname "$0")/lib.sh"

cargo test -p relift --locked --test 'e2e*'
