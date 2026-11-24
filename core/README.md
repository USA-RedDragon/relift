# core

The guest-neutral crates of relift. Nothing here names a console or a CPU:
`relift/tests/workspace_charter.rs` fails when a crate under `core/` reaches
anything under `cpus/`, `guests/` or `testguest/`.

Build and test one crate, or all of them:

```sh
cargo build -p relift-ir
cargo test -p relift-ir
cargo test --workspace
```

| Crate | Folder | Does |
|---|---|---|
| `relift-ir` | `ir` | The IR (values, ops, explicit flags, memory ops, ticks, I/O accesses, sync points, code banks) and an interpreter; see [the IR reference](../docs/reference/ir.md) |
| `relift-sail` | `sail` | The Sail frontend the template generators share: parser, model tables, staged evaluator |
| `relift-cache` | `cache` | Content-addressed store, stage keys and the one file writer |
| `relift-host` | `host` | The host target list that emit and build share |
| `relift-proc` | `proc` | The one subprocess runner |
| `relift-diff` | `diff` | The protocol, page cache and comparison behind `relift diff` |
| `relift-progress` | `progress` | Progress output for `import` and `build` |
| `relift-info` | `info` | The `--relift-info` document: types, embedding and reader (`relift info`) |
| `relift-patch` | `patch` | IPS, IPS32, UPS and BPS patches |
| `relift-objfile` | `objfile` | Executable and library file formats (ELF now): segments, symbols, relocations, dynamic tags, unwind tables, discovery seeds |
| `relift-guest` | `guest` | The guest contract: frontends, naming, program description |
| `relift-names` | `names` | Guest symbol names as host names: Itanium names remangled into `guest::<module>`, the address fallback and the reverse map |
| `relift-analyze` | `analyze` | Function boundaries, control flow, jump tables, indirect targets |
| `relift-emit` | `emit` | IR to C: shards, listings, memory strategy per host target |
| `relift-emit-wasm` | `emit-wasm` | IR to WebAssembly; not linked by `relift` yet |
| `relift-build` | `build` | Toolchain discovery and the Ninja writer |
| `relift-input` | `input` | Input ports, controls and bindings; the C port tables (`docs/reference/input.md`) |
| `relift-manifest` | `manifest` | The `relift.toml` types |

`docs/concepts/architecture.md` lists the bands and the tests that enforce them. The C
counterpart is `runtime/core`.
