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
SPIRV_TOOLS_VERSION=2026.4
SPIRV_TOOLS_SHA256=92df4c377603f086c3d2a1eb2bd9b8f883a73ca890fb939d78a0cecdcc01686c
# The SPIRV-Headers commit SPIRV-Tools pins in its DEPS file for that release.
SPIRV_HEADERS_COMMIT=496543121ce6419f23d6fa5d7194ba66c36212d2
SPIRV_HEADERS_SHA256=a9bb9c48713245eacf97cc539b6f1d45405a92d8813f8b82e635f0a085ee9898

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

# spirv_val_dir: build the pinned spirv-val once into the work dir and
# print the directory that holds it.
spirv_val_dir() {
  local dir="$CI_WORK/spirv-tools-$SPIRV_TOOLS_VERSION"
  if [ ! -x "$dir/build/tools/spirv-val" ]; then
    fetch "https://github.com/KhronosGroup/SPIRV-Tools/archive/refs/tags/v$SPIRV_TOOLS_VERSION.tar.gz" \
      "$SPIRV_TOOLS_SHA256" "$dir/tools.tgz" >&2
    fetch "https://github.com/KhronosGroup/SPIRV-Headers/archive/$SPIRV_HEADERS_COMMIT.tar.gz" \
      "$SPIRV_HEADERS_SHA256" "$dir/headers.tgz" >&2
    rm -rf "$dir/src" "$dir/headers" "$dir/build"
    mkdir -p "$dir/src" "$dir/headers"
    tar -xzf "$dir/tools.tgz" -C "$dir/src" --strip-components=1
    tar -xzf "$dir/headers.tgz" -C "$dir/headers" --strip-components=1
    local gen=()
    command -v ninja >/dev/null && gen=(-G Ninja)
    cmake -S "$dir/src" -B "$dir/build" "${gen[@]}" -DCMAKE_BUILD_TYPE=Release \
      -DSPIRV-Headers_SOURCE_DIR="$dir/headers" -DSPIRV_SKIP_TESTS=ON -DSPIRV_WERROR=OFF >/dev/null
    cmake --build "$dir/build" --target spirv-val -j "$(nproc 2>/dev/null || sysctl -n hw.ncpu)" >/dev/null
  fi
  echo "$dir/build/tools"
}
