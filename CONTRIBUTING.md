# Contributing to relift

relift is a static recompiler under the MIT license. It translates a console game's machine code ahead of time into portable C11, which clang or `zig cc` builds, with a C runtime that stands in for the console, into a native program. The core is host-agnostic; the tool is Rust and the runtime is C.

Outside contributions are welcome. This page covers how to build, how to check a change, the rules, and how a change lands. If you work with an AI coding agent, point it at `AGENTS.md` as well. To find something to work on, read [the gaps list](GAPS.md).

## Before you start

Open an issue before you write code, especially for anything in [GAPS.md](GAPS.md). Several areas have active work, and a short thread saves you a rebase against a change you did not know about. A small fix (a typo, a test, a doc correction) needs no issue; send the PR.

Read [Architecture](docs/concepts/architecture.md). Section 0 lists the hard rules, section 2 the tests that enforce the structure, and section 4 the CI jobs.

## Build and run

You need the Rust toolchain that `rust-toolchain.toml` pins (rustup installs it on the first build), clang or `zig cc` 0.17.1 for the generated C, and ninja. SDL3, libopus and a Vulkan loader are optional; without SDL3 a program runs headless. [Install](docs/start/01-install.md) has the details.

```sh
git clone https://github.com/USA-RedDragon/relift.git
cd relift
cargo build --release -p relift -p relift-switch-tools -p relift-gb-tools -p relift-gba-tools
export PATH="$PWD/target/release:$PATH"
mkdir mygame && cd mygame
relift import ~/dumps/game.gbc   # a ROM or decrypted dump that you own
relift build                     # a shipped build
relift run                       # a dev build, run on this host
```

relift never reads encrypted dumps and ships no game data. Use a dump you own, decrypted. [Make your first game](docs/start/02-first-game.md) goes through the commands, and [Use relift](docs/guides/usage.md) covers stop reports for code that discovery missed.

The C runtime builds and tests on its own:

```sh
make -C runtime -j2 test
make -C runtime charter
```

Any test or headless run you start sets `RELIFT_AUDIO=null`, so a CI machine or a laptop does not open an audio device.

## Check a change

`scripts/ci/all.sh` runs every CI job that your machine can run, in parallel, with the same scripts the workflows call, and prints each job's time. Give it job names (or shell patterns such as `cross-*`) to run only those:

```sh
scripts/ci/all.sh --help
scripts/ci/all.sh lint tests-workspace
CI_WORK=$HOME/ci-work scripts/ci/all.sh docs
```

`CI_WORK` is where jobs fetch pinned downloads (by commit and SHA-256) and keep scratch files; it defaults to a folder under your cache directory. A job that runs past its workflow's `timeout-minutes` is killed and fails.

| Job | Checks |
|---|---|
| `lint` | rustfmt and clippy (three Cargo workspaces), the charter tests, the config docs and Vulkan forwarding are current, `cargo deny` |
| `tests-workspace`, `tests-gb-devices`, `tests-relift`, `tests-relift-e2e` | Every crate's tests; the end-to-end tests build and run synthetic programs |
| `generated` | Reruns the Sail template generators on pinned sources and fails on any difference |
| `docs` | docgen facts are current and the docs site builds with no broken link |
| `workflows` | Workflow files parse and lint |
| `runtime-charter`, `runtime-tests`, `release-guard` | The runtime's layer rules, its C tests, and that release builds load no code |
| `gb-corpus`, `z80-sst`, `z80-zexdoc` | Public test ROMs and known-answer sets, fetched by hash, through relift |
| `mesh-kat` | Shader known answers on lavapipe |
| `appimage` | The Linux AppImage packaging |
| `cross-<guest>-<target>` | Builds the runtime and a test program for one host target (see below) |

Some jobs need a host you may not have:

- `mac-*` (the macOS tests, the macOS and iOS cross builds, relift for both Apple targets) run over ssh on a Mac. `all.sh` copies your tree to the host named in `CI_MAC` (an ssh host). With `CI_MAC` empty or the host unreachable they are reported as skipped. Without a Mac, rely on the PR's CI.
- `cross-*` jobs need zig 0.17.1 for Windows targets, the Android NDK (`ANDROID_NDK_HOME`) for Android, and wine to run the Windows x86-64 program. A missing tool stops the job and names what to install.
- `image-*` jobs build `relift` for four targets inside the pinned `rust-cross` image. They need docker and about 16 GB of disk, and are skipped without them.
- `nds-timing-sdk` needs docker and the pinned devkitPro image.

A skipped job never fails `all.sh`, but it is always printed with its reason. Say in the PR which jobs you could not run. CI runs on GitHub-hosted runners for every PR.

The checks you should always run by hand before a PR:

```sh
cargo fmt -p <crate>                 # writes are package-scoped, never --all
cargo clippy --workspace --all-targets --all-features --locked -- -D warnings
cargo test -p <crate>
```

## Pre-commit hooks

```sh
pre-commit install --hook-type pre-commit --hook-type pre-push
```

On commit the hooks run rustfmt and regenerate `relift.example.toml` from the settings struct; on push they run the clippy gate. Do not skip them with `--no-verify`; fix what they report.

## Commits

- Subject of 72 characters or fewer, in the imperative, prefixed by the area: `switch:`, `gpus/sm86:`, `cpus/arm32:`, `runtime:`, `ci:`, `docs:`.
- One commit per logical change. Each commit builds and passes the checks alone, so a reviewer can revert one without the rest.
- No merge commits; rebase onto `main`.
- No AI attribution and no `Co-Authored-By` trailers.
- Explain why in the body when the reason is not obvious from the diff.

