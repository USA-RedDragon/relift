# guests/gba

The Game Boy Advance guest. State: pieces only; `relift import` does not take
a `.gba` file yet.

| Crate | Folder | Does |
|---|---|---|
| `relift-gba-formats` | `formats` | The cartridge header, complement and logo checks, size and mirror rules, save library strings, multiboot detection |
| `relift-gba-tools` | `tools` | `relift-gbarom`: header, checks and save strings |

```sh
cargo build --release -p relift-gba-tools
cargo test -p relift-gba-formats
relift-gbarom rom.gba
relift-gbarom --json rom.gba
```

Tests build their input with `synth`; no game data is used. The other GBA
pieces live elsewhere:

| Piece | Where |
|---|---|
| ARMv4T lifter | `cpus/arm32` |
| Machine core (bus timing, interrupts, timers, DMA, display), picture unit, cartridge saves, keypad, high-level BIOS | `runtime/guests/gba` |
| Reference emulator for comparisons | `check/oracle/nba` (NanoBoyAdvance) |

Whole-ROM function discovery and the CPU runtime layer are planned.
