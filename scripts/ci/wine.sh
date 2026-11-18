#!/usr/bin/env bash
# Install wine64 on an Ubuntu runner so the Windows program can run on Linux,
# without a live mirror download on every run. The .deb files (wine64 and
# everything it pulls in) live in DIR, which the workflow caches by runner
# image version: a run that finds them installs with dpkg and no network, a run that does not
# downloads them once with short timeouts, so a stalled mirror makes apt
# retry or move to another mirror instead of hanging the job.
# Does nothing where wine is already installed.
# Usage: wine.sh DIR
. "$(dirname "$0")/lib.sh"

dir=${1:?usage: wine.sh DIR}
if command -v wine >/dev/null || command -v wine64 >/dev/null; then
  echo "wine is already installed"
  exit 0
fi

apt=(sudo apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30)
if ! compgen -G "$dir/*.deb" >/dev/null; then
  echo "no cached wine packages in $dir: downloading them"
  mkdir -p "$dir"
  "${apt[@]}" update
  "${apt[@]}" install --download-only --no-install-recommends -y -o Dir::Cache::archives="$dir" wine64
  sudo chown -R "$(id -u):$(id -g)" "$dir"
  rm -rf "$dir/partial" "$dir/lock"
else
  echo "installing wine from the cached packages in $dir"
fi
sudo env DEBIAN_FRONTEND=noninteractive dpkg -i "$dir"/*.deb
