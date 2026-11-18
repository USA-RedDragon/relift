# guests/switch

The Nintendo Switch guest. It reads decrypted dumps only and has no keys.

| Crate | Folder | Does |
|---|---|---|
| `relift-switch-formats` | `formats` | romfs, NSO, NRO, NPDM, NUP and IPSwitch `.pchtxt` readers, and the import of a `.nup` or an extracted tree |
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
"switch2"` (or `"switch"`) in the `[game]` table of `game.toml` overrides the
choice.

`relift build --mods <list>` builds code mods in: Atmosphere or Ryujinx mod
directories, `.ips` files and `.pchtxt` files, applied in order. Two mods that
write different bytes at one address are refused. Data mods and cheats are
chosen at run time (`RELIFT_MODS`, `runtime/README.md`).

Missed code, hot-loading, stop reports, `relift coverage` and `relift diff`
are described in `docs/guides/usage.md`.
