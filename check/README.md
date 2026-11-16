# check

`relift-check`, the differential checker. It is a Cargo workspace of its own:
it links GPL-2 Unicorn, so nothing in the main workspace may depend on it, and
the root manifest excludes it. `relift check` is not implemented; run
`relift-check` directly.

Build and test:

```sh
cargo build --release --manifest-path check/Cargo.toml
cargo test --manifest-path check/Cargo.toml --release
```

The Unicorn tests need no other tool. Tests that use an external oracle skip
when it is missing.

## What it checks

| Guest | Command | Compares |
|---|---|---|
| A64 | `relift-check insn <hex>` or `forms <name>\|all` | lifted words, run on the IR interpreter, against an oracle on random states: registers, flags, next PC and a data window; `--mutate` breaks the first live operation of each block and expects a mismatch |
| SM83 | `relift-check --oracle rustyboi step\|lift\|rom` | one instruction, lifted IR on random states, or a whole ROM against rustyboi-core |
| Z80 | `relift-check --oracle sst-z80 run DIR`, `--oracle zex FILE.com`, `--oracle ares-z80 run` | every instruction template on the IR interpreter against SingleStepTests/z80 v1 (registers, WZ, Q, RAM, ports, T-states and every bus access), ZEXDOC and ZEXALL, and random states and interrupt entries against ares; `--mutate` breaks every lifted block and expects the run to catch it |
| ARMv4T | `relift-check --oracle nba rom FILE` | a ROM run on NanoBoyAdvance: registers, traces, I/O registers, frame and audio hashes, saves |

`relift-check` with no arguments prints the full usage. The A64 oracles are
Unicorn (the default, linked), QEMU user mode, QEMU `max` and an Apple M2
over ssh; `oracle/README.md` explains the native harness and how to build it.
`relift-diff-unicorn` is a Unicorn engine for `relift diff --engine`.

## Folders

| Folder | Holds |
|---|---|
| `src` | the checker: A64 (`emu.rs`, `oracle.rs`, `mutate.rs`), `gb/`, `gba/` and `z80/` |
| `oracle` | the native A64 harness (`oracle/README.md`), the NanoBoyAdvance oracle (`oracle/nba/README.md`) and the ares Z80 oracle (`oracle/ares/README.md`) |
| `gba-ppu` | scene generator and comparison of the GBA picture unit with NanoBoyAdvance (`compare.py ORACLE SCENE.scn...`) |
| `tests` | the Unicorn comparison test |

NanoBoyAdvance is GPL-3.0-or-later. It is fetched at a pinned commit, built
as a separate process by `oracle/nba/build.sh` and never linked.

ares is ISC. It is fetched at a pinned commit, built as a separate process by
`oracle/ares/build.sh` and never linked. The SingleStepTests z80 data (MIT) and
the ZEX exerciser (GPL-2) are test data fetched by the caller, not part of the
repository.
