#!/usr/bin/env bash
# fmt, clippy, tests, charter, generated docs and Vulkan forwarding, deny.
. "$(dirname "$0")/lib.sh"

cargo fmt --all --check
cargo fmt --manifest-path check/Cargo.toml --all --check
cargo fmt --manifest-path guests/gb/devices/Cargo.toml --all --check

cargo clippy --workspace --all-targets --all-features --locked -- -D warnings
cargo clippy --manifest-path check/Cargo.toml --all-targets --all-features --locked -- -D warnings
cargo clippy --manifest-path guests/gb/devices/Cargo.toml --all-targets --all-features --locked -- -D warnings

cargo test -p relift --locked --test workspace_charter --test unsafe_roster --test funnels
cargo test --workspace --locked
cargo test --manifest-path guests/gb/devices/Cargo.toml --release --locked

version=$(grep -A1 '^name = "configulator-rs"$' Cargo.lock | sed -n 's/^version = "\(.*\)"$/\1/p')
cfg_root="$CI_WORK/configulator-$version"
[ -x "$cfg_root/bin/configulator" ] || cargo install configulator-cli --locked --version "=$version" --root "$cfg_root"
(
  cd relift
  "$cfg_root/bin/configulator" --type Settings --sample --format toml --sample-file ../relift.example.toml --check
  "$cfg_root/bin/configulator" --type Settings --markdown --markdown-file ../README.md --env-prefix RELIFT_ --env-separator _ --check
)

cfg=guests/switch/vkgen/vkgen.toml
url=$(sed -n 's/^url = "\(.*\)"$/\1/p' "$cfg")
sum=$(sed -n 's/^sha256 = "\(.*\)"$/\1/p' "$cfg")
fetch "$url" "$sum" "$CI_WORK/vk-$sum.xml"
cargo run --locked -q -p relift-switch-vkgen -- --xml "$CI_WORK/vk-$sum.xml" --config "$cfg" --out runtime/guests/switch/vk/gen --check

name="cargo-deny-$CARGO_DENY_VERSION-x86_64-unknown-linux-musl"
dir="$CI_WORK/cargo-deny-$CARGO_DENY_VERSION"
fetch "https://github.com/EmbarkStudios/cargo-deny/releases/download/$CARGO_DENY_VERSION/$name.tar.gz" \
  "$CARGO_DENY_SHA256" "$dir/deny.tgz"
[ -x "$dir/$name/cargo-deny" ] || tar -xzf "$dir/deny.tgz" -C "$dir"
"$dir/$name/cargo-deny" check
