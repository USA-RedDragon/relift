#!/usr/bin/env bash
# The workspace tests for hosts that run no other checks (macOS), in parts
# that together run what `cargo test --workspace` does:
#   workspace  every crate but relift, in three parts (workspace cpus,
#              workspace switch, workspace rest) because one job over all of
#              them, cold, built the dependencies for 9 minutes and ran the
#              tests for 5 on a hosted three-core runner
#   relift     the relift crate without its end-to-end targets
#   e2e-slow   the end-to-end targets that take longest
#   e2e-rest   the other end-to-end targets, the Linux corpus among them (on
#              macOS its dynamic programs are skipped, so it stays small)
. "$(dirname "$0")/lib.sh"

case ${1:-} in
  workspace) scripts/ci/tests-workspace-part.sh "${2:-}" ;;
  relift) scripts/ci/tests-relift.sh ;;
  e2e-slow) scripts/ci/tests-relift-e2e.sh slow ;;
  e2e-rest) scripts/ci/tests-relift-e2e.sh rest && scripts/ci/tests-relift-e2e.sh linux ;;
  *) echo "usage: tests.sh workspace cpus|workspace switch|workspace rest|relift|e2e-slow|e2e-rest" >&2; exit 2 ;;
esac
