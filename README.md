# relift

relift is a static recompiler. It translates a console game's machine code
ahead of time into portable C11, which clang or `zig cc` builds into a native
program for Linux, macOS, Windows and other hosts, linked against a C runtime
that stands in for the console's operating system.

The tool is written in Rust. The first guest is the Nintendo Switch (ARM64);
the core is guest-neutral so other consoles can follow.

**Status: early development, nothing works yet.** The repository holds the
workspace skeleton and its structural tests.

relift ships no game code, keys or data, and never will. You supply your own
legally obtained dumps.

## Importing a game

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

## Tools

| Binary | Does |
|---|---|
| `relift` | the recompiler: `import`, `build`, `run`, `check`, `where` |
| `relift-nup` | list and extract `.nup` containers |
| `relift-romfs` | list, extract and print files of a romfs image |
| `relift-nso` | show, list symbols of and decompress NSO modules |
| `relift-nro` | show NRO modules and extract their assets |
| `relift-npdm` | show a `main.npdm` |
| `relift-gbrom` | show a Game Boy ROM header and validate its checksums |

## Building

```sh
cargo build --release -p relift -p relift-switch-tools
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

| Key               | Type    | Default | Environment              | Flag                | Description                                                                      |
|-------------------|---------|---------|--------------------------|---------------------|----------------------------------------------------------------------------------|
| `cache-dir`       | string  |         | `RELIFT_CACHE_DIR`       | `--cache-dir`       | where lifted, emitted and built artifacts are cached (empty: the user cache dir) |
| `jobs`            | integer | `0`     | `RELIFT_JOBS`            | `-j`, `--jobs`      | parallel compile jobs (0: one per CPU)                                           |
| `toolchain.clang` | string  |         | `RELIFT_TOOLCHAIN_CLANG` | `--toolchain.clang` | the clang driver                                                                 |
| `toolchain.zig`   | string  |         | `RELIFT_TOOLCHAIN_ZIG`   | `--toolchain.zig`   | zig, for `zig cc` player builds                                                  |
| `toolchain.ninja` | string  |         | `RELIFT_TOOLCHAIN_NINJA` | `--toolchain.ninja` | the ninja build tool                                                             |

<!-- configulator:end -->

## License

MIT. See `LICENSE`.
