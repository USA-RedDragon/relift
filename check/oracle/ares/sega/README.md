# Sega 8-bit oracle (sega-oracle)

`sega-oracle` is a headless build of the ares Master System II, Game Gear
and SG-1000 cores that runs a cartridge for a number of frames with
scripted input and writes trace files: the executed instructions, every
frame's picture, per-frame RAM, VRAM and CRAM hashes, and optionally an
instruction-by-instruction log. The Sega 8-bit guest's exactness runs
compare relift's builds against it frame by frame.

## Licenses

ares is ISC licensed. Its source is fetched at a pinned commit, never
vendored; `patches/` holds the changes relift's oracle needs, which are
ares-derived and ISC:

    Copyright (c) 2004-2025 ares team, Near et al

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

The harness (`main.cpp`, `ms.cpp`, `sg.cpp`, `oracle.hpp`, `platform.hpp`,
`stubs/`) is MIT. The binary is a separate process that nothing in relift
links. No game data is here; ROMs are passed on the command line.

## Build

    check/oracle/ares/sega/build.sh [cache dir] [build dir]

Needs curl, sha256sum, tar, patch, cmake (3.20 or newer), ninja, zlib and
clang (`CC`/`CXX` override it; `JOBS` sets parallelism, default 4). The
script downloads the GitHub archive of ares commit
`0b928930af4cc59ed99e942996f0d3c815379388`, checks its SHA-256, unpacks it to
`<cache>/src` (default cache `~/.cache/relift/sega-oracle`), applies
`patches/*.patch` in name order, builds the ms and sg cores with the harness,
and copies the binary to `<cache>/bin/sega-oracle`.

The patches:

- `0001-sega-oracle-hooks.patch`: a hook struct (every hook null unless
  the harness sets one): instruction, CPU boundary (before interrupt
  acceptance and again before the instruction), interrupt, memory write,
  port, VDP frame and drawn-line hooks; ROM offset queries on the Sega,
  Codemasters and Korean boards; and access to private CPU and VDP state.
- `0002-z80-halt-refresh.patch`: a halted Z80 runs NOPs of 4 T-states that
  increment R, as the chip does; stock ares waits 1 T-state per halted step
  and leaves R alone.

## Running

    sega-oracle --rom FILE --id NAME --out DIR [--system ms|gg|sg]
                [--mapper sega|codemasters|korea] [--bios] [--frames N]
                [--input SCRIPT] [--rand-from FRAME] [--seed N]
                [--rand-block N] [--raw-dump FROM:TO] [--shot N] [--quiet]

`--mapper` takes the mapper relift's formats crate chose (`relift-smsrom
info --json`; its `none` is `sega`). `--bios` runs the image as the Master
System BIOS with no cartridge. `default-input.txt` is the input script of
the corpus runs, used with `--rand-from 1000 --seed 1 --rand-block 6`.
`ORACLE_ILOG=N` also writes the first N instructions to `insn.tsv`.

`DIR/NAME/` gets:

| File | Content |
|---|---|
| `exec.tsv` | every executed instruction: pc, region (`rom`, `ram`, `sram`, `open`), file or RAM offset, first frame, count, first four bytes; one row more per further byte form of code in RAM, cartridge RAM or the open bus |
| `frames_raw.tsv` | per frame the CRC-32 of the picture as the VDP drew it (palette indices, 16 bits a pixel; lines not drawn are 0) |
| `state.tsv` | per frame after the frame event: registers, RAM, VRAM and CRAM hashes, V and H counters |
| `ramhash.tsv` | per frame the work RAM hash at the first instruction boundary at or after the start of line 240 (192 on the SG-1000), at the CPU T-state relift's runtime maps that line to; relift's `--ram-log` samples the same instant |
| `input.tsv` | the button mask of each frame (relift's `--input` reads it) |
| `insn.tsv` | with `ORACLE_ILOG`: `I` T-state, pc and registers per instruction, `Q` interrupt acceptance, `O`/`N` port writes and reads |
| `mapper.tsv`, `irq.tsv`, `irq_summary.tsv`, `ramcode.tsv`, `meta.json` | bank register writes, interrupts, writers of RAM code, run facts |

Fetches from the open bus (the memory control port has unmapped the memory
there) read 0xFF, the bus pull-up of the Master System II and Game Gear.
A run of DD or FD prefixes followed by DD, FD or ED is one instruction in
ares and one 4 T-state instruction per prefix on the chip, so each prefix
byte gets its own `exec.tsv` row.

## Reference

At relift 695496f6 with this oracle, all 1,269 cartridges of a local
collection (12 SG-1000, 717 Master System including 13 BIOS images run with
`--bios`, 540 Game Gear) matched relift's dev-path builds on every frame of
3,000: picture, VRAM, CRAM and `ramhash.tsv`; the SG-1000 instruction logs
matched for 300 frames. This is a local check, not CI: it needs the ROMs and
runs for hours.
