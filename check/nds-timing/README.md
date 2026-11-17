# Nintendo DS CPU timing against the consoles

A test ROM measures how long small loops take on both CPUs of a DS: 578
ARM9 and 474 ARM7 cases. Each case is a timing kernel (ALU, multiplies,
branches, loads and stores of every width, LDM and STM, Thumb), a data
region (TCMs, main RAM cached and uncached, I/O, VRAM, palette, OAM, work
RAM), a code placement, a cache mode and an MPU attribute. Each result is
the cycles per loop iteration: the minimum of 5 runs of 16 and of 48
iterations, timed by the cascaded timers at 33.513982 MHz.

- `gen.py OUT` writes the kernels (`k9.s`, `k7.s`), their tables and the
  case list (`cases.h`).
- `gen_swi.py OUT` writes the BIOS call cases' inputs (LZ77, Huffman and
  run-length data in the BIOS's formats); `swi.c`, `svc.s`, `card.c` and
  `wake.c` are the BIOS call, cartridge transfer and interrupt wake cases
  the SDK build runs after the kernels (CSV rows from idx 1000, 2000 and
  3000; the header lines say what their columns hold). Hwtest 3c adds
  `fixed.c` (the fixed cost of one call, idx 6000), more inputs and calls
  in `swi.c` (idx 1024 on), the ARM7's BIOS calls and main RAM kernels
  again with the ARM9 off main RAM (idx 5000 and 4200 on), and
  `contend.c`: both CPUs on main RAM at once, each pair timed alone and
  together, with either main RAM priority (idx 4000 on). The SDK build's
  `hw_after` hook lets a bundling build run more after the timing file is
  closed.
- `sdk/` is the devkitPro build that ran on the consoles (`make` gives
  `relift-hwtest-ntr.nds`, a plain DS cartridge that refuses DSi mode and
  the 133 MHz ARM9 clock, and `relift-hwtest-dsi.nds`). It writes
  `relift-hwtest.csv` to the SD card.
- `build.py OUT.nds` builds the same kernels and cases with clang and
  ld.lld, without an SDK (`h9.c`, `h7.c`). It sets up the console as the
  SDK leaves it (MPU regions, cache and write buffer bits, TCMs, the ARM7
  program in shared work RAM) and prints the CSV through the no$gba debug
  port. In melonDS it gives the SDK build's numbers in every case it runs.
  It does not yet run the 9 ARM9 cases whose code is copied to the
  instruction TCM. `build.py OUT.nds --calls` builds the cartridge that
  runs the BIOS call, cartridge, wake, fixed-cost and contention cases
  instead of the kernels; `bare.S` holds the SDK build's compiled code of
  the parts whose instructions are timed (callbacks, call loops, the
  ARM7's transfer loop, the wake sequences), so both builds time the same
  instructions.
- `hw.csv` holds the results of three consoles in DS mode: a 3DS through a
  slot-1 cartridge, and a DSi twice. They agree on all but 11 cases (the
  `may_differ` column marks the cases that depend on main RAM or the MPU).
  Of those 11, 7 are palette accesses that contend with the display, and
  2 are straight-line instruction cache loops whose steady state depends
  on the round-robin counter the code before them leaves.
- `hw.csv` also holds hwtest 3's BIOS call, cartridge and wake cases (idx
  1000 on) from a 3DS and a DSi, which agree on all 66; `compare.py RUN.csv
  --calls` checks a run of the `--calls` cartridge against them (per output
  unit, transfer or wake, not the calls' fixed parts).
- `compare.py RUN.csv` checks a run against `hw.csv`. `open.csv` lists the
  cases relift does not reproduce yet, with what it gives and the open
  question.

## Probes for the next console round

`gen.py OUT --probes` (`build.py OUT.nds --probes`, `make PROBES=1` in
`sdk/`) adds 17 ARM9 cases after all the others (idx 578 to 594). The
ROM the consoles ran is built without it, and its output is unchanged.
Each probe splits an open case (see the `PROBES` comment in `gen.py`):

- `p_ldm1` to `p_ldm8`, cold: when each word of a data cache line fill
  reaches the CPU (s_ldm8 cold);
- `p_ldr0_4` to `p_ldr0_28`, cold: a missed line's first word, then its
  word at that offset (copy cold);
- `p_copy_dtcm`, cold: copy with its stores to the data TCM;
- `p_stm8_pal_blank`: stm8 to palette in forced blank (stm8 pal);
- `hold_vs_code`, `hold_vs_poll` (idx 4100 to 4103, region p9 or p7 as
  the contention rows): the ARM9 times each uncached main RAM load while
  the ARM7 runs main RAM code (ADDs, or a loop polling IPCFIFOCNT) for
  about a million ticks: t16 is the loads it got in, t48 the longest time
  between two. It settles whether a gapless stream of the CPU EXMEMCNT
  bit 15 favors ever lets the other in (hwtest 3c shows only that it
  keeps it out for 8000 ticks and more).

All 17 run and print in melonDS (clang build); the devkitPro build with
`PROBES=1` builds in the pinned devkitPro image.

## Main RAM arbitration

`make ARB=1` in `sdk/` builds the arbitration ROM instead (`arb.c`, which
says what each row does; it writes `relift-arb-probe.csv`): e2e_nds's FIFO
program with the ARM7 polling from main RAM code, under each EXMEMCNT
bit 15 setting, with the ARM9's side in main RAM or in ITCM, a single
ARM9 load during the ARM7's poll, and the hold probes. On both consoles
(`arb.csv`) a gapless stream of the CPU main RAM favors keeps the other
out until it ends: at ARM7 priority the ARM9's load waits until the ARM7
gives up its poll, and e2e_nds's program hangs as written (7001).
`ARB_SPINS` (the ARM7's polls before it gives up) and `ARB_WAIT9` (the
ARM9's wait in ticks) shorten it for CI; `compare_arb.py RUN.csv` checks
a run.

`relift/tests/e2e_nds_timing.rs` builds the devkitPro ROM (with
`DEVKITARM` set; otherwise the clang one, checked with `compare.py
--clang`), lifts and runs it, and runs `compare.py`; a second test does
the same with the clang calls cartridge (`compare.py --calls`), a third
with the arbitration ROM (`compare_arb.py`).
