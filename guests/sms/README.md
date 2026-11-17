# guests/sms

The Sega Master System, Game Gear and SG-1000 guest. State: `relift
import` takes a `.sms`, `.gg` or `.sg` cartridge, and `relift build` lifts it
from an ares oracle trace until discovery lands (below).

| Crate | Folder | Does |
|---|---|---|
| `relift-sms-formats` | `formats` | Image size and bank layout (odd sizes, copier header), the `TMR SEGA` header and export BIOS checksum, the Codemasters header and checksum, system detection, a static mapper guess |
| `relift-sms` | `guest` | The Z80 frontend for the runtime's machine (`runtime/guests/sms/run`), and a ROM's code lifted into one unit per ROM bank and slot and one per group of code in work RAM, with the program files that run it |
| `relift-sms-tools` | `tools` | `relift-smsrom`: `info` prints all of the above as text or JSON; `lift-trace` (development) lifts the instructions an ares trace executed |

```sh
cargo build --release -p relift-sms-tools
cargo test -p relift-sms-formats -p relift-sms -p relift-sms-tools
cargo test -p relift --test sms_run
relift-smsrom info rom.sms
relift-smsrom info --json rom.gg
```

The system comes from the header's region code; the file extension is used
only when there is no usable header. The mapper guess is a Codemasters
checksum match first, then a byte scan for `ld (nn),a` stores to the mapper
registers; function discovery refines it later. Tests build their input with
`synth`; no game data is used.

## Building a game

```sh
relift import game.sms
RELIFT_SMS_TRACE=exec.tsv relift build
relift run
```

`relift import` copies the ROM (less a copier header) into the project and
writes `game.toml` with `guest = "sms"`, the console (`sms`, `gg`, `gg-ms`
for a Master System cartridge on a Game Gear, or `sg`), the region (`ntsc-u`,
`ntsc-j` with the FM unit, `pal`) and the mapper guess; edit them there.
Until discovery's report is wired in, `relift build` takes the code from the
executed instructions of an ares oracle trace (`exec.tsv`, the spike's
oracle) named by `RELIFT_SMS_TRACE`, and fails without one. The program
takes `--headless`, `--frames N`, `--input FILE` (an input script per
frame), `--png`, `--png-dir`, `--wav FILE`, `--no-audio` and `--stats`; in
a window it reads the pad from the keyboard or a gamepad and plays the PSG
and FM sound. `relift/tests/e2e_sms.rs` runs the whole path on a synthetic
cartridge. `relift/tests/e2e_sg.rs` builds our own SG-1000 test ROM (the
TMS9918A's four modes with pseudo-random VRAM, sprites, the frame
interrupt, the pad and the PSG) and checks five frames against the CRCs
the ares SG-1000 core gives; its 600-frame run and instruction log matched
ares.

## Lifted code

`relift-sms` lifts the instructions it is given by location: a CPU address
and a ROM bank, or the instruction's bytes in work RAM (`code::Code`, the
shape of discovery's report). Every block ends in a sync point
(`rl_sms_sync`) and every access goes to the machine, so interrupts, the
VDP and the ports see each access at its T-state. Each ROM bank, as one
slot shows it, is a unit; an exit that may go on inside its unit carries a
check that the slot still shows the bank (or that work RAM still holds the
group's bytes), and every other transfer goes through dispatch by the
mapping. An instruction with no lifted body stops the run with a stop
report; there is no interpreter.

`relift-smsrom lift-trace ROM --exec exec.tsv --out DIR` writes the C of a
program from the executed instructions of an ares oracle trace, for
checking the machine and the run layer frame by frame before discovery
gives the code. Measured on the 26 test and homebrew ROMs of the Sega spike
(commit 77bc5f0b plus this guest): 23 match ares on all 3000 frames
(the CRC of every frame's VDP colours, VRAM and CRAM hashes), and their
per-instruction logs (T-state, pc, registers, every port access) match
ares for the first 300 frames; zexall and zexdoc match up to their stop at
RAM code whose opcode bytes change while it runs (the forms come from
discovery). Code in work RAM or cartridge RAM is lifted with its operands
read when it runs. On a sample of 60 commercial Master System and Game Gear
ROMs, 59 match ares on all 3000 frames; the other stops at RAM code whose
opcode bytes change.
