# guests/switch

The Nintendo Switch guest. It reads decrypted dumps only and has no keys.

| Crate | Folder | Does |
|---|---|---|
| `relift-switch-formats` | `formats` | romfs, NSO, NRO, NPDM, NUP and IPSwitch `.pchtxt` readers, and the import of a `.nup` or an extracted tree |
| `relift-switch-fonts` | `fonts` | The open fallback for the shared fonts: Noto Sans CJK variable fonts subset to each font type's standard character set and instanced at regular weight |
| `relift-switch-names` | `names` | Itanium names as host names, the address fallback and the reverse map |
| `relift-switch` | `guest` | The guest over the formats and `relift-cpu-arm64`: the CPU feature set, module loading, exception-handling tables, the program description |
| `relift-switch-tools` | `tools` | `relift-nup`, `relift-romfs`, `relift-nso`, `relift-nro`, `relift-npdm` and `relift-ir` |
| `relift-switch-shaders` | `shaders` | `relift-shaders`: finds the SM86 (Switch 2) or SM53 (Switch 1) shader programs of a game |
| `relift-switch-vkgen` | `vkgen` | `relift-vkgen`: generates the Vulkan forwarding C from the pinned `vk.xml` |

```sh
cargo build --release -p relift-switch-tools
cargo test -p relift-switch-formats -p relift-switch -p relift-switch-names
relift-nup ls game.nup
relift-nso show main.nso
```

Tests build synthetic input; no game data is used. The runtime half is
`runtime/guests/switch`: the Horizon kernel, the scheduler, IPC and the
services, and `vk/` for Vulkan forwarding.

## Building a game

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
has them; they give the game's title and icon (`docs/guides/packaging.md`).
Encrypted dumps are refused: convert them with a separate tool first.

Import never extracts the romfs into a folder tree. From a `.nup` it writes a
RomFS map (`romfs.map`, a few MB) that reads every file in place in the
`.nup` after checking its SHA-256, plus `romfs.blob` holding the files the
`.nup` stores zstd compressed; keep the `.nup` where it was imported from.
From an extracted tree it writes one plain image, `romfs.img`. Either way a
game with hundreds of thousands of romfs files takes a few files on disk.
`relift-romfs hash <romfs>` lists every file's SHA-256 of a map, an image or a
tree, to compare two of them.

The CPU code is lifted for comes from `main.npdm` as well: a program whose
kernel capabilities grant SVC 0x80 is a Switch 2 program (Cortex-A78C, with
pointer authentication), any other one a Switch program (Cortex-A57), and
Switch programs a Switch 2 runs keep the Cortex-A57 they were compiled for.
A program without an NPDM (homebrew) is a Switch program; `console =
"switch2"` (or `"switch"`) in the `[game]` table of `relift.toml` overrides the
choice.

`relift build --mods <list>` builds code mods in: Atmosphere or Ryujinx mod
directories, `.ips` files and `.pchtxt` files, applied in order. Two mods that
write different bytes at one address are refused. Data mods and cheats are
chosen at run time (`RELIFT_MODS`, `runtime/README.md`).

Missed code, hot-loading, stop reports, `relift coverage` and `relift diff`
are described in `docs/guides/usage.md`.

## Shaders at launch

The build translates every GPU program of a game to SPIR-V (`shaders/table.rlspv`), so the translation never runs
while the game does. A host Vulkan driver still compiles each SPIR-V stage to its GPU's code, and a driver that
does that when a pipeline is first made would stall the first draw of each pipeline during play. The runtime does
that compile at launch instead, and the game starts when it is done.

Where the host driver has `VK_EXT_graphics_pipeline_library` with fast linking (the NVIDIA driver and lavapipe do), a game's pipeline is made from four libraries: vertex input, pre-rasterization shaders, fragment
shader and fragment output. The launch warm-up makes a pre-rasterization library of every vertex program and a
fragment library of every fragment program with the specialization constants a draw starts from, and a compute
pipeline of every compute program the game has registered, on one worker thread per processor (fewer when the
available memory is short: a driver's compile of a large program was measured at 2.4 GiB). A draw links the four
libraries it needs, which the driver does without compiling a stage. State that changes from draw to draw (culling,
depth and stencil tests and operations, depth bias, rasterizer discard) is dynamic state, so it is no part of any
library.

A pipeline cache (`VkPipelineCache`) holds what the driver compiled. It is written to the app's user folder, with a
header naming the device and driver UUIDs, the driver version, the device and vendor ids and the SHA-256 of the
shader table's index; a file made for anything else is discarded and the warm-up compiles again.

A host driver without the extension (MoltenVK, and the Adreno 740 driver on a Thor, which lists neither it nor
`VK_EXT_shader_object`) compiles a stage only inside a whole pipeline, and the whole pipeline depends on state the
build cannot know. Creating a shader module does no work there (0.1 ms on MoltenVK, 0.3 ms on the NVIDIA driver),
and a second pipeline of the same two stages with another blend, render target format or vertex stride costs as
much as the first (7.7 s and 7.5 s on MoltenVK, 3.1 s and 3.1 s on the NVIDIA driver), so no work done on a stage
carries to a pipeline of different state. What can be done ahead of the draw is the exact pipelines: the runtime
records every pipeline the game makes (`pipeline-keys.bin` beside the cache) and the next launch makes all of them,
in parallel, before the game starts. The first play on such a host still compiles a pipeline when a draw first needs
it; no later launch does.
