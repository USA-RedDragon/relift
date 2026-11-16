# relift architecture

relift recompiles a console game's machine code ahead of time into portable
C11 and builds it, with a C runtime that replaces the console's operating
system, into a native program for the host.

This file describes the **shape** of the workspace and the rules that hold it
in that shape. Every structural claim names the file or test that enforces
it. A rule with no test is marked as a convention. Section 0 is the short
list of hard rules; read it before changing anything.

---

## 0. Hard rules

- **No game data in the repo, ever.** No dumps, keys, extracted files,
  symbol lists, offsets or anything else derived from a game, and no game
  names. CI never sees game data; tests use `relift-testguest` and synthetic
  input.
- **Decrypted dumps only.** relift reads and ships decrypted inputs only (a
  `.nup` or an extracted `exefs`/`romfs` tree). It holds no key files, title
  keys or tickets, and it has no NCA, NSP, XCI or NCZ decryption; conversion
  of encrypted dumps lives in a separate tool that relift does not depend on.
  This is held by behavior and by names, not by a ban on crates: import
  refuses encrypted containers and encrypted headers, no source names key
  material or the NCA, ticket and BKTR structures, and no crate of the
  separate tool is in the graph. A crypto crate is allowed where its charter
  row states why (guest crypto instructions, SDK `nn::crypto` replacements,
  save data hashing). The tests are in section 2.
- **No GPL, AGPL or LGPL code.** Never copy from GPL projects (other
  recompilers, emulators, QEMU). MIT, Apache-2.0, BSD, ISC and 0BSD sources
  are fine with their notices kept. `cargo deny check` rejects GPL crates;
  GPL code may only live in the separate `check/` workspace.
- **The funnels.** `std::process::Command::new` goes through
  `relift_proc::command`; file writes (`fs::write`, `File::create`,
  `OpenOptions::open`, `fs::copy`, `io::copy`) go through `relift-cache`.
  clippy fails anything else, and `relift/tests/funnels.rs` fails a second
  opt-out.
- **The charter.** `relift/tests/workspace_charter.rs` lists every crate's
  band and exact dependencies, so a new crate or dependency needs a row
  edit. The core never reaches a CPU or a guest, CPUs reach only `core/ir`
  and `core/guest`, guests never reach each other, `override/` reaches
  nothing in the tool, and nothing reaches `check/` or a generator.
- **Generated code is committed.** `cpus/arm64/src/decode/generated.rs`
  (from AARCHMRS) and `cpus/arm64/src/lift/sail/generated/*.rs` (from sail-arm,
  one file per instruction class, `linguist-generated` in `.gitattributes`)
  and `cpus/sm83/src/lift/sail/generated.rs` (from our SM83 Sail model) are
  never edited by hand; `cpus/arm64/gen/README.md` and
  `cpus/sm83/gen/README.md` say how to regenerate them, and their notices sit next to the crate manifest. CI's
  `generated templates are current` job fetches the pinned Sail files,
  reruns both generators and fails on any difference. A form joins
  `cpus/arm64/gen/forms.txt` only after `relift-check forms` passes it and its
  mutation control on an oracle that implements its features. The ARMv4T
  tables and templates (`cpus/arm32/src/decode/generated.rs`,
  `cpus/arm32/src/lift/generated/{a32,t16}.rs`) come the same way from
  sail-arm's AArch32 part and the overlay `cpus/arm32/gen/armv4t.sail`
  (`cpus/arm32/gen/README.md`), which carries the ARM7TDMI rules where the
  Armv8 model differs, and the same CI job checks them. The VR4300 table
  and templates (`cpus/mips/src/vr4300/decode/generated.rs`,
  `cpus/mips/src/vr4300/lift/generated/*.rs`) come from sail-cheri-mips's MIPS
  model and the overlay `cpus/mips/gen/vr4300.sail`
  (`cpus/mips/gen/README.md`), which carries the VR4300 rules and all of
  COP1, with their notice next to the crate manifest; the same job checks
  them.
- **`unsafe` is forbidden** outside the roster in
  `relift/tests/unsafe_roster.rs`. Ceilings only fall.
