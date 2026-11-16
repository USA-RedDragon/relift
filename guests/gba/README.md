# guests/gba

The Game Boy Advance guest.

| Crate | Folder | Does |
|---|---|---|
| `relift-gba-formats` | `formats` | The cartridge header, complement and logo checks, size and mirror rules, save library strings, multiboot detection |
| `relift-gba-analyze` | `analyze` | Function discovery across ARM and Thumb code from the reset entry and the IRQ vector, including code the ROM copies to work RAM |
| `relift-gba` | `guest` | The ARMv4T frontend over `relift-cpu-arm32` and the lifting of a ROM into one unit with its program files |
| `relift-gba-tools` | `tools` | `relift-gbarom`: header, checks, save strings and `functions` |

```sh
cargo build --release -p relift-gba-tools
cargo test -p relift-gba-formats -p relift-gba-analyze -p relift-gba
relift-gbarom rom.gba
relift-gbarom --json rom.gba
relift-gbarom functions rom.gba
```

Tests build their input with `synth`; no game data is used. The other GBA
pieces live elsewhere:

| Piece | Where |
|---|---|
| ARMv4T lifter | `cpus/arm32` |
| Machine core (bus timing, interrupts, timers, DMA, display), picture unit, cartridge saves, keypad, high-level BIOS, the program | `runtime/guests/gba` |
| Reference emulator for comparisons | `check/oracle/nba` (NanoBoyAdvance) |

## Building a game

```sh
relift import game.gba
relift build
relift run          # or the built program: game --headless --frames 600 --png last.png
```

The import copies the ROM into the project; the build finds its code from
the reset entry and the IRQ vector, lifts ARM and Thumb code (and code the
ROM copies to work RAM) into one unit and links the runtime's ARMv4T and
GBA layers (`docs/abi.md` section 13, "The Game Boy Advance"). The program
runs on the high-level BIOS; no BIOS image is needed. The backup (SRAM,
Flash or EEPROM) is a `.sav` next to the ROM, or in the `save.dir`
setting's folder; Android and iOS apps keep it as `battery.sav` in the
app's own folder. Missed code stops the program with a stop report
(`docs/usage.md`); hot-loading is not available for this guest yet.

`relift-gbarom` reads and checks a ROM header and lists the save library
strings and the multiboot entry of an image that fits work RAM.
`relift-gbarom functions` lists the functions of a ROM across ARM and Thumb
code, the code it copies to work RAM and the transfers it cannot resolve;
`--multiboot` reads a multiboot image.
