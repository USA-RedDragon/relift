# relift

relift is a static recompiler under the MIT license. It translates a game's
machine code ahead of time into portable C11, which clang or `zig cc` builds
into a native program. The program links against a C runtime that stands in
for the original console: its operating system, its devices, or both. The
core is host-agnostic: it names no console and no CPU. The tool is written
in Rust.

**Status: early development.** Nothing is ready for players.

relift ships no game code, keys or data, and never will.

## Guests

| Guest | CPU | State |
|---|---|---|
| Nintendo Switch | ARM64 | Test programs built with libnx run end to end, and CI runs synthetic ones. A commercial game reaches gameplay in test runs; most games are untested and many will fail. |
| Game Boy and Game Boy Color | SM83 | Test ROMs from public suites pass in CI. Retail games run in test runs. |
| Game Boy Advance | ARMv4T | Public test ROMs pass and match the NanoBoyAdvance oracle in test runs. Retail games run in test runs; code that discovery misses stops the program with a report. |

In development, with no runnable guest yet: the Nintendo 64 CPU (VR4300,
`cpus/mips`) and a Switch 2 shader translator (SM86 to SPIR-V,
`gpus/sm86`).

## Hosts

| Host | State |
|---|---|
| Linux x86-64 | All three guests run; CI runs the tests |
| Linux arm64 | relift builds in CI |
| macOS arm64 | All three guests run on an Apple M2; CI runs the tests |
| macOS x86-64 | Game Boy and GBA programs build in CI; a GBA game ran under Rosetta |
| Windows x86-64 | Game Boy and GBA programs build in CI and run under wine; the runtime tests and the hot-load prototype ran on Windows 11 |
| Windows arm64 | Game Boy and GBA programs build in CI; not run |
| Android arm64 | All three guests run on an Android handheld as APKs; Game Boy and GBA programs build in CI |
| iOS arm64 | All three guests run on an iPhone; Game Boy and GBA apps build in CI |

## Requirements

- Decrypted inputs that you dumped yourself from hardware you own: for the
  Switch a `.nup` or an extracted `exefs`/`romfs` tree, otherwise a
  cartridge ROM. Encrypted dumps are refused.
- The Rust toolchain pinned in `rust-toolchain.toml`.
- clang (or `zig cc`) and ninja.
- Optional: SDL3 for a window, SDL3 and libopus for sound, a Vulkan loader
  (MoltenVK on macOS) for Switch games that render. Without SDL3 the program
  runs headless. Switch games' VP9 movies need the pinned libvpx source
  (`runtime/tools/libvpx_src.py`). Phone builds need the Android SDK and NDK,
  or Xcode.

## Quick start

```sh
cargo build --release -p relift -p relift-switch-tools -p relift-gb-tools -p relift-gba-tools
export PATH="$PWD/target/release:$PATH"
mkdir mygame && cd mygame
relift import ~/dumps/game.gbc   # or a .gba ROM, a Switch .nup or dump folder
relift build      # a shipped, packaged build
relift run        # a dev build, run on this host
```

## Tools

- `relift`: `import`, `build`, `run`, `push` (copy game data to a phone),
  `report` (explain a stop report), `info`, `coverage` and `diff`.
- `relift-check`: the differential checker (`check/`).
- `relift-nup`, `relift-romfs`, `relift-nso`, `relift-nro`, `relift-npdm`,
  `relift-ir`: Switch file formats and the A64 IR.
- `relift-vkgen`: generates the Switch Vulkan forwarding C.
- `relift-gbrom`, `relift-gbarom`: Game Boy and GBA ROM headers and
  function discovery.

## Documentation

| Document | Holds |
|---|---|
| `docs/usage.md` | projects, commands, missed code and stop reports |
| `docs/packaging.md` | what `relift build` makes for each host, titles and icons |
| `docs/configuration.md` | relift's own settings |
| `guests/switch/README.md`, `guests/gb/README.md`, `guests/gba/README.md` | importing and running each guest's games |
| `runtime/README.md` | the C runtime: layers, build, tests, run-time settings, environment |
| `docs/abi.md` | the contract between emitted C and the runtime |
| `docs/input.md` | input ports, controls and bindings |
| `ARCHITECTURE.md` | crate graph, rules and the tests that enforce them, CI |
| `core/README.md`, `cpus/*/README.md`, `gpus/sm86/README.md`, `check/README.md` | one page per folder |

## Developing

```sh
pre-commit install --hook-type pre-commit --hook-type pre-push
```

Before contributing, read `ARCHITECTURE.md`: section 0 lists the hard rules.
`scripts/ci/all.sh` runs every CI job this machine can run.

## License

MIT. See `LICENSE`.
