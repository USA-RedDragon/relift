# GBA oracle (NanoBoyAdvance)

`nba-oracle` runs a GBA ROM on the NanoBoyAdvance core and answers a line
protocol on stdin and stdout, so `relift-check --oracle nba` can compare
relift against a cycle-accurate emulator. NanoBoyAdvance is
GPL-3.0-or-later: its source is fetched at a pinned commit, never vendored,
and the binary is a separate process that nothing in relift links.
`driver.cc` is MIT; the binary built from it and the core is GPL-3.0-or-later.

## Build

    check/oracle/nba/build.sh [cache dir] [build dir]

Needs git, cmake (3.24 or newer), ninja and clang (`CC`/`CXX` override it).
The script clones `https://codeberg.org/nba-emu/NanoBoyAdvance` at commit
`e8482c1efc6cde3f553e8170a51abea71cee426e` into `<cache>/src/nba` (default
cache `~/.cache/relift/gba-oracle`), builds only the core library, its fmt
and atom dependencies and the driver (no Qt, SDL or OpenGL), and copies the
binary to `<cache>/bin/nba-oracle`. With `RELIFT_NBA_PATCHES=<dir>` the
`*.patch` files in that directory are applied to the checkout first, in
name order.

`relift-check` finds the binary through `RELIFT_NBA_ORACLE`, else at
`~/.cache/relift/gba-oracle/bin/nba-oracle`; its tests skip when neither
exists.

## Running

    nba-oracle --rom FILE (--bios FILE | --skip-bios) [--save TYPE] [--save-file FILE]

`--skip-bios` starts at 0x08000000 in system mode with the stacks the BIOS
would set; without `--bios` the BIOS region reads as zero. The save type is
`none`, `sram`, `flash64`, `flash128`, `eeprom4` or `eeprom64`; the default is
`sram` when the ROM holds `SRAM_V`, else `none`. SRAM lives in memory
(`--save-file` loads it, `SAVE` writes it); the other types need
`--save-file` and write it as they go.

## Protocol

After start the oracle prints `READY nba-oracle 1`. Each command is one line;
numbers are hex unless noted. A failed command answers `ERR <message>`
instead (a `RUN frames` that hits its limit of N+1 frames of cycles has run
those cycles).

| Command | Answer |
|---|---|
| `RUN cycles N` (N decimal) | runs N cycles (the last instruction may overshoot), `OK cyc=<total> frames=<count>` |
| `RUN frames N` (N decimal) | runs until N more frames have been drawn (a frame ends at the start of VBlank) |
| `RUN until LIMIT ADDR` (LIMIT decimal) | runs until the next instruction is at ADDR, at most LIMIT cycles |
| `STEP N` (N decimal) | runs N instructions, one trace line each, then `OK cyc=<total>` |
| `KEYS MASK` | holds the keys of MASK (KEYINPUT bit order, 1 = pressed), `OK` |
| `REGS` | `REGS r0=.. r15=.. cpsr=.. spsr_<mode>=.. r8_fiq..r14_fiq r13_<mode> r14_<mode> r8_usr..r12_usr pc=.. halt=0/1 cyc=<decimal>` |
| `SET name=value ...` | writes r0..r14, cpsr (switches the register bank) or spsr_<mode>, `OK` |
| `JUMP ADDR 0/1` | continues at ADDR in ARM (0) or Thumb (1) state with a refilled pipeline, `OK cyc=..` |
| `MEM ADDR LEN` | `MEM <hex bytes>` read from the backing memory, without bus timing or side effects |
| `POKE ADDR HEXBYTES` | writes EWRAM, IWRAM, palette, VRAM, OAM or SRAM, `OK` |
| `IO` | `IO <hex bytes>`: 0x04000000..0x040003FF as the CPU reads them, by halfword |
| `IOPOKE ADDR HEXBYTES` | writes I/O registers (offsets 0..3FF from 0x04000000) one byte at a time through the bus, `OK` |
| `PPUREGS` | `PPUREGS <hex>`: the 0x58 bytes of picture unit registers as last written, including the ones the CPU cannot read back (scroll, affine, windows, mosaic, BLDY) |
| `FRAME` | `FRAME count=<decimal> hash=<16 hex>` |
| `PIXELS` | `PIXELS <hex>`: the last frame, 240x160 BGR555 halfwords, little-endian |
| `AUDIO` | `AUDIO samples=<decimal> hash=<16 hex>` since reset |
| `SAVE PATH` | writes the 32 KiB SRAM, `OK` |
| `RESET` | power cycle with the same ROM, BIOS and keys, `OK` |
| `QUIT` | `OK`, then the process exits |

`REGS pc` is the address of the next instruction (r15 minus 8 in ARM state,
minus 4 in Thumb). Trace lines:

- `T <cyc> <pc> <opcode> <r0> .. <r15> <cpsr>`: the instruction at pc ran;
  the registers and the cycle count are those after it.
- `I <cyc> <pc>`: an interrupt was taken before the instruction at pc; the
  next `T` line is the vector at 0x18.
- `H <cyc>`: the CPU was halted until cyc.

Both hashes are FNV-1a 64. The frame hash runs over the 240x160 BGR555
pixels as little-endian halfwords; the audio hash over each mixer sample
(left, right) as a signed 16-bit value in the -512..511 range, before
resampling, so it depends only on the APU.