- **The CI gate** is `cargo clippy --workspace --all-targets --all-features
  -- -D warnings`. Writes (`cargo fmt`, `clippy --fix`) are package-scoped
  with `-p`.

## 1. The crate graph

One Cargo workspace (`Cargo.toml`), `resolver = "2"`, edition 2024,
toolchain pinned exactly in `rust-toolchain.toml`. Crates are grouped in
folders by kind; package names stay flat (`relift-<area>`). `members` uses
globs (`core/*`, `cpus/*`, `guests/*/*`, `override/*`) plus `relift` and
`testguest`. No crate sits at the root of a folder that also holds nested
crates, because a glob such as `guests/*/*` would then match that crate's
`src/` and `cargo metadata` fails. So the Switch guest lives at
`guests/switch/guest`.

Read the graph bottom-up. Nothing in a lower band depends on a higher one.

| Band | Crate | Folder | Role |
|---|---|---|---|
| 0 | `relift-ir` | `core/ir` | Guest-neutral IR: values, ops, explicit flags, memory ops with width, endianness and ordering; cycle ticks, I/O accesses, sync points and code banks for hardware-register guests |
| 0 | `relift-sail` | `core/sail` | The shared Sail frontend of the template generators: parser, model tables and the staged evaluator that turns the semantics of one encoding into a lifting template body. A CPU implements `eval::Cpu` (the cut line, register setup) and `stage::Form` (its encodings and decode groups) in its generator crate; knows no CPU and no guest |
| 0 | `relift-cache` | `core/cache` | Content-addressed store, stage keys, and the one file writer |
| 0 | `relift-host` | `core/host` | The host target list that emit and build share |
| 0 | `relift-proc` | `core/proc` | The one subprocess runner |
| 0 | `relift-diff` | `core/diff` | The wire protocol, page cache and comparison of `relift diff`: one guest function run in a reference engine from a live process's state and compared with the lifted code's registers and pages; no CPU |
| 0 | `relift-progress` | `core/progress` | Progress output for `build` and `import`: live lines on a terminal, throttled plain lines elsewhere, color rules (`--color`, `NO_COLOR`, `TERM=dumb`), time from an injected clock |
| 1 | `relift-guest` | `core/guest` | The guest contract and vocabulary, including the `Naming` trait a guest implements for its symbol scheme |
| 2 | `relift-analyze` | `core/analyze` | Function boundaries, CFG, jump tables, indirect targets, generic over a CPU frontend |
| 2 | `relift-emit` | `core/emit` | IR to C, shards, `#line` and listings, memory strategy per host target |
| 2 | `relift-emit-wasm` | `core/emit-wasm` | IR to WebAssembly modules that share one memory and one funcref table (`docs/abi.md` section 15); not linked by `relift` yet; its tests run the modules in wasmtime against the IR interpreter |
| 2 | `relift-build` | `core/build` | Toolchain discovery and the Ninja writer |
| 2 | `relift-input` | `core/input` | The input model: ports, controls, `bind.<port>.<control>` bindings, extra ports from `game.toml` or a mod, the self-description `input` section and the C port declarations |
| 2 | `relift-manifest` | `core/manifest` | `game.toml` types: core keys plus a guest-owned section |
| 2 | `relift-cpu-arm64` | `cpus/arm64` | ARM64 decoder and lifter to `relift-ir`; not tied to one guest |
| 3 | `relift-cpu-arm64-gen` | `cpus/arm64/gen` | Generates the A64 lifting templates from the sail-arm model: the A64 cut and `Form` over `relift-sail`; run by hand, linked by nothing |
| 2 | `relift-cpu-arm32` | `cpus/arm32` | ARMv4T (Game Boy Advance ARM7TDMI) decoder and lifter to `relift-ir`, with templates generated from sail-arm AArch32 and an ARMv4T overlay, checked against console records |
| 3 | `relift-cpu-arm32-gen` | `cpus/arm32/gen` | Generates the ARMv4T decode tables and lifting templates from sail-arm and `cpus/arm32/gen/armv4t.sail`: the ARMv4T cut and `Form` over `relift-sail`; run by hand, linked by nothing |
| 2 | `relift-cpu-mips` | `cpus/mips` | VR4300 (Nintendo 64 CPU) decoder and lifter to `relift-ir`, with templates generated from sail-cheri-mips and a VR4300 overlay; a branch lifts with its delay slot |
| 3 | `relift-cpu-mips-gen` | `cpus/mips/gen` | Generates the VR4300 decode table and lifting templates from sail-cheri-mips and `cpus/mips/gen/vr4300.sail`: the VR4300 cut and `Form` over `relift-sail`; run by hand, linked by nothing |
| 3 | `relift-cpu-sm83-gen` | `cpus/sm83/gen` | Generates the SM83 lifting templates (`cpus/sm83/src/lift/sail/generated.rs`) from our Sail model `cpus/sm83/sail/sm83.sail`: the SM83 cut and `Form` over `relift-sail`; run by hand, linked by nothing |
| 2 | `relift-cpu-sm83` | `cpus/sm83` | SM83 (Game Boy and Game Boy Color) decoder, opcode table (length, M-cycles taken and not taken, flags, bus access per cycle) and lifter to `relift-ir`, with templates generated from the SM83 Sail model |
| 0 | `relift-info` | `core/info` | The self-description of a built binary: the `--relift-info` JSON document in sections (identity, needs, settings, input, mods, save), its embedding in the binary and its reader; read by `relift info` and by launchers |
| 0 | `relift-patch` | `core/patch` | Binary patch formats IPS, IPS32, UPS and BPS applied to a byte image, with the ranges each patch wrote |
| 2 | `relift-gb-formats` | `guests/gb/formats` | Game Boy and Game Boy Color cartridge format: header, mapper kinds, ROM and RAM size codes, both checksums, bank layout; no dependencies |
| 2 | `relift-gb-analyze` | `guests/gb/analyze` | Function discovery across mapper banks: code locations are (bank, address), bank-register writes are tracked along paths, bank-switch trampolines are found and resolved at their call sites, pointers stored in RAM and jump tables in ROM are followed along proven load and store paths, a computed jump that depends on one entry register is resolved by running the function for each of its 256 values, jumps into RAM are reported; decode and control flow only |
| 2 | `relift-gb` | `guests/gb/guest` | The Game Boy guest: the exact-mode SM83 frontend (every access through the device library, a sync point after every instruction, `EI`, `DI`, `HALT` and `STOP` on the library's CPU) and a ROM lifted into one unit per bank, with `program.c` |
| 2 | `relift-gb-tools` | `guests/gb/tools` | `relift-gbrom`: prints a ROM's header as text or JSON and validates logo, checksums and size; `functions` runs `relift-gb-analyze` |
| 2 | `relift-gba` | `guests/gba/guest` | The Game Boy Advance guest: the ARMv4T frontend (every access through the machine's bus, fetch and refill costs, SWI as a trap, a sync point after every instruction), and a ROM, discovered by `relift-gba-analyze`, lifted into one unit with its RAM copies, with `program.c` |
| 2 | `relift-gba-analyze` | `guests/gba/analyze` | Function discovery for ARM and Thumb code: runs the lifted IR of each instruction over abstract values, so the state of every instruction comes from the Thumb bit the code sets; follows calls, Thumb `BL` pairs, `BX` with known registers, returns through the saved link register, shared epilogues reached by `BL`, bounded jump tables, tables of code addresses in ROM, pointers stored in RAM and the IRQ vector; lifts ROM code copied to work RAM (`CpuSet`, `CpuFastSet`, DMA, calls run with constant arguments) at its run address; constants read are data, unknown targets are listed |
| 2 | `relift-gba-formats` | `guests/gba/formats` | Game Boy Advance cartridge format: header, complement check, logo hash, size and mirror rules with the open-bus read, save library strings, the multiboot entry; reads bytes only |
| 2 | `relift-gba-tools` | `guests/gba/tools` | `relift-gbarom`: prints a ROM header as text or JSON, validates logo hash, complement and size, lists the save library strings and the multiboot entry; `functions` runs `relift-gba-analyze`, as a cartridge or with `--multiboot` from work RAM |
| 2 | `relift-switch-formats` | `guests/switch/formats` | Decrypted Switch formats (romfs, NSO, NRO, NPDM, NUP) and the import of a `.nup` or an extracted dump tree |
| 2 | `relift-switch-tools` | `guests/switch/tools` | `relift-romfs`, `relift-nso`, `relift-npdm`, `relift-nro`, `relift-nup`: thin tools over the formats crate; `relift-ir` prints the IR of A64 words or NSO symbols |
| 2 | `relift-switch-vkgen` | `guests/switch/vkgen` | `relift-vkgen`: generates the Vulkan forwarding C in `runtime/guests/switch/vk/gen/` from the pinned `vk.xml`; standalone (nothing depends on it) |
| 2 | `relift-switch-names` | `guests/switch/names` | Switch symbol names as host names: Itanium names remangled into `guest::<module>`, the address fallback, and the reverse map |
| 2 | `relift-switch` | `guests/switch/guest` | The Switch guest, over its formats and `relift-cpu-arm64` |
| 2 | `relift-testguest` | `testguest` | Tiny synthetic guest the core is tested against |
| 3 | `relift` | `relift` | The binary: settings, command dispatch, guest registry |
| apart 0 | `relift-override-macros` | `override/macros` | Override attribute macros for game repos |
| apart 1 | `relift-override` | `override/api` | The Rust override API for game repos |

`guests/gb/devices` (`relift-gb-devices`) is also a separate Cargo workspace:
rustyboi-core's devices behind a C ABI, built as a static library by
`relift build` for Game Boy projects and linked into the program. The root
manifest excludes it and CI tests it on its own.

`check/` is a separate Cargo workspace holding `relift-check`, the
differential runner: it lifts A64 words, runs them in the `relift-ir`
interpreter and on an oracle on random states, and compares the
registers, flags, next PC and a data window; `--mutate` breaks the first
live operation of each lifted block and expects the comparison to catch it.
The oracles (`--oracle`, `check/src/oracle.rs`): Unicorn (GPL-2, linked,
every feature, the CI default), `qemu` and `qemu-max` (the native harness
`check/oracle/harness.c` under `qemu-aarch64 -cpu cortex-a57` or `max`,
run as a separate program) and `m2` (the same harness on an Apple M2 over
ssh). Words decode and lift for the oracle's feature set; `--undef` checks
that words of features the oracle lacks lift to an undefined-instruction
trap and fault there.
Nothing in the main workspace may link it; `relift check` runs it as a
subprocess. The root manifest excludes it. `--oracle rustyboi` swaps in a
Game Boy SM83 reference built on the rustyboi-core emulator (pinned by git
commit): `step` runs one instruction, `rom` a whole cartridge, and `lift`
compares lifted SM83 IR with it on random states (with a mutation control). `--oracle nba` is the GBA reference: the
NanoBoyAdvance core (GPL-3.0-or-later) built by `check/oracle/nba/build.sh`
from a pinned commit into a separate `nba-oracle` process, driven over a
line protocol on stdin and stdout (run cycles or frames with held keys,
single-step traces, registers, memory, I/O registers, frame and audio
hashes; see `check/oracle/nba/README.md`).

`relift-cpu-arm64` lifts each encoding with its Sail-generated template when it
has one, else with a hand-written lifter, always for a CPU feature set
(`decode::Features`, from the guest descriptor: `relift_switch::CPU_FEATURES`
is the Cortex-A57, Armv8.0-A plus AES, PMULL, SHA1, SHA256 and CRC32). The
decoder refuses encodings whose AARCHMRS feature condition fails, so they
lift to UNDEFINED as on that CPU; where the Sail semantics test a feature
the template tests `St::has` at lift time (one template for every level),
and a template that assumed a feature absent refuses CPUs that have it.
For the emitter, the cryptographic instructions lift to one `arm64.*`
intrinsic each (`lift/native.rs`), which the runtime runs on the host's own
AES, SHA and PMULL instructions when it has them (docs/abi.md 10.2). Templates that use the
floating-point cut assume FPCR zero and start with an `Op::Guard` that sends
any other FPCR to the runtime's slow path; `relift-check --fpcr` checks that
the guard trips (see `relift_ir::FpOp` for the contract). The hand-written lifters cover only
the forms the checker was brought up on, kept as a cross-check:
`cpus/arm64/tests/lift.rs` (`generated_templates_agree_with_the_hand_lifter`)
runs both in the interpreter.

`relift-emit-wasm` is the second emitter, for WebAssembly hosts. Like
`relift-emit` it knows no CPU: the frontend says how each intrinsic is
carried (`relift_guest::Frontend::wasm_intrinsic`). Its differential test
(`core/emit-wasm/tests/diff/`) emits random blocks over every IR operation,
section 13 blocks over a Game Boy-like I/O map, and random multi-function
programs in two modules (with and without tail calls), runs them in
wasmtime, compares them with `relift_ir::interp`, and breaks one operation
per block as a mutation control. The default sample takes seconds;
`RELIFT_WASM_CHECK=full` runs the full sizes and `RELIFT_WASM_SEED` picks
another seed. wasmtime is a dev-dependency of that crate only.

There is no core names crate: Itanium remangling is the Switch guest's
business (`relift-switch-names`), and the core sees only
`relift_guest::Naming`.

The C runtime sits outside Cargo, built and tested by `runtime/Makefile`
(see `runtime/README.md`), in three layers that mirror the Rust folders:

| Layer | Folder | Owns |
|---|---|---|
| core | `runtime/core` | Guest memory with its own permission flags, faults at guest addresses, host threads with an opaque CPU context, the clock, dispatch, logging and configuration; knows no guest OS and no guest CPU |
| CPU | `runtime/cpus/arm64` | The ARM64 lifted-code ABI and `rl_ctx`, FPCR, WFE, CNTPCT, `rl_undefined`, `rl_resume`, register summaries for reports |
| guest | `runtime/guests/switch` | Horizon: kernel, `rl_svc`, IPC, `sm:` and the services, the scheduler, the NPDM layout, Horizon's page, clock and permission encodings; `switch/vk` forwards Vulkan to the host |
| CPU | `runtime/cpus/sm83` | The SM83 lifted-code ABI and `rl_ctx`, interrupts, HALT and STOP, over a callback table |
| guest | `runtime/guests/gb` | The Game Boy memory map, mappers and I/O routed to scheduler device slots; `gb/exact` is the program that runs lifted code with the device library |
| CPU | `runtime/cpus/arm32` | The ARMv4T lifted-code ABI and `rl_ctx`, mode banks, exceptions, the I bit's mask, the return address stack |
| guest | `runtime/guests/gba` | The Game Boy Advance machine, picture unit and high-level BIOS; `gba/run` is the program that runs lifted code on it |

The layers above tell the core their page size, address width, tick rate,
context layout, code granule and byte order through two descriptor structs
at start-up. Each layer builds with only its own include paths and those
below it, and each layer's tests link nothing above it. The public override
header (`include/relift/override.h`) does not exist yet.

The contract between emitted C and that runtime is
`runtime/cpus/arm64/include/relift/arm64/abi.h`, over the CPU-neutral
`runtime/core/include/relift/abi_core.h`; `docs/abi.md` gives the reason for
each choice.

## 2. Structural rules and the tests that enforce them

All of these run in `cargo test` and in CI's checks job.

| Rule | Enforced by |
|---|---|
| Every member has a charter row with a band and an exact list of declared normal, dev and build dependencies | `relift/tests/workspace_charter.rs`: `every_member_has_a_charter_row`, `every_declared_ceiling_holds` |
| No member declares a member in a higher band, by any kind of edge | `workspace_charter.rs`: `the_band_order_holds` |
| Nothing under `core/` reaches anything under `cpus/`, `guests/` or `testguest/` by any route | `workspace_charter.rs`: `core_reaches_no_cpu_and_no_guest` |
| `cpus/` crates reach only `core/ir` and `core/guest` | `workspace_charter.rs`: `cpus_reach_only_ir_and_guest` |
| No guest reaches another guest (each `guests/<name>/` is one guest, `testguest/` another) | `workspace_charter.rs`: `no_guest_reaches_another_guest` |
| `override/` crates reach no tool crate | `workspace_charter.rs`: `override_reaches_no_tool_crate` |
| Nothing reaches `check/`; it stays its own workspace and is excluded | `workspace_charter.rs`: `nothing_reaches_check` |
| No crate of the separate conversion tool (`switch-formats`, `switch-io`, `switch-tools`) is in the resolved graph or declared by a member; controls: `sha2`, `digest` and `relift-switch-formats` are found. Crypto crates are not banned: a charter row states why one is allowed | `workspace_charter.rs`: `no_crate_of_the_separate_tool_is_in_the_graph` |
| No workspace source names key files or key material (`prod.keys`, `title.keys`, title keys, header keys, key area keys, `titlekek`, `eticket`, master keys) or the NCA, ticket and BKTR structures; the allow list is empty; controls: a fixture built at run time with each kind of word is found, a clean one is not | `relift/tests/decrypted_only.rs`: `no_workspace_source_handles_key_material`, `the_scan_finds_planted_words`, `the_allow_list_is_empty_or_names_real_files` |
| Import refuses NSP, NSZ, XCI, XCZ, NCA and NCZ by extension and by magic, an NCA with an encrypted header, and the removed `--keys` setting | `guests/switch/formats/tests/import.rs`: `encrypted_inputs_are_refused`, `an_encrypted_header_is_refused_without_a_key`; `relift/tests/cli.rs`: `import_refuses_encrypted_dumps_pointing_to_a_separate_tool` |
| No member reaches a generator (`cpus/*/gen`) | `workspace_charter.rs`: `no_member_reaches_a_generator` |
| No member links a test-only package (`wasmtime`, `cranelift-codegen`) through normal or build edges; controls: `relift-emit-wasm`'s tests reach both and its build reaches `wasm-encoder` | `workspace_charter.rs`: `test_only_packages_stay_out_of_every_build` |
| The charter reads this tree and its walk recurses | `workspace_charter.rs`: `the_reader_reads_this_workspace` |
| `unsafe_code = "forbid"` for the workspace; every member inherits it or is on the roster with its own `deny` pair | `relift/tests/unsafe_roster.rs`: `the_workspace_lint_table_forbids_unsafe_code`, `every_member_inherits_the_forbid_or_is_rostered` |
| No `unsafe` outside the roster; each rostered crate reads exactly its ceiling | `unsafe_roster.rs`: `no_unrostered_member_spells_unsafe`, `each_rostered_crate_holds_its_unsafe_ceiling` |
| `std::process::Command::new` only in `relift-proc`; `fs::write`, `File::create`, `OpenOptions::open`, `fs::copy`, `io::copy` only in `relift-cache` | `clippy.toml` (clippy, `-D warnings` in CI) |
| The funnel opt-out appears once per owning crate and nowhere else; no crate has its own `clippy.toml` | `relift/tests/funnels.rs` |
| No GPL, AGPL or LGPL crate; crates.io only | `deny.toml` (`cargo deny check` in CI) |
| The generated config table in `README.md` and `relift.example.toml` match the settings struct | CI step "generated config docs are current"; `relift/tests/cli.rs`: `the_example_config_loads` |
| The C runtime core (`runtime/core`) includes no header of `runtime/cpus` or `runtime/guests` and spells no guest OS or CPU word outside `runtime/charter/allow.txt` | `runtime/charter/check.py` (`make -C runtime charter`, the Runtime workflow's charter job); each layer's test binary links nothing above it |

The cut edges are folder rules read from each member's manifest path, so a
new crate under `core/` or `guests/` is covered without editing the test.
They walk the resolved graph from `cargo metadata` over every target: the
first hop may be any kind (a crate's tests compile what its dev-dependencies
reach), later hops are normal or build. A direct-name check would miss an
indirect route. Each absence rule asserts a presence control, so an empty
walk cannot pass.

Negative control: adding `relift-switch` as a dev-dependency of `relift-ir`
fails `every_declared_ceiling_holds`, `the_band_order_holds`,
`core_reaches_no_cpu_and_no_guest` (with the route
`relift-ir -> (dev) relift-switch -> (normal) relift-cpu-arm64`) and
`the_reader_reads_this_workspace`. As a normal dependency Cargo refuses it
outright as a cycle.

### Ratchets

A ceiling equals the count it measures. The change that sheds an occurrence
lowers the constant in the same commit; a ceiling is never raised without a
written plan amendment, and a needle is never hidden by re-spelling it.
`relift-override` holds its roster seat at a ceiling of 0 because overrides
will export symbols with `#[unsafe(export_name)]`, which `forbid` rejects.

### The funnels

`clippy.toml` disallows the subprocess and file-write APIs everywhere. The
owning crate's single funnel function opts out with
`#[expect(clippy::disallowed_methods, reason = ...)]`: `relift_proc::command`
and `relift_cache::write_if_changed`. `expect` rather than `allow`, so a
funnel that stops calling the API fails the lint. Tests use the funnels too.

## 3. Conventions (not enforced by a test)

- Every registry dependency is pinned exactly (`=x.y.z`) in
  `[workspace.dependencies]`; members write `{ workspace = true }`. Patched
  upstream crates go in `vendor/` behind `[patch.crates-io]`.
- `cfg(target_*)` may select a value, dependency or type alias, never fork
  behavior inside a function body.
- No comments by default; rustdoc instead. At most six `//` comments per
  crate (`// SAFETY:` exempt). ASCII only.
- `cargo fmt` and clippy fixes are package-scoped (`-p`); only checks run
  workspace-wide.
- No game data, keys or anything derived from them (symbol lists included)
  in this repository. CI runs synthetic equivalents.

## 4. Builds and CI

`.github/workflows/ci.yaml`, GitHub-hosted runners only:

Every job sets `timeout-minutes: 15`; a job that needs longer is split or
sped up, never given more time.

- **checks** (ubuntu-latest, the pinned toolchain through rustup): fmt,
  clippy `--all-targets --all-features -D warnings` (main and `check/`
  workspaces), the structural tests, all tests, the generated config docs, the
  generated Vulkan forwarding (from the pinned `vk.xml`), `cargo deny check`.
- **gb-corpus**: builds `relift` and runs the cases of
  `runtime/guests/gb/devtests/ci_cases.toml` through `relift import`,
  `build` and the built program (`ci_corpus.py`; the ROMs are fetched from a
  pinned public release and are never in the repository).
- **generated**: fetches the pinned Sail sources, reruns the A64, SM83 and
  ARMv4T template generators and fails on any difference.
- **macos** (macos-latest): all tests.
- **Runtime** (`runtime.yaml`): the charter check, the generated Vulkan
  types on six host targets, and the C runtime tests on Linux and macOS.
- **cross**: `relift` built for `x86_64-unknown-linux-gnu`,
  `aarch64-unknown-linux-gnu`, `x86_64-pc-windows-gnullvm`,
  `aarch64-pc-windows-gnullvm`, `aarch64-apple-darwin` and
  `x86_64-apple-darwin` inside `ghcr.io/usa-reddragon/rust-cross`, pinned by
  digest. The job frees disk first and then uses `docker run`, since a
  job-level `container:` is pulled before any step can free space. It fails
  if the image's `RUST_VERSION` differs from `rust-toolchain.toml`.

Profiles: dev with `debug = "line-tables-only"` and dependencies at
`opt-level = 2`; release with `lto = true`, `codegen-units = 1`,
`panic = "abort"`, `strip = true`.

## 5. Before landing

```sh
cargo fmt --all --check
cargo clippy --workspace --all-targets --all-features -- -D warnings
cargo test --workspace
cargo deny check
```

`pre-commit install --hook-type pre-commit --hook-type pre-push` turns on the
hooks in `.pre-commit-config.yaml`: on commit, rustfmt and the generated
config docs; on push, the clippy gate.
