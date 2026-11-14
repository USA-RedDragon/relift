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

## Building

```sh
cargo build --release -p relift
pre-commit install --hook-type pre-commit --hook-type pre-push
```

The toolchain is pinned in `rust-toolchain.toml`. Before contributing, read
`ARCHITECTURE.md`: section 0 lists the rules that bite, and the rest covers
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
