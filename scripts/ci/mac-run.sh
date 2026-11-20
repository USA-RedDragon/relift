#!/usr/bin/env bash
# Run a command in the copied tree on the Mac the way all.sh runs a mac job.
# Usage: mac-run.sh JOB KIND COMMAND...
# KIND names what the job builds: debug (the host target, dev and test
# profiles) or release-TRIPLE. The cargo target dir is the cache of KIND, the
# host triple, the rustc version and RUSTFLAGS under ~/relift-ci/cache, so jobs
# that build the same thing share one dir, kept between runs like rust-cache
# on a runner. As on a runner, the incremental cache is off and the workspace
# crates are rebuilt: rust-cache keeps the dependencies only.
# The first job of a run (CI_MAC_RUN, set by all.sh) to reach a cache dir
# prepares it under a lock: units unused for CI_MAC_CACHE_DAYS days (default 4)
# are pruned, and while the cache holds more than CI_MAC_CACHE_GB gigabytes in
# all (default 8), the least recently used dirs that no job of this run has
# prepared are removed. This dir is wiped if it alone is over that.
set -euo pipefail

job=${1:?usage: mac-run.sh JOB KIND COMMAND...}
kind=${2:?usage: mac-run.sh JOB KIND COMMAND...}
shift 2

host=$(rustc -vV | sed -n 's/^host: //p')
version=$(rustc -vV | sed -n 's/^release: //p')
key="$kind-$host-$version"
flags=${RUSTFLAGS-}${CARGO_ENCODED_RUSTFLAGS-}
[ -z "$flags" ] || key="$key-$(printf %s "$flags" | cksum | cut -d' ' -f1)"

root="$HOME/relift-ci/cache"
cache="$root/$key"
run=${CI_MAC_RUN:-$$}
budget_kb=$((${CI_MAC_CACHE_GB:-8} * 1048576))
mkdir -p "$root"

# mkdir is the lock: the Mac has no flock.
lock="$cache.lock"
until mkdir "$lock" 2>/dev/null; do
  [ -z "$(find "$lock" -maxdepth 0 -mmin +10)" ] || rm -rf "$lock"
  sleep 1
done
trap 'rmdir "$lock"' EXIT
mkdir -p "$cache"
if [ "$(cat "$cache/.run" 2>/dev/null)" != "$run" ]; then
  "$(dirname "$0")/mac-prune.sh" "$cache/target" "${CI_MAC_CACHE_DAYS:-4}"
  # Dirs of the per-job scheme (mac-*) are never used again.
  rm -rf "$root"/mac-*
  total() { du -sk "$root" 2>/dev/null | cut -f1; }
  # Evict the dirs no job of this run has prepared yet, least recently used
  # first: a dir this run has prepared may be in use.
  while [ "$(total)" -gt "$budget_kb" ]; do
    victim=$(cd "$root" && for d in */; do
      d=${d%/}
      [ -f "$d/.run" ] && [ "$(cat "$d/.run")" != "$run" ] && [ ! -d "$d.lock" ] &&
        echo "$(stat -f %m "$d/.run") $d"
    done | sort -n | head -n 1 | cut -d' ' -f2)
    [ -n "$victim" ] || break
    echo "cache holds $(($(total) / 1048576)) GB, over ${CI_MAC_CACHE_GB:-8}: removed $victim"
    rm -rf "${root:?}/$victim"
  done
  if [ "$(du -sk "$cache" | cut -f1)" -gt "$budget_kb" ]; then
    echo "cache $cache holds more than ${CI_MAC_CACHE_GB:-8} GB: wiped"
    rm -rf "$cache"
    mkdir -p "$cache"
  fi
  rm -rf "$cache/target/tmp"
  if [ -d "$cache/target" ]; then
    while read -r name; do
      find "$cache/target" -maxdepth 4 -path "*/.fingerprint/$name-*" -prune -exec rm -rf {} +
    done < <(cargo metadata --no-deps --format-version 1 --locked | jq -r '.packages[].name')
  fi
  printf %s "$run" >"$cache/.run"
fi
rmdir "$lock"
trap - EXIT

work="$CI_MAC_WORK/$job"
rm -rf "$work"
mkdir -p "$work"

export CI_WORK=$work CARGO_TARGET_DIR=$cache/target CARGO_BUILD_JOBS=4 CARGO_INCREMENTAL=0
exec "$@"
