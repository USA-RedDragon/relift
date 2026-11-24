#!/usr/bin/env bash
# fmt, clippy, charter, the Android app's game data import logic (with javac),
# generated docs and Vulkan forwarding, deny. The tests run in tests-*.sh.
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

cfg=gpus/apigen/vkgen.toml
url=$(sed -n 's/^url = "\(.*\)"$/\1/p' "$cfg")
sum=$(sed -n 's/^sha256 = "\(.*\)"$/\1/p' "$cfg")
fetch "$url" "$sum" "$CI_WORK/vk-$sum.xml"
cargo run --locked -q -p relift-gpu-apigen -- --xml "$CI_WORK/vk-$sum.xml" --config "$cfg" --out runtime/guests/switch/vk/gen --check

name="cargo-deny-$CARGO_DENY_VERSION-x86_64-unknown-linux-musl"
dir="$CI_WORK/cargo-deny-$CARGO_DENY_VERSION"
fetch "https://github.com/EmbarkStudios/cargo-deny/releases/download/$CARGO_DENY_VERSION/$name.tar.gz" \
  "$CARGO_DENY_SHA256" "$dir/deny.tgz"
[ -x "$dir/$name/cargo-deny" ] || tar -xzf "$dir/deny.tgz" -C "$dir"
"$dir/$name/cargo-deny" check
