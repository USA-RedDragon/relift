#!/usr/bin/env bash
# Make room on a hosted runner's root disk for a job that needs NEED_GIB GiB.
# With that much already free it does nothing. Otherwise it deletes the
# preinstalled toolchains no job here uses, all at once (the time of the
# slowest tree, not of their sum), and only when that is still short prunes
# the preinstalled docker images.
# Usage: free-disk.sh NEED_GIB
need=${1:?usage: free-disk.sh NEED_GIB}
set -euo pipefail

avail() { df --output=avail -B1G / | tail -n 1 | tr -dc '0-9'; }

df -h /
if [ "$(avail)" -ge "$need" ]; then
  echo "$(avail) GiB are free, $need are needed"
  exit 0
fi

printf '%s\0' /usr/share/dotnet /usr/local/lib/android /opt/ghc /usr/local/.ghcup \
  /opt/hostedtoolcache/CodeQL /usr/local/share/boost /usr/share/swift |
  sudo xargs -0 -n 1 -P 7 rm -rf
df -h /
if [ "$(avail)" -lt "$need" ]; then
  sudo docker image prune --all --force
  df -h /
fi
[ "$(avail)" -ge "$need" ] || { echo "error: only $(avail) GiB are free, $need are needed" >&2; exit 1; }
