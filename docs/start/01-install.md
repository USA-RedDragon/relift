# Install

This step builds relift and its tools from the repository, so that you can run the commands of the next steps. relift is in early development and has no release builds yet.

## What you need

- The Rust toolchain that the repository pins, which `rustup` installs when you build in a clone:

<!-- docgen:code file=rust-toolchain.toml -->
```toml
[toolchain]
channel = "1.98.1"
components = ["clippy", "rustfmt"]
```
<!-- /docgen:code -->

- clang or `zig cc`, to build the C that relift writes, and ninja.
- Optional: SDL3 for a window, SDL3 and libopus for sound, and a Vulkan loader (MoltenVK on macOS) for Switch games that render. Without SDL3 a program runs headless. Switch games' VP9 movies need the pinned libvpx source (`runtime/tools/libvpx_src.py`) and Switch 2 games' AV1 movies the pinned dav1d source (`runtime/tools/dav1d_src.py`). Phone builds need the Android SDK and NDK, or Xcode.
- A decrypted dump of a game that you own, to use in [the next step](02-first-game.md): for the Switch a `.nup` or an extracted `exefs` and `romfs` tree, otherwise a cartridge ROM (`.gb`, `.gbc` or `.gba`). relift refuses encrypted dumps and ships no game data.

## Build the tools

```sh
git clone https://github.com/USA-RedDragon/relift.git
cd relift
cargo build --release -p relift -p relift-switch-tools -p relift-gb-tools -p relift-gba-tools
export PATH="$PWD/target/release:$PATH"
```

The `relift` command is the one that you use. The other binaries read single file formats and ROM headers; [Use relift](../guides/usage.md) names each of them.

## Check the setup

```sh
relift --help
```

Next: [Make your first game](02-first-game.md).
