# guests/sms

The Sega Master System, Game Gear and SG-1000 guest. State: pieces only;
`relift import` does not take a `.sms`, `.gg` or `.sg` file yet.

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
ares for the first 300 frames; the other 3 match up to their stop at RAM
code whose bytes change while it runs.
