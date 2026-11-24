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
For the pointer authentication forms, the sail-arm model itself is an oracle:
`--oracle 'cmd:relift-cpu-arm64-gen oracle SAIL_LIB MODEL_SRC' --features
cortex-a78c --pac-states` runs each word in the model evaluated concretely
(`cpus/arm64/gen/src/oracle.rs`); `--pac-states` gives the authenticating forms
signed and tampered pointers (`src/pac.rs`).
`relift-diff-unicorn` is a Unicorn engine for `relift diff --engine`.

## A64 known answers

`relift-check kat record cpus/arm64/tests/kat` runs generated cases of every
liftable A64 encoding the Thor's CPU has (the Cortex-X3 cluster, Android, over
`adb shell` with `check/oracle/harness.c` built by `oracle/build.sh` and pushed
to `/data/local/tmp/a64kat/harness-linux`) and writes the hardware's answers
to `cpus/arm64/tests/kat/<class>.txt`, one case a line, format in
`cpus/arm64/tests/kat/format.rs`. Every case runs twice in two processes: the
pointer authentication code bits differ with the process keys and are masked,
and any other difference drops the case. `kat cross DIR` lists where Unicorn
disagrees with a recording (Unicorn is a cross-check, never a source).
`skipped.txt` names the encodings the Thor lacks and the features they need.
Two replays need no hardware and run in the `a64-kat` CI job:
`cpus/arm64/tests/kat.rs` (the `relift-ir` interpreter) and
`relift/tests/emit_a64_kat.rs` (emitted C compiled with clang, FPSR flags and
FPCR modes through the runtime). Take `~/.cache/relift/thor.lock` while recording.

## Folders

| Folder | Holds |
|---|---|
| `src` | the checker: A64 (`emu.rs`, `oracle.rs`, `mutate.rs`, `pac.rs`), `gb/`, `gba/` and `z80/` |
| `oracle` | the native A64 harness (`oracle/README.md`), the NanoBoyAdvance oracle (`oracle/nba/README.md`) and the ares VR4300 and Z80 oracles (`oracle/ares/README.md`) |
| `gba-ppu` | scene generator and comparison of the GBA picture unit with NanoBoyAdvance (`compare.py ORACLE SCENE.scn...`) |
| `tests` | the Unicorn comparison test |

NanoBoyAdvance is GPL-3.0-or-later. It is fetched at a pinned commit, built
as a separate process by `oracle/nba/build.sh` and never linked.

ares is ISC. It is fetched at a pinned commit, built as separate processes by
`oracle/ares/build.sh` (VR4300) and `oracle/ares/build-step.sh` (Z80) and never
linked. The SingleStepTests z80 data (MIT) and
the ZEX exerciser (GPL-2) are test data fetched by the caller, not part of the
repository.
