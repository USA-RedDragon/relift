#!/usr/bin/env bash
# Remove the cargo build units of a target dir that no build has used for DAYS
# days. cargo touches a unit's .fingerprint/UNIT/invoked.timestamp each time a
# build includes the unit, so that file's age is the unit's last use. A unit's
# files all carry its hash: .fingerprint/NAME-HASH, build/NAME-HASH and
# deps/*NAME-HASH.* (or deps/NAME-HASH for a binary).
# Usage: mac-prune.sh TARGET_DIR DAYS
set -euo pipefail

target=${1:?usage: mac-prune.sh TARGET_DIR DAYS}
days=${2:?usage: mac-prune.sh TARGET_DIR DAYS}

for fp in "$target"/*/.fingerprint; do
  [ -d "$fp" ] || continue
  dir=$(dirname "$fp")
  find "$fp" -mindepth 2 -maxdepth 2 -name invoked.timestamp -mtime "+$days" -print |
    while read -r stamp; do
      unit=$(basename "$(dirname "$stamp")")
      hash=${unit##*-}
      name=${unit%-*}
      rm -rf "$fp/$unit" "$dir/build/$unit"
      find "$dir/deps" -maxdepth 1 \( -name "*$name-$hash.*" -o -name "$name-$hash" \) -exec rm -rf {} +
    done
done
