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
- **Decrypted dumps only.** relift reads decrypted dumps only (a `.nup` or
  an extracted `exefs`/`romfs` tree); there are no keys or crypto code in
  this repo, and conversion of encrypted dumps lives in a separate tool.
  `relift/tests/workspace_charter.rs` (`no_crypto_crate_is_in_the_graph`)
  fails any cipher crate in the graph.
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
- **Generated code is committed.** `cpus/a64/src/decode/generated.rs`
  (from AARCHMRS) and `cpus/a64/src/lift/sail/generated/*.rs` (from sail-arm,
  one file per instruction class, `linguist-generated` in `.gitattributes`)
  are never edited by hand; `cpus/a64/gen/README.md` says how to regenerate
  them, and their notices sit next to the crate manifest. CI's
  `generated templates are current` job fetches the pinned sail-arm files,
  reruns the generator and fails on any difference. A form joins
  `cpus/a64/gen/forms.txt` only after `relift-check forms` passes it and its
  mutation control.
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
| 0 | `relift-ir` | `core/ir` | Guest-neutral IR: values, ops, explicit flags, memory ops with width, endianness and ordering |
| 0 | `relift-cache` | `core/cache` | Content-addressed store, stage keys, and the one file writer |
| 0 | `relift-host` | `core/host` | The host target list that emit and build share |
| 0 | `relift-proc` | `core/proc` | The one subprocess runner |
| 1 | `relift-guest` | `core/guest` | The guest contract and vocabulary, including the `Naming` trait a guest implements for its symbol scheme |
| 2 | `relift-analyze` | `core/analyze` | Function boundaries, CFG, jump tables, indirect targets, generic over a CPU frontend |
| 2 | `relift-emit` | `core/emit` | IR to C, shards, `#line` and listings, memory strategy per host target |
| 2 | `relift-build` | `core/build` | Toolchain discovery and the Ninja writer |
| 2 | `relift-manifest` | `core/manifest` | `game.toml` types: core keys plus a guest-owned section |
| 2 | `relift-cpu-a64` | `cpus/a64` | ARM64 decoder and lifter to `relift-ir`; not tied to one guest |
| 3 | `relift-cpu-a64-gen` | `cpus/a64/gen` | Generates the A64 lifting templates from the sail-arm model; run by hand, linked by nothing |
| 2 | `relift-switch-formats` | `guests/switch/formats` | Decrypted Switch formats (romfs, NSO, NRO, NPDM, NUP) and the import of a `.nup` or an extracted dump tree |
| 2 | `relift-switch-tools` | `guests/switch/tools` | `relift-romfs`, `relift-nso`, `relift-npdm`, `relift-nro`, `relift-nup`: thin tools over the formats crate; `relift-ir` prints the IR of A64 words or NSO symbols |
| 2 | `relift-switch-vkgen` | `guests/switch/vkgen` | `relift-vkgen`: generates the Vulkan forwarding C in `runtime/guests/switch/vk/gen/` from the pinned `vk.xml`; standalone (nothing depends on it) |
| 2 | `relift-switch-names` | `guests/switch/names` | Switch symbol names as host names: Itanium names remangled into `guest::<module>`, the address fallback, and the reverse map |
| 2 | `relift-switch` | `guests/switch/guest` | The Switch guest, over its formats and `relift-cpu-a64` |
| 2 | `relift-testguest` | `testguest` | Tiny synthetic guest the core is tested against |
| 3 | `relift` | `relift` | The binary: settings, command dispatch, guest registry |
| apart 0 | `relift-override-macros` | `override/macros` | Override attribute macros for game repos |
| apart 1 | `relift-override` | `override/api` | The Rust override API for game repos |

`check/` is a separate Cargo workspace holding `relift-check`, the
differential runner: it lifts A64 words, runs them in the `relift-ir`
interpreter and in Unicorn (GPL-2) on random states, and compares the
registers, flags, next PC and a data window; `--mutate` breaks the first
live operation of each lifted block and expects the comparison to catch it.
Nothing in the main workspace may link it; `relift check` runs it as a
subprocess. The root manifest excludes it.

`relift-cpu-a64` lifts each encoding with its Sail-generated template when it
has one, else with a hand-written lifter. Templates that use the
floating-point cut assume FPCR zero and start with an `Op::Guard` that sends
any other FPCR to the runtime's slow path; `relift-check --fpcr` checks that
the guard trips (see `relift_ir::FpOp` for the contract). The hand-written lifters cover only
the forms the checker was brought up on, kept as a cross-check:
`cpus/a64/tests/lift.rs` (`generated_templates_agree_with_the_hand_lifter`)
runs both in the interpreter.

There is no core names crate: Itanium remangling is the Switch guest's
business (`relift-switch-names`), and the core sees only
`relift_guest::Naming`.

The C runtime sits outside Cargo, built and tested by `runtime/Makefile`
(see `runtime/README.md`), in three layers that mirror the Rust folders:

| Layer | Folder | Owns |
|---|---|---|
| core | `runtime/core` | Guest memory with its own permission flags, faults at guest addresses, host threads with an opaque CPU context, the clock, dispatch, logging and configuration; knows no guest OS and no guest CPU |
| CPU | `runtime/cpus/a64` | The ARM64 lifted-code ABI and `rl_ctx`, FPCR, WFE, CNTPCT, `rl_undefined`, `rl_resume`, register summaries for reports |
| guest | `runtime/guests/switch` | Horizon: kernel, `rl_svc`, IPC, `sm:`, the NPDM layout, Horizon's page, clock and permission encodings |

The layers above tell the core their page size, address width, tick rate,
context layout, code granule and byte order through two descriptor structs
at start-up. Each layer builds with only its own include paths and those
below it, and each layer's tests link nothing above it. The public override
header (`include/relift/override.h`) does not exist yet.

The contract between emitted C and that runtime is
`runtime/cpus/a64/include/relift/a64/abi.h`, over the CPU-neutral
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
| No cipher crate (`aes`, `ctr`, `xts-mode`, `cipher`, ...) is anywhere in the resolved graph; control: `sha2` and `digest` are found | `workspace_charter.rs`: `no_crypto_crate_is_in_the_graph` |
| No member reaches a generator (`cpus/*/gen`) | `workspace_charter.rs`: `no_member_reaches_a_generator` |
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
`relift-ir -> (dev) relift-switch -> (normal) relift-cpu-a64`) and
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
  workspaces), the structural tests, all tests, the generated config docs,
  `cargo deny check`.
- **macos** (macos-latest): all tests.
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
