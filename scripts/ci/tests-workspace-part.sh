#!/usr/bin/env bash
# One part of the workspace tests of every crate but relift, for the hosted
# macOS runners. A crate lands in a part by where its manifest lives:
#   cpus    cpus/: the CPU lifters, generators and their tests
#   switch  gpus/ and guests/switch/: the zstd, font and shader dependencies
#   rest    every other crate
# Every crate is in exactly one part: the others' crates are matched first and
# rest takes the remainder, so a new crate needs no entry here. With no
# argument, or all, every part's crates run in one go.
# Usage: tests-workspace-part.sh [cpus|switch|rest|all]
. "$(dirname "$0")/lib.sh"

part=${1:-all}
case $part in
  cpus | switch | rest | all) ;;
  *) echo "usage: tests-workspace-part.sh [cpus|switch|rest|all]" >&2; exit 2 ;;
esac

# part_of RELATIVE-MANIFEST-DIR: the part the crate there belongs to.
part_of() {
  case $1 in
    cpus/*) echo cpus ;;
    gpus/* | guests/switch/*) echo switch ;;
    *) echo rest ;;
  esac
}

args=()
count=0
while IFS=$'\t' read -r name dir; do
  [ "$name" != relift ] || continue
  count=$((count + 1))
  [ "$part" = all ] || [ "$(part_of "${dir#"$ROOT"/}")" = "$part" ] || continue
  args+=(-p "$name")
done < <(cargo metadata --no-deps --format-version 1 --locked |
  jq -r '.workspace_members as $m | .packages[] | select(.id as $i | $m | index($i)) | [.name, (.manifest_path | rtrimstr("/Cargo.toml"))] | @tsv')
[ ${#args[@]} -gt 0 ] || { echo "no crates in part $part ($count crates in the workspace)" >&2; exit 1; }

cargo test --locked "${args[@]}"
