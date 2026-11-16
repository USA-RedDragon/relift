# VR4300 oracle (ares)

`n64-oracle` is a headless build of the ares Nintendo 64 core that runs one
unit of CPU instructions from a JSON machine state and prints the resulting
state, so `relift-check --oracle ares lift` can compare relift's VR4300 lifter
against it. ares is ISC licensed: its source is fetched at a pinned commit,
never vendored, and the binary is a separate process nothing in relift links.
`driver.cpp` is MIT; `patches/` holds the hooks ares needs (a headless
frontend, state injection and extraction, exception reporting).

## Build

    check/oracle/ares/build.sh [cache dir] [build dir]

Needs git, cmake (3.24 or newer), ninja and clang (`CC`/`CXX` override it;
`JOBS` sets parallelism, default 4). The script fetches
`https://github.com/ares-emulator/ares` at commit
`0b928930af4cc59ed99e942996f0d3c815379388` into `<cache>/src/ares` (default
cache `~/.cache/relift/n64-ares`), applies `patches/*.patch`, builds only the
n64 core and the driver, and copies the binary to `<cache>/bin/n64-oracle`.

## Running the check

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
