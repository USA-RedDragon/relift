#!/usr/bin/env bash
# The relift crate's tests that tests-relift-e2e.sh does not run: its library,
# binaries, doc tests and every test target not named e2e*, so a new target
# lands in exactly one of the two.
. "$(dirname "$0")/lib.sh"

args=(--lib --bins)
while read -r name; do
  case $name in e2e*) ;; *) args+=(--test "$name") ;; esac
done < <(cargo metadata --no-deps --format-version 1 --locked |
  jq -r '.packages[] | select(.name == "relift") | .targets[] | select(.kind | index("test")) | .name')
cargo test -p relift --locked "${args[@]}"
cargo test -p relift --locked --doc
