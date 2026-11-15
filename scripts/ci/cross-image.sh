#!/usr/bin/env bash
# Build relift for one Rust target inside the pinned rust-cross image.
# Usage: cross-image.sh TARGET
. "$(dirname "$0")/lib.sh"

CROSS_IMAGE=ghcr.io/usa-reddragon/rust-cross:1.98.1@sha256:a4d9cb8c3d4a4134c36dec1df78f1b0cc760d7fe443240dd5303ee64a01996c2
TARGET=${1:?usage: cross-image.sh TARGET}
export TARGET

docker pull -q "$CROSS_IMAGE" >/dev/null
docker run --rm -v "$ROOT:/project" -w /project -e TARGET "$CROSS_IMAGE" bash -euo pipefail -c '
  pinned=$(sed -n "s/^channel = \"\(.*\)\"$/\1/p" rust-toolchain.toml)
  test "${RUST_VERSION:?the image must set RUST_VERSION}" = "$pinned" \
    || { echo "error: the image carries Rust $RUST_VERSION but rust-toolchain.toml pins $pinned"; exit 1; }
  export RUSTUP_TOOLCHAIN="$RUST_VERSION"
  rustup target list --installed | grep -qx "$TARGET" \
    || { echo "error: the image no longer ships $TARGET std"; exit 1; }
  CARGO_TARGET_DIR=target/cross cargo build --locked --release -p relift --target "$TARGET"
  ls -l target/cross/"$TARGET"/release/relift*
'
