# guests/n64

The Nintendo 64 guest. State: `relift import`, `build` and `run` take a
ROM in any byte order and run it with every CPU and RSP instruction lifted;
the RDP and audio are not there yet; controller 1 and the cartridge saves (SRAM, Flash and EEPROM, found by behavior) are.

| Crate | Folder | Does |
|---|---|---|
| `relift-n64-formats` | `formats` | Byte orders and normalization, the header, the boot variant by hash, CRC1/CRC2, Yay0, Yaz0 and MIO0 decoders and a stream scan |
| `relift-n64-analyze` | `analyze` | Code discovery: the VR4300 IR over abstract values from the boot entry, the PI DMA fixed point, the ROM-wide region scan with the link address vote, relocation masks, the sweep, the RSP image list and the microcodes' IMEM layouts |
| `relift-n64` | `guest` | The VR4300 frontend for the runtime's N64 layer, the RSP frontend for its RSP layer and the build's lift: one unit per segment linked in RDRAM, one per relocatable RSP image of at most 4 KiB and one per IMEM layout, `program.c` with every code run's source, link address and hash, `rsp_images.c` with every RSP image's bytes and successors |
| `relift-n64-tools` | `tools` | `relift-n64rom`: `info`, `streams`, `normalize`, `functions` |

```sh
cargo build --release -p relift-n64-tools
cargo test -p relift-n64-formats -p relift-n64-analyze -p relift-n64-tools
relift-n64rom info rom.z64
relift-n64rom info --json rom.v64
relift-n64rom streams rom.n64
relift-n64rom normalize rom.v64 rom.z64
relift-n64rom functions rom.z64
relift-n64rom functions --json --cic 6102 rom.z64
```

`functions` prints the entry point, the counts, the constant PI DMAs, the RSP
images, the IMEM layouts and one line per function entry (address, source,
source offset, origin), then the transfers left unknown. With `--json` it also lists the
segments and the code units: each unit is a run of code words with its
source (the ROM or a decoded stream), offset, link address and a hash of the
words with the relocation fields cleared, the key the runtime binds code by
when it finds those bytes in RDRAM. The analysis uses no per-game table.

The boot code area (`0x40..0x1000`) is hashed to name the boot variant. For a
known variant the analysis also lifts the boot code where the console runs
it, in DMEM from `0xA4000040`, and follows the code it copies to RDRAM with
a word copy loop; the game it loads is placed by the variant's rule. The
boot code of a variant not known by its hash is not followed. The checksum check is available for the
variants whose checksum is computed from the game's own data (6101, 6102,
6103, 6106); for the others it reports `not checked`. `info --cic 6102`
checks as that variant, for images with a custom boot code.

## Running a ROM

```sh
relift import rom.z64        # writes game.toml with guest = "n64"
relift build                 # analysis, lift, C, the runtime's N64 layers
relift run
game --headless --frames 60 --png out.png --stats
```

The program runs the cartridge's boot code as lifted code: the runtime sets
only what the PIF's boot leaves (the ROM's first 4 KiB in DMEM, the PI
timing from the header, the RI registers, the registers at the jump to
`0xA4000040`), and the boot code loads, checks and starts the game at the
console's cycles. A ROM whose boot variant is not known by its hash boots at
a high level instead: the runtime sets what the boot code leaves (the first
MiB of the game at the entry point, the RDRAM size word, the registers). No
lifted code is in dispatch at the start. Each code run the analysis found
is entered through a check that hashes its bytes in RDRAM and binds the run
when they match; a PI, SP or SI DMA into RDRAM or an instruction cache
invalidation makes the next entry check again. An entry that no lifted run
matches is missed code: the program stops with a report (and an RDRAM dump
with `--dump`). Nothing is interpreted; `--stats` prints `interpreted 0`
with the binding counters.

RSP microcode is found the same way, with no per-game table: the scan finds
runs of RSP code, and each microcode the CPU code loads is run over abstract
values from its start (`analyze/src/ucode.rs`). Its own SP DMAs to IMEM
lead, through the task words the CPU code wrote into DMEM and the constant
addresses the CPU code builds, to the RSP code and data in RDRAM and, through
the analysis's segments, to their bytes in the ROM; an overlay table in the
microcode's data gives the overlays. Each IMEM layout it can make (its own
code, the overlays over it and what remains of the code around them) is
lifted as its own image, with the IMEM its DMAs may overwrite entered only
through the layer's byte check.

Controller 1 reads the `p1` port (`bind.p1.<control>` settings, a gamepad
or the keyboard: arrows for the stick, X, Z and C for A, B and Z, I, J, K,
L for the C buttons, WASD for the pad). Saves go in the user folder's `save` folder as `<rom name>.eep`, `.sra` and `.fla` (`save.dir` or
`--save BASE` change the place, `--no-save` turns it off); only the stores
the game changed are written. EEPROM is reported as 16 Kbit unless a 512-byte
`.eep` exists; a game that only addresses blocks below 64 works the same
either way. The Controller Pak is absent.

The picture is what VI scans out (ares' software path: 640 by 480, or 576
for a PAL cartridge, scaled and window-clipped by the video registers, one
field per frame when interlaced; no anti-aliasing, dither, divot or gamma)
and is presented at the guest's own frame rate. `--png FILE` writes the last
frame's picture, `--raw-png FILE` the raw framebuffer. The sound is AI's DMA
read at the DAC's rate and resampled to 44100 Hz: it plays through the
audio output in a window, `--wav FILE` captures it (also headless) and
`--no-audio` opens no output.

Tests build their input with `synth`; no game data is used.
`relift/tests/e2e_n64.rs` builds and runs a synthetic ROM that loads a
routine with a PI DMA and calls it.
