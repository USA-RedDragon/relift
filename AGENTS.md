# AGENTS.md

Instructions for AI coding agents working in this repository. Humans: read `CONTRIBUTING.md`. The hard rules are in `docs/concepts/architecture.md` section 0; read it before you change anything.

relift is a static recompiler: guest machine code becomes portable C11, built with clang or `zig cc` and linked with a C runtime. The tool is Rust; the runtime is C. Guests: Switch, Switch 2, GB/GBC, GBA, N64, NDS, SMS/GG/SG-1000. Hosts: Linux, macOS, Windows, Android, iOS.

## Layout

| Path | Holds |
|---|---|
| `core/*` | Guest-neutral crates: IR, emitters, analysis, cache, build, input, manifest. Knows no CPU and no guest |
| `cpus/*` | One crate per CPU (arm64, arm32, mips, rsp, sm83, z80): decoder and lifter to the IR. `cpus/*/gen` are generators run by hand |
| `gpus/*` | Shader translators (`sm86`, `sm53`) to SPIR-V; `apigen`, the GPU-API forwarding generator |
| `guests/<name>/{formats,analyze,guest,tools}` | One guest: file formats, code discovery, frontend and lifting, small tools. `guests/switch` also has `names`, `shaders` |
| `runtime/{core,cpus,guests,host}` | The C runtime in three layers plus optional host backends. Built by `runtime/Makefile` |
| `relift` | The `relift` binary and its end-to-end tests |
| `check` | Separate Cargo workspace: the differential checker and external oracles |
| `testguest`, `override` | A synthetic guest for core tests; the override API |
| `scripts/ci` | One script per CI job; `all.sh` runs them |
| `docs` | Pages of the docs site; `docs/nav.md` is the outline |

A crate's band and exact dependencies are in `relift/tests/workspace_charter.rs`. A new crate or dependency needs a row there.

## Before you say done

1. `cargo fmt -p <crate>` for each crate you touched (never `--all` to write; `--all --check` to verify).
2. `cargo clippy --workspace --all-targets --all-features --locked -- -D warnings`.
3. `cargo test -p <crate>` for each crate you touched, then `scripts/ci/all.sh tests-workspace tests-relift "tests-relift-e2e-*" lint` when the change is wide.
4. Runtime C changes: `make -C runtime -j2 test` and `make -C runtime charter`.
5. Generated code (`cpus/*/gen`): `scripts/ci/all.sh generated`.
6. Docs: `go run github.com/USA-RedDragon/docgen@v0.1.0 update`, then `check -diff`, then `scripts/ci/all.sh docs`.
7. A new dependency: `cargo deny check` (`scripts/ci/all.sh lint` runs it).
8. Every test or headless run you start sets `RELIFT_AUDIO=null`.

Report what you ran and what it returned. If a check needs a host you do not have (`mac-*`, `image-*`, `nds-timing-sdk`, Android or iOS devices), say so; do not claim it passed.

## House rules

- Correctness first: exact console behavior. No per-game hacks, hints, tables or annotations. Fix the general cause.
- Zero interpreted guest instructions at run time. Code that discovery misses stops the program with a named report. Never invent a value a game uses. Label an inferred value as inferred and cite its source in the doc comment.
- No game names in code, tests, commits or docs. No game data of any kind in the repository: no ROMs, dumps, keys, BIOS images, symbol lists, offsets or text taken from a game.
- Tests use public test ROMs fetched by commit and SHA-256 into the CI work dir, or synthetic programs built at test time (`synth`, `relift-testguest`).
- No code comments except doc comments and a comment on a genuinely confusing detail. Describe code by what it does.
- No lint or build-check suppression directives (`#[allow]`, `#pragma` diagnostics, `NOLINT`). Fix the cause. The funnels' `#[expect]` with a reason is the one existing form; do not add more.
- `std::process::Command::new` only through `relift_proc::command`. File writes only through `relift-cache`. `unsafe` only in crates on the roster in `relift/tests/unsafe_roster.rs`; ceilings only fall.
- Generated files (`cpus/*/src/**/generated*`, `runtime/gpuapi/vk/gen`) are never edited by hand. Change the generator or the Sail overlay and regenerate.
- Docgen markers (`<!-- docgen:... -->`) are never edited by hand. Run docgen.
- Docs: plain US English, no em-dashes, no filler words (leverage, robust, seamless, ensure), describe code by what it does. Units: never mix MiB/GiB with MB/GB.
- Every CI job sets `timeout-minutes: 15`. Split or speed up a job; never raise the limit.
- Container images: `FROM scratch` or Alpine.
- Use zstd for compression unless a tool cannot read it.
- New dependencies: license must pass `cargo deny` (MIT, BSD, ISC, Apache-2.0, Zlib, 0BSD, Unicode-3.0). Pin the exact version (`=x.y.z`) in `[workspace.dependencies]`, and pin downloads by commit and hash. Prefer a well-tested library over writing your own, within the license rules.
- Licensing: never copy code from GPL, AGPL or LGPL projects. Read them for facts (behavior, register layouts, timings) only. GPL emulators may run as separate processes in `check/` as oracles, never linked into the main workspace. Keep the notices of MIT, BSD, ISC and Apache-2.0 sources you port from.
- Host rules: no JIT, no writable and executable memory, no `dlopen`-only paths in release builds (iOS must stay possible). `cfg(target_*)` may select a value, never fork behavior inside a function body.
- Runtime layers: `runtime/core` names no guest OS and no CPU (`make -C runtime charter`). A layer includes only its own headers and those below it.

## Commits

- Subject at most 72 characters, imperative, prefixed by the area: `switch:`, `gpus/sm86:`, `cpus/arm32:`, `runtime:`, `ci:`, `docs:`.
- One commit per logical change. Every commit builds and passes the checks on its own.
- Rebase; no merge commits. Main moves fast.
- No AI attribution and no `Co-Authored-By` trailers.
- Do not add a repo-local git identity.

## Do not

- Never force-push to `main`, and never rewrite a branch someone else pushed.
- Never `git stash` in a shared worktree (the stash is shared by every worktree of the repository). Commit work in progress and amend, or `git rebase --autostash`.
- Never kill a process you did not start. No `pkill -f` and no `killall`.
- Never `git reset --hard`, `git clean -fd` or `git checkout .` over changes you did not make.
- Never leave scratch files in the repository; use `CI_WORK` or a folder you own, and delete it afterwards.
- Never wipe, reflash or change the settings of a device you test on. Install and run the test build only.
- Never push or open a PR unless the human asked you to.
- Never fix a failing check by editing the check, the charter ceiling, the unsafe roster or a generated file. Fix the cause.
- Never add a game-specific branch, table or constant to make one title work. Find the general cause and report it if you cannot fix it.

## Where to start

`GAPS.md` lists open work with sizes and how to verify each. Many areas have active work: read its "coordinate first" list and ask the human to open an issue before you start there.
