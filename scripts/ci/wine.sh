#!/usr/bin/env bash
# Install wine64 on an Ubuntu runner so the Windows program can run on Linux,
# without a live mirror download on every run. The .deb files (wine64 and
# everything it pulls in) live in DIR, which the workflow caches under one
# key prefix, newest first. Runners of one image name run several image
# versions at once, so the key is not the image version: a run installs the
# newest cached set with dpkg and no network, and only when there is none, or
# when that set no longer installs on this runner's image, downloads a fresh
# set with short timeouts and says so (downloaded=true in GITHUB_OUTPUT) for
# the workflow to cache it.
# Does nothing where wine is already installed.
# Usage: wine.sh DIR
. "$(dirname "$0")/lib.sh"

dir=${1:?usage: wine.sh DIR}
if command -v wine >/dev/null || command -v wine64 >/dev/null; then
  echo "wine is already installed"
  exit 0
fi

apt=(apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30)
download() {
  rm -rf "$dir"
  mkdir -p "$dir"
  sudo "${apt[@]}" update
  sudo "${apt[@]}" install --download-only --no-install-recommends -y -o Dir::Cache::archives="$dir" wine64
  sudo chown -R "$(id -u):$(id -g)" "$dir"
  rm -rf "$dir/partial" "$dir/lock"
  [ -z "${GITHUB_OUTPUT:-}" ] || echo "downloaded=true" >> "$GITHUB_OUTPUT"
}

install() {
  sudo env DEBIAN_FRONTEND=noninteractive dpkg -i "$dir"/*.deb
}

# undo: remove the packages of the set that dpkg left unpacked but not
# configured, so apt can download a fresh set.
undo() {
  local f p left=()
  for f in "$dir"/*.deb; do
    p=$(dpkg-deb -f "$f" Package)
    [ "$(dpkg-query -W -f='${Status}' "$p" 2>/dev/null)" = "install ok installed" ] || left+=("$p")
  done
  [ ${#left[@]} -eq 0 ] || sudo dpkg --remove --force-depends "${left[@]}" || true
}

if compgen -G "$dir/*.deb" >/dev/null; then
  echo "installing wine from the cached packages in $dir"
  if install; then
    exit 0
  fi
  echo "the cached packages do not install on this image: downloading a fresh set"
  undo
else
  echo "no cached wine packages in $dir: downloading them"
fi
download
install
