#!/usr/bin/env bash
# Run a command in the copied tree on the Mac the way all.sh runs a mac job.
# Usage: mac-run.sh JOB KEY COMMAND...
# The work dir is new for every run. The cargo target dir is the cache of KEY
# under ~/relift-ci/cache, kept between runs like rust-cache on a runner, and
# wiped when it holds more than CI_MAC_CACHE_GB gigabytes (default 5). As on a
# runner, the incremental cache is off, the target's tmp dir is cleared and the
# workspace crates are rebuilt: rust-cache keeps the dependencies only.
set -euo pipefail

job=${1:?usage: mac-run.sh JOB KEY COMMAND...}
key=${2:?usage: mac-run.sh JOB KEY COMMAND...}
shift 2

cache="$HOME/relift-ci/cache/$key"
mkdir -p "$cache"
kb=$(du -sk "$cache" | cut -f1)
if [ "$kb" -gt $((${CI_MAC_CACHE_GB:-5} * 1048576)) ]; then
  echo "cache $cache holds $((kb / 1048576)) GB, over ${CI_MAC_CACHE_GB:-5}: wiped"
  rm -rf "$cache"
  mkdir -p "$cache"
fi
rm -rf "$cache/target/tmp"
if [ -d "$cache/target" ]; then
  while read -r name; do
    find "$cache/target" -maxdepth 4 -path "*/.fingerprint/$name-*" -prune -exec rm -rf {} +
  done < <(cargo metadata --no-deps --format-version 1 --locked | jq -r '.packages[].name')
fi

work="$CI_MAC_WORK/$job"
rm -rf "$work"
mkdir -p "$work"

export CI_WORK=$work CARGO_TARGET_DIR=$cache/target CARGO_BUILD_JOBS=4 CARGO_INCREMENTAL=0
exec "$@"
