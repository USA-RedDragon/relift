#!/usr/bin/env bash
# The workspace tests for hosts that run no other checks (macOS), in three
# parts that together run what `cargo test --workspace` does:
#   workspace  every crate but relift
#   relift     the relift crate without its end-to-end targets
#   e2e        the relift crate's end-to-end targets
. "$(dirname "$0")/lib.sh"

case ${1:-} in
  workspace) cargo test --workspace --exclude relift --locked ;;
  relift) scripts/ci/tests-relift.sh ;;
  e2e) scripts/ci/tests-relift-e2e.sh ;;
  *) echo "usage: tests.sh workspace|relift|e2e" >&2; exit 2 ;;
esac
