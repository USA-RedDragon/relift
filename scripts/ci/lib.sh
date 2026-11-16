# Shared by every scripts/ci job. Sourced, not run.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

CI_WORK=${CI_WORK:-${RUNNER_TEMP:-${XDG_CACHE_HOME:-$HOME/.cache}/relift-ci}}
mkdir -p "$CI_WORK"
export RELIFT_REPORT_DIR=${RELIFT_REPORT_DIR:-$CI_WORK/reports}

ACTIONLINT_VERSION=1.7.12
ACTIONLINT_SHA256=8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8
CARGO_DENY_VERSION=0.20.2
CARGO_DENY_SHA256=9f12ed4c49936e09b48bf862b595cde2fe64fcbd9d74dfacac6131ca824c8d5f
APPIMAGE_RUNTIME_TAG=20251108
APPIMAGETOOL_VERSION=1.9.1
APPIMAGETOOL_SHA256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0

# fetch URL SHA256 OUT: download once into the work dir and check the hash.
fetch() {
  local url=$1 sum=$2 out=$3
  if [ ! -f "$out" ] || ! echo "$sum  $out" | sha256sum -c - >/dev/null 2>&1; then
    mkdir -p "$(dirname "$out")"
    curl -fsSL -o "$out.part" "$url"
    echo "$sum  $out.part" | sha256sum -c -
    mv "$out.part" "$out"
  fi
}
