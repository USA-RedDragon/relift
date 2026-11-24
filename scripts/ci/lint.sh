#!/usr/bin/env bash
# fmt, clippy, charter, the Android app's game data import logic (with javac),
# generated docs and GPU-API forwarding, deny. The tests run in tests-*.sh.
. "$(dirname "$0")/lib.sh"

cargo fmt --all --check
cargo fmt --manifest-path check/Cargo.toml --all --check
cargo fmt --manifest-path guests/gb/devices/Cargo.toml --all --check
cargo fmt --manifest-path gpus/xlate/Cargo.toml --all --check

cargo clippy --workspace --all-targets --all-features --locked -- -D warnings
cargo clippy --manifest-path check/Cargo.toml --all-targets --all-features --locked -- -D warnings
cargo clippy --manifest-path guests/gb/devices/Cargo.toml --all-targets --all-features --locked -- -D warnings
cargo clippy --manifest-path gpus/xlate/Cargo.toml --all-targets --all-features --locked -- -D warnings

cargo test -p relift --locked --test workspace_charter --test unsafe_roster --test funnels

java_work="$CI_WORK/android-java-tests"
rm -rf "$java_work"
mkdir -p "$java_work/classes" "$java_work/tmp"
javac -Xlint:all -Werror -d "$java_work/classes" \
  relift/src/android/java/GameData.java relift/src/android/test/GameDataTest.java
java -Djava.io.tmpdir="$java_work/tmp" -cp "$java_work/classes" dev.relift.app.GameDataTest

version=$(grep -A1 '^name = "configulator-rs"$' Cargo.lock | sed -n 's/^version = "\(.*\)"$/\1/p')
cfg_root="$CI_WORK/configulator-$version"
[ -x "$cfg_root/bin/configulator" ] || cargo install configulator-cli --locked --version "=$version" --root "$cfg_root"
(
  cd relift
  "$cfg_root/bin/configulator" --type Settings --sample --format toml --sample-file ../relift.example.toml --check
)

# Each relift-vkgen configuration against its committed output: the pinned
# registry files (url and sha256 lines, main first, then each extra) are
# fetched and passed in order.
apigen_check() {
  local cfg=$1 out=$2 xml=() url sum
  while read -r url && read -r sum; do
    fetch "$url" "$sum" "$CI_WORK/apigen-$sum.xml"
    xml+=(--xml "$CI_WORK/apigen-$sum.xml")
  done < <(sed -n 's/^\(url\|sha256\) = "\(.*\)"$/\2/p' "$cfg")
  cargo run --locked -q -p relift-gpu-apigen -- "${xml[@]}" --config "$cfg" --out "$out" --check
}
apigen_check gpus/apigen/vkgen.toml runtime/gpuapi/vk/gen
apigen_check gpus/apigen/vk-i386.toml runtime/gpuapi/vk/gen/i386
apigen_check gpus/apigen/gl.toml runtime/gpuapi/gl/gen/i386
apigen_check gpus/apigen/egl.toml runtime/gpuapi/egl/gen/i386

name="cargo-deny-$CARGO_DENY_VERSION-x86_64-unknown-linux-musl"
dir="$CI_WORK/cargo-deny-$CARGO_DENY_VERSION"
fetch "https://github.com/EmbarkStudios/cargo-deny/releases/download/$CARGO_DENY_VERSION/$name.tar.gz" \
  "$CARGO_DENY_SHA256" "$dir/deny.tgz"
[ -x "$dir/$name/cargo-deny" ] || tar -xzf "$dir/deny.tgz" -C "$dir"
"$dir/$name/cargo-deny" check
