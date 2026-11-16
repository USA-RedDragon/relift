# guests/switch/shaders

Finds every NVIDIA SM86 (Ampere, the Switch 2 GPU) shader program in a game:
`relift-switch-shaders` is the library and `relift-shaders` the tool. It reads
a decrypted `.nup`, or an extracted `exefs`/`romfs` tree, and writes each
unique program once.

```sh
cargo build --release -p relift-switch-shaders
relift-shaders scan game.nup --out shaders/ [--cache cache/] [-j 8] [--package 0]
```

`--out` gets `<sha256>.bin` per program, `manifest.json` (per program: `kind`
`sph` (graphics) or `cs` (compute), `stage` from the SPH, `sph` header size,
`size`, `n` times found, up to four `srcs`) and `summary.json` (counts, the
containers seen, and errors such as a missing zstd dictionary or a zstd frame
that decodes past 1 GiB, which is reported rather than cut short). The cache
defaults to `<out>/.cache`.

A program is the bytes of the 0x80-byte shader program header (graphics only)
and the code through the `BRA self` terminator. The scan unwraps zstd frames
(dictionaries come from the game's own `ZsDic.pack.zs`), SARC archives, and
looks for programs in shader archives (`bfsha`, BNSH, agl `BAHS`), sead `.bin`
files, effect files with embedded BNSH, the NSOs of `exefs`, and KTGL
`rdb`/`rdx`/`fdata` entries (whole or in zlib chunks; the zlib decoder is
this crate's own).

Each input file is one cached job, keyed by its path, size and content (the
SHA-256 a NUP records; a hash of the file for a directory, size and
modification time above 32 MiB), the dictionaries and, for an `.fdata`, the
byte ranges the indexes name, salted with the cache and crate versions, a
digest of the crate's sources, whether labels name packages and `--package`.
A rerun with no change restores every job from the cache without reading or
decompressing the input; a result is used only while its programs are still in
`--out`. Jobs run on `-j` threads.

The library takes any `Source`: `DirSource`, or `NupSource` over any
`relift_switch_formats::storage::Storage` (a file, memory, or a remote
reader).

```sh
cargo test -p relift-switch-shaders
```

Tests build synthetic containers; no game data is used.
