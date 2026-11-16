# ares oracles

Two separate builds from the same pinned ares commit: `build.sh` builds the
Nintendo 64 oracle for the VR4300 lifter, and `build-step.sh` builds the
steppers for the Z80 templates and the SN76489 device.

## VR4300 oracle (n64-oracle)

`n64-oracle` is a headless build of the ares Nintendo 64 core that runs one
unit of CPU instructions from a JSON machine state and prints the resulting
state, so `relift-check --oracle ares lift` can compare relift's VR4300 lifter
against it. ares is ISC licensed: its source is fetched at a pinned commit,
never vendored, and the binary is a separate process nothing in relift links.
`driver.cpp` is MIT; `patches/` holds the hooks ares needs (a headless
frontend, state injection and extraction, exception reporting).

### Build

    check/oracle/ares/build.sh [cache dir] [build dir]

Needs git, cmake (3.24 or newer), ninja and clang (`CC`/`CXX` override it;
`JOBS` sets parallelism, default 4). The script fetches
`https://github.com/ares-emulator/ares` at commit
`0b928930af4cc59ed99e942996f0d3c815379388` into `<cache>/src/ares` (default
cache `~/.cache/relift/n64-ares`), applies `patches/*.patch`, builds only the
n64 core and the driver, and copies the binary to `<cache>/bin/n64-oracle`.

### Running the check

    relift-check --oracle ares lift [-n N] [--seed S] [--mutate] [--fp-corpus]
                 [--bin FILE] [--rom FILE] [--work DIR] [KIND...]
    relift-check --oracle ares lift --list

The binary is found through `--bin`, `RELIFT_ARES_ORACLE`, or
`~/.cache/relift/n64-ares/bin/n64-oracle`. Every lifted encoding runs on N
random states (one or two instruction units, with delay slots) on the IR
interpreter and on ares; GPRs, HI/LO, FPRs, FCR31, the CP0 set, a 128-byte
data region, the next PC and the exception code are compared.

- `--fp-corpus` runs the 7,040-case FP special-value corpus. Cases where the
  lifted code takes the FP slow path (guard 0x2000, handled by the runtime)
  are not compared against ares's result but against the rule the runtime
  applies (`RULE` lines); the check fails if the rule and ares differ.
- `--mutate` breaks the first live operation of each lifted block and expects
  the comparison to catch it.
- `--rom FILE` runs the oracle's boot code from a ROM first (any N64 test ROM;
  never committed). Without it the oracle starts at once.
- Known divergence: SC/SCD with a misaligned address and LLbit clear, where
  relift follows the manual and ares does not (counted separately).

Reference numbers at `-n 25`: 157 kinds, 3,890 cases pass, 0 fail, 2 known
divergence, 33 slow path. With `--fp-corpus`: 2,735 compared pass and 4,305
slow path, all matching the rule.

This is a local check, like the GBA oracle: it is not in CI because the ares
build and the full run take several minutes each.

## Z80 oracle (z80-step)

`z80-step` runs one instruction of the Z80 core of ares, or takes one
interrupt, per input line, so `relift-check --oracle ares-z80` can compare the
Z80 templates of `relift-cpu-z80` with an independent implementation on random
states. ares is ISC-licensed (https://github.com/ares-emulator/ares); its
source is fetched at a pinned commit, never vendored, and the binary is a
separate process that nothing in relift links. `z80-step.cpp`, `json.hpp` and
`ares.gen.cpp` are MIT.

### Build

    check/oracle/ares/build-step.sh [cache dir]

Needs curl, tar, sha256sum and clang++ (`CXX` overrides it); a build takes
under a minute. The script downloads the GitHub archive of ares commit
`0b928930af4cc59ed99e942996f0d3c815379388`, checks its SHA-256, unpacks it to
`<cache>/src` (default cache `~/.cache/relift/ares-oracle`) and builds the
core's Z80 and the harness into `<cache>/bin/z80-step`. Nothing else of ares
is compiled: no frontend, audio or video.

### Running

    z80-step [--check] [--quiet] < states.jsonl

Each input line is a SingleStepTests/z80 case (only `initial` is read), or a
whole SST file. The output has one line per case: the final state, the RAM
bytes the case lists or the instruction wrote, `cycles` (address, value, kind
`f r w i o`, and the time the access is logged at) and `tstates`. A line with
`"irq": K` takes an interrupt instead (0 NMI, 1 IM 0, 2 IM 1, 3 IM 2, with 0xFF
on the data bus). `--check` compares with the `final` of each case and prints
the pass counts and the failing opcodes on stderr.

    relift-check --oracle ares-z80 run [N [SEED]] [--bin FILE]
    relift-check --oracle ares-z80 rand N SEED [--irq] > in.jsonl
    z80-step < in.jsonl > out.jsonl
    relift-check --oracle ares-z80 compare in.jsonl out.jsonl

`run` does all three steps: N random states per template (default 100, seed
31), then 10 N per interrupt entry. The binary is `--bin`, `RELIFT_ARES_Z80`
or `~/.cache/relift/ares-oracle/bin/z80-step`.

### Where ares and relift differ

ares departs from SingleStepTests, and so from the real chip, in a few
places. The comparison counts these as known and fails on any other
difference. With 100 states per template and seed 31, 177,416 of 178,000
instruction states are equal, and the 584 others are:

| Opcode | Difference |
|---|---|
| `ED 40`, `ED 48` (`IN B,(C)`, `IN C,(C)`) | ares sets WZ to BC + 1 after the `IN` has overwritten B or C |
| `DD 37`, `DD 3F`, `FD 37`, `FD 3F` | ares keeps Q across the `DD` or `FD` prefix, which is an instruction of its own for Q, so X and Y of `SCF` and `CCF` differ |
| `E3`, `DD E3`, `FD E3` (`EX (SP),HL/IX/IY`) | ares writes the low byte first; the chip writes (SP+1) first |

The interrupt entries agree on registers, RAM and T-states. The access
times of the NMI and IM 2 entries differ: ares pushes PC and then waits where
the chip's 5 T-state acknowledge cycle comes first (NMI), and reads the vector
before pushing PC where the chip pushes first (IM 2). The templates follow
the Zilog manual.

## SN76489 oracle (psg-step)

`psg-step` drives ares' SN76489 and the mix of `ms/psg/psg.cpp` from a script
of timed writes and prints every sample: the four attenuations ares' `clock()`
returns and the left and right values, as hexadecimal floats. ares' volume
table is the same scale as relift's (`2^(-level/3)`, 15 off), so the mixed
values are compared exactly too, with no mapping.

    check/oracle/ares/build-step.sh [cache dir]
    make -C runtime BUILD=<dir> <dir>/sms_psg_script
    python3 -I check/oracle/ares/psg-compare.py --ares <cache>/bin/psg-step \
        --relift <dir>/sms_psg_script [--scripts N] [--events N] [--seed N]

Each random script runs once as a Master System and once as a Game Gear and
must give identical samples. The script format is in the header of
`psg-step.cpp`. A write at clock t is applied after every sample complete by t;
the harness fixes that order itself, so the comparison covers the chip and the
mix, not ares' thread scheduling.
