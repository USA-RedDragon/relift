#!/usr/bin/env bash
# Install Ubuntu packages on a hosted runner with their .deb files kept in
# APT_DEBS (default ~/.cache/apt-debs), so that a CI cache of that folder
# saves the download when an Ubuntu mirror is slow; apt fetches only the
# packages that the folder does not hold at the version it picks.
# Usage: apt-install.sh PACKAGE...
set -euo pipefail
debs="${APT_DEBS:-$HOME/.cache/apt-debs}"
mkdir -p "$debs/partial"
sudo apt-get update
sudo apt-get install --no-install-recommends -y -o "Dir::Cache::archives=$debs" "$@"
sudo chown -R "$(id -u):$(id -g)" "$debs"
