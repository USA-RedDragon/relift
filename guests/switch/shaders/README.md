# guests/switch/shaders

Finds every NVIDIA SM86 (Ampere, the Switch 2 GPU) shader program in a game,
or with `--sm53` every SM53 (Maxwell, the Switch 1 GPU) program:
`relift-switch-shaders` is the library and `relift-shaders` the tool. It reads
a decrypted `.nup`, or an extracted `exefs`/`romfs` tree, and writes each
unique program once.

```sh
cargo build --release -p relift-switch-shaders
relift-shaders scan game.nup --out shaders/ [--cache cache/] [-j 8] [--package 0] [--sm53]
```

`--out` gets `<sha256>.bin` per program, `manifest.json` (per program: `kind`
`sph` (graphics) or `cs` (compute), `stage` from the SPH, `sph` header size,
`size`, `n` times found, up to four `srcs`), `meshes.json` (each BNSH and agl BAHS
program with a task or mesh stage: the SHA-256 of its `fragment`, `task` and
`mesh` programs, their GLSLC headers `task_glslc` and `mesh_glslc`, which
hold the pipeline state the SPH lacks, and the mesh stage's `primitive_sph`)
and `summary.json` (counts, the
containers seen, and errors such as a missing zstd dictionary or a zstd frame
that decodes past 1 GiB, which is reported rather than cut short). The cache
defaults to `<out>/.cache`. A `--package` that holds no file is an error.

A program is the bytes of the 0x80-byte shader program header (graphics only)
and the code through the `BRA self` terminator. The scan unwraps zstd frames
(dictionaries come from the game's own `ZsDic.pack.zs`), SARC archives, and
looks for programs in shader archives (`bfsha`, BNSH, agl `BAHS`), sead `.bin`
files, effect files with embedded BNSH, the NSOs of `exefs`, and KTGL
`rdb`/`rdx`/`fdata` entries (whole or in zlib chunks; the zlib decoder is
this crate's own).

With `--sm53` every file is read (NSOs as their flat image). A program
starts at an NVN code header (the word 0x12345678 and 44 zero bytes); its
bytes are the 0x50-byte SPH (graphics only) and the code through the last
bundle control reaches from the first instruction (branch, call and
SSY/PBK targets). The scan unwraps zstd frames (also inside archives such
as BEA, which store files as frames after a table; a frame magic that
starts no decodable frame is counted as `zstd-embedded-undecodable`),
SARC archives, Yaz0 streams, UnityFS bundles (stored, LZMA or LZ4 blocks)
and the compressed blobs of Unity Shader objects. Switch builds strip the
type tree, so a Shader's blob is found by the shape of its fields: the
platform list, the offsets, compressed and decompressed lengths (one per
platform, or a list per platform from Unity 2021), then the blob, with
every segment inside it LZ4-decompressing to exactly its length.

CUDA modules are not carved: each ELF cubin (whole, or an entry of a fat
binary, stored plain, LZ4 or zstd) gives one compute program per sm_86 or
sm_87 kernel. The kernel's `.text` section sits at address 0, followed by
every function section its relocations reach and every one whose address
the module's data holds, and the relocations that point into that layout
are applied (call targets, return addresses), so `CALL.ABS` and `RET.ABS`
resolve inside the program. References outside it (functions the driver
provides, data addresses) keep the cubin's value and are listed in the
summary's errors.

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
