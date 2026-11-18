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
| Nintendo Switch 2 | ARM64 | Shaders are translated from SM86 to SPIR-V at build time (`gpus/sm86`), and the NVN2 graphics API runs over Vulkan. A retail game reaches its menus in test runs; most games stop early in their boot. |
| Game Boy and Game Boy Color | SM83 | Test ROMs from public suites pass in CI. Retail games run in test runs. |
| Game Boy Advance | ARMv4T | Public test ROMs pass and match the NanoBoyAdvance oracle in test runs. Retail games run in test runs; code that discovery misses stops the program with a report. |
| Nintendo 64 | VR4300 and RSP | Public test ROMs for the CPU, the RSP and VI match the ares oracle in test runs. A retail game reaches gameplay in test runs and matches ares frame for frame through its intro. |
| Nintendo DS | ARM946E-S and ARM7TDMI | relift imports and builds decrypted `.nds` dumps. A retail game runs through its boot and into its intro in test runs, compared frame by frame with the melonDS oracle. |
| Master System, Game Gear and SG-1000 | Z80 | relift imports, builds and runs cartridges and BIOS images, compared with the ares oracle. Code discovery is not written yet: a build takes its code from an ares trace. |

## Hosts

| Host | State |
|---|---|
| Linux x86-64 | Programs of every guest run; CI runs the tests |
| Linux arm64 | relift builds in CI |
| macOS arm64 | Switch, Game Boy and GBA games run on an Apple M2; CI runs the tests |
| macOS x86-64 | Game Boy, GBA, N64 and Master System programs build in CI; a GBA game ran under Rosetta |
| Windows x86-64 | Game Boy, GBA, N64 and Master System programs build in CI and run under wine; the runtime tests and the hot-load prototype ran on Windows 11 |
| Windows arm64 | Game Boy, GBA, N64 and Master System programs build in CI; not run |
| Android arm64 | Switch, Game Boy, GBA and N64 games run on an Android handheld as APKs; Game Boy, GBA, N64 and Master System programs build in CI |
| iOS arm64 | Switch, Game Boy and GBA games run on an iPhone; Game Boy, GBA, N64 and Master System apps build in CI |

## Get started

**Have a game and want to recompile it? Read [Recompile a game](docs/guides/workflow.md).** It answers whether your console is supported, what file to bring, what each command prints, and what to do when the program stops.

Build the tools, import a dump that you own and run it:

```sh
cargo build --release -p relift -p relift-switch-tools -p relift-gb-tools -p relift-gba-tools
export PATH="$PWD/target/release:$PATH"
mkdir mygame && cd mygame
relift import ~/dumps/game.gbc   # or a .gba ROM, a Switch .nup or dump folder
relift build      # a shipped, packaged build
relift run        # a dev build, run on this host
```

[Install](docs/start/01-install.md) lists what you need, and [Make your first game](docs/start/02-first-game.md) goes through the commands.

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

The docs are built as a site from these pages, in the order of [the site outline](docs/nav.md).

| Page | Holds |
|---|---|
| [Get started](docs/start/01-install.md) | install, and your first game |
| [Recompile a game](docs/guides/workflow.md) | the end-to-end answer: support, inputs, real output, stops, reports, packaging, when things go wrong |
| [Use relift](docs/guides/usage.md) | projects, commands, missed code and stop reports |
| [Package a game for each host](docs/guides/packaging.md) | what `relift build` makes for each host, titles and icons |
| [Settings](docs/reference/configuration.md) | relift's own settings |
| [Switch](guests/switch/README.md), [Game Boy](guests/gb/README.md), [Game Boy Advance](guests/gba/README.md) | importing and running each guest's games |
| [The C runtime](runtime/README.md) | layers, build, tests, run-time settings, environment |
| [The IR](docs/reference/ir.md) | blocks, operations, device extensions, text and binary formats, checks |
| [Runtime ABI contract](docs/abi.md) | the contract between emitted C and the runtime |
| [Input ports and bindings](docs/reference/input.md) | input ports, controls and bindings |
| [Architecture](docs/concepts/architecture.md) | crate graph, rules and the tests that enforce them, CI |
| [core](core/README.md), [cpus](cpus/arm64/README.md), [gpus/sm86](gpus/sm86/README.md), [check](check/README.md) | one page per folder |

## Developing

Before contributing, read [Contributing](CONTRIBUTING.md) and [Architecture](docs/concepts/architecture.md): section 0 lists the hard rules. [Open work](GAPS.md) lists what to pick up. [Tooling and checks](docs/project/tooling.md) covers the hooks, `scripts/ci/all.sh` and the docs build. AI coding agents read `AGENTS.md`.

## License

MIT. See `LICENSE`.
