#!/usr/bin/env bash
# Cross-build the runtime and a Game Boy program for one relift host target,
# check the release, and run it where the host can (windows-x64 under wine).
# For ios-arm64 it also builds SDL3 and libopus for iOS and the program as an
# unsigned app.
# Usage: gbcross.sh TARGET
. "$(dirname "$0")/lib.sh"

target=${1:?usage: gbcross.sh TARGET}
run=()
[ "$target" = windows-x64 ] && run=(--run)
cargo build --locked -p relift
python3 -I runtime/tools/cross/gbcross.py --target "$target" \
  --relift "${CARGO_TARGET_DIR:-target}/debug/relift" --work "$CI_WORK/cross-$target" ${run[@]+"${run[@]}"}

if [ "$target" = ios-arm64 ]; then
  work="$CI_WORK/cross-$target"
  cache="$work/cache"
  relift="${CARGO_TARGET_DIR:-$ROOT/target}/debug/relift"
  command -v ninja >/dev/null || brew install ninja
  python3 -I runtime/tools/cross/ios_deps.py --cache "$cache" --target ios-arm64
  (cd "$work/proj" && RELIFT_CACHE_DIR="$cache" "$relift" build --target ios-arm64)
  app="$cache/projects/proj/build/ios-arm64/proj.app"
  plutil -lint "$app/Info.plist"
  python3 -I runtime/tools/cross/release_guard.py --binary "$app/game"
fi