## Pull requests

Open PRs on GitHub against `main`. `main` moves fast, so rebase before you push and expect to rebase again. The maintainer lands accepted work in batches, rebased onto `main`, so the commit hashes in your branch will change and your branch may be closed as landed rather than merged. That is normal.

A PR should say what it changes, how you verified it (the jobs you ran, the test ROMs or oracle comparisons), and which jobs you could not run. Keep PRs small. A change that needs a new crate, a new dependency or a new `unsafe` needs a charter or roster edit in the same PR; the tests in `relift/tests` tell you which.

## The hard rules

[Architecture section 0](docs/concepts/architecture.md) is the list; read it rather than a copy here. In short for outsiders:

- **Exact behavior.** relift reproduces the console, not a game. No per-game hacks, hints, tables or annotations: find and fix the general cause.
- **Zero interpreted guest instructions at run time.** When discovery misses code, the program stops with a named report; it never falls back to an interpreter. Never invent a value a game uses. If you infer a value, label it `inferred` in its doc comment and cite the source (a datasheet, a hardware test, a public document).
- **No game names** in code, tests, commits or docs, and no game data (see below).
- **No code comments** except doc comments and a comment on a genuinely confusing detail. No lint or build-check suppression: fix the cause.
- **Funnels and charter.** Subprocesses go through `relift_proc::command`, file writes through `relift-cache`, and `unsafe` only in rostered crates.
- **Host rules.** No JIT, no writable and executable memory, no `dlopen`-only path in a release build. iOS must stay possible even though it is the lowest priority.
- **Every CI job finishes in under 15 minutes** (`timeout-minutes: 15`). Split or speed up a slow job.
- **Containers** use `FROM scratch` or Alpine. Use zstd for compression unless a tool cannot read it.
- **Units.** Never mix MiB/GiB with MB/GB.

## Documentation

The docs site builds from the README, the `docs` folder and the `README.md` of each folder that [the site outline](docs/nav.md) lists. A page that is in `docs/` but not in the outline, or in the outline but not written, fails the `docs` job, as does any broken link or heading anchor.

Facts that come from the repository (a settings table, a crate table, a file excerpt, a command's usage) sit inside docgen markers, `<!-- docgen:... -->`. Never edit the text inside a marker by hand. Change the source and run:

```sh
go run github.com/USA-RedDragon/docgen@v0.1.0 update
go run github.com/USA-RedDragon/docgen@v0.1.0 check -diff
```

Write plain US English, with no em-dashes and no filler words. Describe code by what it does. A folder's `README.md` says what the folder holds and how to test it; update it when your change makes it wrong. [Tooling and checks](docs/project/tooling.md) has more on the docs build.

## Licensing

relift is MIT. Contributions are under the same license.

- Third-party code may enter the repository only under an MIT, BSD, ISC, Apache-2.0 or zlib-style license, with its copyright notice and license text kept (a `NOTICE` file next to the code, or the header of the ported file). `cargo deny check` enforces the allowed list in `deny.toml` for crates: MIT, Apache-2.0, BSD-2-Clause, BSD-3-Clause, ISC, 0BSD, Zlib, Unicode-3.0.
- **Never copy GPL, AGPL or LGPL code**, in any form, including translated to another language. You may read a GPL project for facts (how a register behaves, how long an access takes) and write your own code from those facts. GPL emulators may run as separate programs in the `check/` workspace as differential oracles; nothing in the main workspace may link them.
- Port from permissive sources where they exist (for example device behavior from MIT, BSD and ISC emulators) and keep the notice per ported file. Credit a project in a notice only for code or tables you took from it.
- A new dependency needs a license check (`cargo deny check`), an exact pinned version (`=x.y.z`) in `[workspace.dependencies]`, a hash for any download, and a row in the charter. Prefer an existing, well-tested library over writing your own, within these rules.

See [License](docs/project/license.md).

## Data rules

No game data enters this repository, ever: no ROMs, cartridge or disc dumps, firmware or BIOS images, keys, tickets, extracted files, symbol lists, offsets, or anything derived from a game. relift reads decrypted inputs only and holds no key material; tests enforce both.

Tests use public test ROMs and synthetic programs:

- Public test ROMs (from open test suites or homebrew with a license that allows it) are fetched at test time by commit and SHA-256 into the CI work dir. They are never committed.
- Synthetic programs are built at test time (`synth` in each guest's tests, `relift-testguest`).
- To compare with a console behavior, use a public hardware test, a datasheet or a public document, and cite it. Do not paste console output you took from a game.

If you are unsure whether something counts as game data, leave it out and ask in the issue.

## Picking work

[GAPS.md](GAPS.md) is a menu of open work, grouped by area, with a size, where it lives in the tree, what to read first and how to verify it. Entries marked "good first issue" need little context. Areas under heavy active work are listed there as "coordinate first". Open an issue naming the gap before you start.

## Where things live

| Looking for | Go to |
|---|---|
| The crate graph, rules and CI | [Architecture](docs/concepts/architecture.md) |
| The C runtime, layers, settings, environment | [The C runtime](runtime/README.md) |
| The contract between emitted C and the runtime | [Runtime ABI contract](docs/abi.md) |
| A guest | `guests/<name>/README.md` |
| Commands and the cache | [Use relift](docs/guides/usage.md) |
| Hooks, `all.sh` and the docs build | [Tooling and checks](docs/project/tooling.md) |
