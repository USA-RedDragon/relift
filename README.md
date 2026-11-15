# relift

relift is a static recompiler under the MIT license. It translates a game's
machine code ahead of time into portable C11, which clang or `zig cc` builds
into a native program. The program links against a C runtime that stands in
for the original console: its operating system, its devices, or both. The
core is host-agnostic: it names no console and no CPU.

The tool is written in Rust. Each guest adds a CPU frontend under `cpus/`, a
guest layer under `guests/` and its runtime part under `runtime/guests/`.

**Status: early development.** Nothing is ready for players. The table lists
what runs and what does not.

| Guest | CPU frontend | State |
|---|---|---|
| Nintendo Switch | ARM64 (`cpus/a64`, generated from Arm's Sail model) | Test programs built with libnx run end to end. Commercial Switch games have reached gameplay on Linux x86-64 in test runs; most games are untested and many will fail. Includes Vulkan forwarding, a core scheduler with preemption points, hot-load, code and data mods, input and settings. |
| Game Boy and Game Boy Color | SM83 (`cpus/sm83`) | `relift import`, `build` and `run` work on ROMs. Test ROMs from public corpora pass in CI. Retail Game Boy Color games reach their title screens in test runs. |
| Game Boy Advance | ARMv4T (`cpus/arm32`) | Pieces only, no `relift import` yet. The ARMv4T lifter, the machine core (bus timing, interrupts, timers, DMA, display, picture unit), the high-level BIOS, the cartridge saves and the keypad are built and tested against console records and the NanoBoyAdvance oracle. Whole-ROM discovery and the CPU runtime layer are planned. |

relift ships no game code, keys or data, and never will.

## Requirements

- Decrypted inputs that you dumped yourself from a console you own. For the
  Switch: a `.nup` or an extracted `exefs`/`romfs` tree. For the Game Boy:
  a `.gb` or `.gbc` cartridge ROM. Encrypted dumps are refused: convert them
  with a separate tool first.
- The Rust toolchain pinned in `rust-toolchain.toml`. A Game Boy build also
  uses cargo to build the device library (rustyboi-core) into the cache.
- clang (or `zig cc`) and ninja.
- Optional: SDL3 for a window (`pkg-config sdl3`, or `RELIFT_SDL3_DIR`), a
  Vulkan loader for Switch games that render (MoltenVK on macOS, linked
  directly: Homebrew's `molten-vk` or `RELIFT_MOLTENVK_DIR`), and SDL3 plus libopus for sound
  (`runtime/README.md`). Without SDL3 the program runs headless.

## Quick start

Build the tools once:

```sh
cargo build --release -p relift -p relift-switch-tools -p relift-gb-tools -p relift-gba-tools
```

Run `relift` in an empty project folder. Each project holds one `game.toml`;
artifacts go to the cache directory. The cache is relocatable: copy or
hard-link it to another path, or move the project, and the next build reuses
every lifted file and object (Ninja runs nothing). Build commands name files
by paths relative to the build directory, the runtime sources are copied into
the cache, and debug info records `.` as its compilation directory, so the
program is byte-identical. What stays absolute: a cartridge ROM's path (the
save sits beside it) and SDL3 library search paths outside the cache.

### Nintendo Switch

```sh
relift import game.nup                 # a NUP container
relift import dump/                    # an extracted tree
relift import dump/ --update 65536     # a chosen update
relift import dump/ --update base      # the base alone
relift build                           # a shipped build
relift run                             # a dev build with hot-loading, then run it
```

The extracted tree uses the layout the reference NUP packer
([github.com/a55uka/nup](https://github.com/a55uka/nup), "Packing a dump")
reads:

| Directory | Holds |
|---|---|
| `base/exefs/`, `base/romfs/` | the base program |
| `updates/<decimal version>/exefs/`, `.../romfs/` | one update per version; its romfs is the full patched tree, not a delta |
| `dlc/<16-hex add-on title id>/<index>/romfs/` | one add-on per index (`<index>` is an opaque directory name) |

A directory holding `exefs/` and/or `romfs/` directly is taken as a base.
The newest update (the highest version) is used unless `--update` names
another; its exefs and romfs replace the base's when it has them. The same
choice applies to the update packages of a `.nup`. The program ID comes from
`main.npdm`. Import also writes `control.nacp` and `icon.jpg` when the dump
has them.

`relift build --mods <list>` builds code mods in: Atmosphere or Ryujinx mod
directories, `.ips` files and `.pchtxt` files, applied in order. Two mods that
write different bytes at one address are refused. Data mods and cheats are
chosen at run time (`RELIFT_MODS`, `runtime/README.md`).

### Game Boy and Game Boy Color

```sh
relift import some-game.gbc
relift build
relift run
```

`relift import` takes a cartridge ROM as it is and writes `game.toml` with
`guest = "gb"`. `relift build --mods hack.ips` applies IPS, UPS or BPS
ROM-hack patches in order before discovery. `relift run` opens a window (SDL3)
and runs until you close it. `relift build` prints the path of the built
program; call it directly for a headless run, with the options below.

The built program takes `--headless`, `--model dmg|mgb|sgb|cgb|agb|...`,
`--frames N` (stop after N frames and print the frame hash and the serial
output), `--input` (a scripted joypad, `frame:buttons,...`), `--png FILE`,
`--png-dir DIR --png-every N`, `--wav FILE` (record the sound, 44100 Hz
stereo, and print its length, peak and hash), `--no-audio`, `--case FILE`
(grade a test ROM by a corpus case spec, sound cases included), `--save FILE`,
`--no-save`, `--stats` and `--check` (lockstep against the device library's
own CPU). `--help` lists them. In a window the sound goes to the host output,
and the frames are paced on that output's queue. The battery save is a raw
`.sav` next to the imported ROM, the format other emulators read.

The devices come from rustyboi-core (`guests/gb/devices`), driven at every
instruction boundary; lifted code runs the instruction bodies. Code with no
lifted function (code copied to RAM, or missed by discovery) runs on the
device library's CPU one instruction at a time until it returns or reaches
lifted code. CI builds and runs 38 corpus cases through this path
(`runtime/guests/gb/devtests/ci_cases.toml`).

### Game Boy Advance

Import is not available yet. `relift-gbarom` reads and checks a ROM header and
lists the save library strings and the multiboot entry of an image that fits
work RAM. `relift-gbarom functions` lists the functions of a ROM across ARM
and Thumb code, the code it copies to work RAM and the transfers it cannot
resolve; `--multiboot` reads a multiboot image.

## Building and running

```sh
relift build      # a shipped build: the runtime linked in, no code loaded at run time
relift run        # a dev build, then run it with hot-loading on
```

Discovery cannot see every code address (a pointer the program computes, a
jump table it does not recognize). A shipped build stops at such a miss
with a report and exit status 70. Under `relift run` the program asks relift
for the missed code instead: relift lifts it and everything newly reachable
from it, compiles one shared library, the program loads it and goes on.
relift adds each such address to the `[hints]` list in `game.toml`, and the
next `relift build` lifts it with the rest (docs/abi.md section 6).

A built program prints its own description with `--relift-info`: identity,
run-time settings, input ports and bindings, mods and save kind. `relift info`
prints the same document without running the program. A launcher reads it to
build its settings and input screens (docs/input.md).

### Linux: AppImage

On Linux `relift build` also writes `<project>.AppImage` beside the program:
one executable that runs on a mainstream distribution with nothing installed
but glibc and the system's graphics stack. It holds the program, the shared
libraries it needs that a distribution cannot be relied on to have (SDL3), a
desktop file and icon (the game's own, else a generated one), the
self-description (`relift info <game>.AppImage` reads it without running the
game) and the license of everything bundled
(`usr/share/licenses/<game>/BUNDLED.txt`). The Vulkan loader, GPU drivers,
the C library and the display and audio servers are always the system's. A
Switch game's romfs and DLC trees go inside; saves, settings and the SD card
go to `$XDG_DATA_HOME/<game>` (default `~/.local/share/<game>`), never into
the AppImage.

It needs `appimagetool` on `PATH` (or `toolchain.appimagetool`); without it
the build prints how to get it and still finishes. `package.appimage` is
`auto` (the default), `always` (a missing tool fails the build) or `never`.
`relift run` and the dev build are unchanged.

To make the AppImage run on distributions older than this machine, run
`runtime/tools/linux-sdk.sh` once. It builds SDL3 for glibc 2.28 into the
cache; `relift build` then compiles with `zig cc` for that glibc and links
that SDL3, and the AppImage asks glibc 2.28 or newer. Without it the program
is built with the host compiler and asks the host's glibc (the build summary
prints it).

## Tools

| Binary | Guest | Does |
|---|---|---|
| `relift` | all | `import`, `build` and `run` for the Switch and the Game Boy; `info` prints a built binary's self-description; `coverage` lists what discovery could not resolve; `diff` compares one lifted function with an interpreter in a live run; `check` and `where` are listed but not implemented |
| `relift-check` | all | the differential checker in `check/`, a separate workspace: lifted A64 instructions against Unicorn, QEMU or an Apple M2; reference runs on rustyboi-core (Game Boy) and on NanoBoyAdvance (Game Boy Advance) |
| `relift-nup` | Switch | list and extract `.nup` containers |
| `relift-romfs` | Switch | list, extract and print files of a romfs image |
| `relift-nso` | Switch | show, list symbols of and decompress NSO modules |
| `relift-nro` | Switch | show NRO modules and extract their assets |
| `relift-npdm` | Switch | show a `main.npdm` |
| `relift-ir` | Switch | print the IR of A64 instruction words or NSO symbols |
| `relift-vkgen` | Switch | generate the Vulkan forwarding C from `vk.xml` (package `relift-switch-vkgen`, not in the build line above) |
| `relift-gbrom` | Game Boy | show a ROM header and validate its checksums; `functions` lists functions per bank, unresolved transfers and code run from RAM |
| `relift-gbarom` | Game Boy Advance | show a ROM header, validate logo hash, complement and size, list save library strings; `functions` lists ARM and Thumb functions |

`relift coverage` (Switch projects; `--json` for a machine-readable form, `--module <name>`
for one module) lists, per module, the functions found, the instructions that
do not lift, each indirect call or computed jump left to the dispatcher with
its reason (`slot`, `table` or `pointer`), and the hint addresses earlier dev
runs recorded in `game.toml`.

`relift diff <function>` (Switch projects) builds a dev build, runs it under gdb and, at each
call of the function, runs the same function in the reference interpreter from
the same registers and live guest memory. It compares the registers and every
page the interpreter touched (`--count`, `--skip`, `--engine`). It needs gdb
and exits 1 on a mismatch and 2 when no call was compared.

## Platforms

| Host | State |
|---|---|
| Linux x86-64 | built and tested, including Switch and Game Boy runs |
| macOS arm64 | built and tested (CI and an Apple M2); only the windowless parts of the runtime ran with SDL3 |
| Windows x86-64 | the runtime tests, fibers, crash handler and hot-load prototype ran on Windows 11; the rest is untested |
| Linux arm64, Windows arm64, macOS x86-64 | `relift` cross-builds in CI; not run |

## Developing

```sh
pre-commit install --hook-type pre-commit --hook-type pre-push
```

The toolchain is pinned in `rust-toolchain.toml`. Before contributing, read
`ARCHITECTURE.md`: section 0 lists the hard rules, and the rest covers how the
workspace is laid out and which tests hold it in shape.

| Document | Holds |
|---|---|
| `ARCHITECTURE.md` | crate graph, rules and the tests that enforce them, CI |
| `docs/abi.md` | the contract between emitted C and the runtime |
| `docs/input.md` | input ports, controls and bindings |
| `runtime/README.md` | the C runtime: layers, build, tests, settings, environment |
| `runtime/guests/switch/vk/README.md` | Vulkan forwarding |
| `core/README.md`, `cpus/*/README.md`, `guests/*/README.md`, `check/README.md` | one page per folder; the generator folders (`cpus/*/gen`) and `check/oracle` have their own |

## Configuration

relift reads its own settings from `relift.toml` in the working directory (or
the file given with `-c`), then `RELIFT_*` environment variables, then flags.
Per-game data lives in each game project's `game.toml`, not here.
`relift.example.toml` lists every key.

<!-- configulator:begin -->

| Key                      | Type    | Default | Environment                     | Flag                       | Description                                                                                                                   |
|--------------------------|---------|---------|---------------------------------|----------------------------|-------------------------------------------------------------------------------------------------------------------------------|
| `cache-dir`              | string  |         | `RELIFT_CACHE_DIR`              | `--cache-dir`              | where lifted, emitted and built artifacts are cached (empty: the user cache dir)                                              |
| `jobs`                   | integer | `0`     | `RELIFT_JOBS`                   | `-j`, `--jobs`             | parallel compile jobs (0: one per CPU)                                                                                        |
| `runtime-dir`            | string  |         | `RELIFT_RUNTIME_DIR`            | `--runtime-dir`            | the C runtime sources to build against (empty: the ones beside this relift's source)                                          |
| `color`                  | string  | `auto`  | `RELIFT_COLOR`                  | `--color`                  | color in output: auto (a terminal that shows it, and NO_COLOR unset), always or never                                         |
| `toolchain.clang`        | string  |         | `RELIFT_TOOLCHAIN_CLANG`        | `--toolchain.clang`        | the clang driver                                                                                                              |
| `toolchain.zig`          | string  |         | `RELIFT_TOOLCHAIN_ZIG`          | `--toolchain.zig`          | zig, for `zig cc` player builds                                                                                               |
| `toolchain.ninja`        | string  |         | `RELIFT_TOOLCHAIN_NINJA`        | `--toolchain.ninja`        | the ninja build tool                                                                                                          |
| `toolchain.appimagetool` | string  |         | `RELIFT_TOOLCHAIN_APPIMAGETOOL` | `--toolchain.appimagetool` | appimagetool, which packs the Linux release output                                                                            |
| `emit.shard-kib`         | integer | `4096`  | `RELIFT_EMIT_SHARD_KIB`         | `--emit.shard-kib`         | the most KiB of C per emitted file; clang needs about 120 bytes of memory per byte of C                                       |
| `emit.piece-kib`         | integer | `1024`  | `RELIFT_EMIT_PIECE_KIB`         | `--emit.piece-kib`         | the most KiB of C per C function; a larger guest function is split into several                                               |
| `package.appimage`       | string  | `auto`  | `RELIFT_PACKAGE_APPIMAGE`       | `--package.appimage`       | the Linux release output: auto (an AppImage when appimagetool is found), always (a build without appimagetool fails) or never |

<!-- configulator:end -->

## License

MIT. See `LICENSE`.
