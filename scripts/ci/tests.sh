#!/usr/bin/env bash
# The workspace tests for hosts that run no other checks (macOS), in four
# parts that together run what `cargo test --workspace` does:
#   workspace  every crate but relift
#   relift     the relift crate without its end-to-end targets
#   e2e-slow   the end-to-end targets that take longest
#   e2e-rest   the other end-to-end targets
. "$(dirname "$0")/lib.sh"

case ${1:-} in
  workspace) cargo test --workspace --exclude relift --locked ;;
  relift) scripts/ci/tests-relift.sh ;;
  e2e-slow) scripts/ci/tests-relift-e2e.sh slow ;;
  e2e-rest) scripts/ci/tests-relift-e2e.sh rest ;;
  *) echo "usage: tests.sh workspace|relift|e2e-slow|e2e-rest" >&2; exit 2 ;;
esac
