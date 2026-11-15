# cpus/arm64

The ARM64 (AArch64) frontend of relift: decodes A64 words and lifts them to
`relift-ir`, for a CPU feature set that the guest names (the Switch uses a
Cortex-A57: Armv8.0-A with AES, PMULL, SHA1, SHA256 and CRC32). The crate
(`relift-cpu-arm64`) depends only on `core/ir` and `core/guest`.

- `src/decode/generated.rs` is generated from Arm's AARCHMRS; the lifting
  templates in `src/lift/sail/generated/` come from sail-arm. Both are
  committed and never edited by hand: `gen/README.md` says how to regenerate
  them.
- A hand-written lifter covers the forms the checker was brought up on and is
  kept as a cross-check.
- Cryptographic instructions lift to `arm64.*` intrinsics that the runtime runs
  on the host's own instructions when it has them (`docs/abi.md` section 10.2).

```sh
cargo test -p relift-cpu-arm64      # lifted words on the IR interpreter
```

The differential check against Unicorn, QEMU and an Apple M2 is in `check/`.
The C half is `runtime/cpus/arm64`.
