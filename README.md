# relift

relift is a static recompiler. It translates a game's machine code ahead of
time into portable C11, which clang or `zig cc` builds into a native program
for Linux, macOS, Windows and other hosts. The program links against a C
runtime that stands in for the original console: its operating system, its
devices, or both.

The tool is written in Rust. The core (IR, analysis, C emitter, build driver,
runtime core) knows no particular console or CPU. Each guest adds a CPU
frontend under `cpus/` and a guest layer under `guests/` and
`runtime/guests/`.

| Guest | CPU frontend | State |
|---|---|---|
| Nintendo Switch | ARM64 (`cpus/a64`, generated from Arm's Sail model) | libnx test programs build and run end to end on Linux x86-64; commercial games are not running yet |
| Game Boy and Game Boy Color | SM83 (`cpus/sm83`) | test ROMs import, build and run as native programs on Linux, graded by the corpus manifest; commercial games boot (see below) |

**Status: early development.** Nothing is ready for players.

relift ships no game code, keys or data, and never will. You supply your own
legally obtained dumps.

## Importing a game

### Nintendo Switch

relift reads decrypted dumps only. An encrypted dump must first be converted
to one of the inputs below with a separate tool.

```sh
relift import game.nup                 # a NUP container
relift import dump/                    # an extracted tree
relift import dump/ --update 65536     # a chosen update
relift import dump/ --update base      # the base alone
```

The extracted tree uses the same layout the reference NUP packer
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
`main.npdm`.

`relift coverage` (`--json` for a machine-readable form, `--module <name>`
for one module) lists what discovery could not resolve: per module, the
functions found, the instructions that do not lift, each indirect call or
computed jump left to the dispatcher with its reason (`slot`, `table` or
`pointer`), and the hint addresses earlier dev runs recorded in `game.toml`.

### Game Boy and Game Boy Color

```sh
relift import "Some Game (USA).gbc"
relift build
```

`relift import` takes a cartridge ROM (`.gb`, `.gbc`) as it is and writes
`game.toml` with `guest = "gb"`. `relift build --mods hack.ips` applies IPS,
UPS or BPS ROM-hack patches in order before discovery. The program runs in a
window (SDL3) or with `--headless`; its battery save is a raw `.sav` next to
the imported ROM, the format other emulators read. Options of the program:
`--model dmg|mgb|sgb|cgb|agb|...`, `--frames N` (stop after N frames and
print the frame hash and the serial output), `--input` (a scripted joypad,
`frame:buttons,...`), `--png FILE`, `--png-dir DIR --png-every N`,
`--case FILE` (grade a test ROM by a corpus case spec), `--stats` and
`--check` (lockstep against the device library's own CPU).

The devices come from rustyboi-core (`guests/gb/devices`), driven at every
instruction boundary; lifted code runs the instruction bodies. Code with no
lifted function (code copied to RAM, or missed by discovery) runs on the
device library's CPU one instruction at a time until it returns or reaches
lifted code.

### Game Boy Advance

Import is not available yet. `relift-gbarom` reads and checks a ROM header and lists the save library strings; multiboot images are refused.

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

## Tools

| Binary | Guest | Does |
|---|---|---|
| `relift` | all | the recompiler: `import`, `build`, `run`, `coverage`, `check`, `where`; Game Boy ROMs: `import`, `build`, `run` |
| `relift-nup` | Switch | list and extract `.nup` containers |
| `relift-romfs` | Switch | list, extract and print files of a romfs image |
| `relift-nso` | Switch | show, list symbols of and decompress NSO modules |
| `relift-nro` | Switch | show NRO modules and extract their assets |
| `relift-npdm` | Switch | show a `main.npdm` |
| `relift-ir` | Switch | print the IR of A64 instruction words or NSO symbols |
| `relift-gbrom` | Game Boy | show a ROM header and validate its checksums; `functions` lists functions per bank, unresolved transfers and code run from RAM |
| `relift-gbarom` | Game Boy Advance | show a ROM header, validate logo hash, complement and size, list save library strings |

## Building

```sh
cargo build --release -p relift -p relift-switch-tools -p relift-gb-tools -p relift-gba-tools
pre-commit install --hook-type pre-commit --hook-type pre-push
```

The toolchain is pinned in `rust-toolchain.toml`. Before contributing, read
`ARCHITECTURE.md`: section 0 lists the hard rules, and the rest covers
how the workspace is laid out and which tests hold it in shape.

## Configuration

relift reads its own settings from `relift.toml` in the working directory (or
the file given with `-c`), then `RELIFT_*` environment variables, then flags.
Per-game data lives in each game project's `game.toml`, not here.
`relift.example.toml` lists every key.

<!-- configulator:begin -->

| Key               | Type    | Default | Environment              | Flag                | Description                                                                             |
|-------------------|---------|---------|--------------------------|---------------------|-----------------------------------------------------------------------------------------|
| `cache-dir`       | string  |         | `RELIFT_CACHE_DIR`       | `--cache-dir`       | where lifted, emitted and built artifacts are cached (empty: the user cache dir)        |
| `jobs`            | integer | `0`     | `RELIFT_JOBS`            | `-j`, `--jobs`      | parallel compile jobs (0: one per CPU)                                                  |
| `runtime-dir`     | string  |         | `RELIFT_RUNTIME_DIR`     | `--runtime-dir`     | the C runtime sources to build against (empty: the ones beside this relift's source)    |
| `color`           | string  | `auto`  | `RELIFT_COLOR`           | `--color`           | color in output: auto (a terminal that shows it, and NO_COLOR unset), always or never   |
| `toolchain.clang` | string  |         | `RELIFT_TOOLCHAIN_CLANG` | `--toolchain.clang` | the clang driver                                                                        |
| `toolchain.zig`   | string  |         | `RELIFT_TOOLCHAIN_ZIG`   | `--toolchain.zig`   | zig, for `zig cc` player builds                                                         |
| `toolchain.ninja` | string  |         | `RELIFT_TOOLCHAIN_NINJA` | `--toolchain.ninja` | the ninja build tool                                                                    |
| `emit.shard-kib`  | integer | `4096`  | `RELIFT_EMIT_SHARD_KIB`  | `--emit.shard-kib`  | the most KiB of C per emitted file; clang needs about 120 bytes of memory per byte of C |
| `emit.piece-kib`  | integer | `1024`  | `RELIFT_EMIT_PIECE_KIB`  | `--emit.piece-kib`  | the most KiB of C per C function; a larger guest function is split into several         |

<!-- configulator:end -->

## License

MIT. See `LICENSE`.
