#!/usr/bin/env bash
# Build relift for one Rust target inside the pinned rust-cross image, in
# target/cross/TARGET (one folder per target, so local parallel runs do not
# wait on each other), and give the files back to the calling user.
# Usage: cross-image.sh TARGET
. "$(dirname "$0")/lib.sh"

CROSS_IMAGE=ghcr.io/usa-reddragon/rust-cross:1.98.1@sha256:a4d9cb8c3d4a4134c36dec1df78f1b0cc760d7fe443240dd5303ee64a01996c2
TARGET=${1:?usage: cross-image.sh TARGET}
OWNER="$(id -u):$(id -g)"
CARGO_BUILD_JOBS=${CARGO_BUILD_JOBS:-$(nproc)}
export TARGET OWNER CARGO_BUILD_JOBS

mkdir -p "$ROOT/target/cross/$TARGET"
docker pull -q "$CROSS_IMAGE" >/dev/null
docker run --rm -v "$ROOT:/project" -w /project -e TARGET -e OWNER -e CARGO_BUILD_JOBS "$CROSS_IMAGE" bash -euo pipefail -c '
  pinned=$(sed -n "s/^channel = \"\(.*\)\"$/\1/p" rust-toolchain.toml)
  test "${RUST_VERSION:?the image must set RUST_VERSION}" = "$pinned" \
    || { echo "error: the image carries Rust $RUST_VERSION but rust-toolchain.toml pins $pinned"; exit 1; }
  export RUSTUP_TOOLCHAIN="$RUST_VERSION"
  rustup target list --installed | grep -qx "$TARGET" \
    || { echo "error: the image no longer ships $TARGET std"; exit 1; }
  trap "chown -R \"\$OWNER\" target/cross" EXIT
  CARGO_TARGET_DIR=target/cross/$TARGET cargo build --locked --release -p relift --target "$TARGET"
  ls -l target/cross/"$TARGET"/"$TARGET"/release/relift*
'
