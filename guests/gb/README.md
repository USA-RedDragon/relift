# guests/gb

The Game Boy and Game Boy Color guest.

| Crate | Folder | Does |
|---|---|---|
| `relift-gb-formats` | `formats` | The cartridge header, mapper kinds, size codes, checksums and bank layout |
| `relift-gb-analyze` | `analyze` | Function discovery across mapper banks (bank-switch trampolines, jump tables, pointers in RAM); decode and control flow only |
| `relift-gb` | `guest` | The exact-mode SM83 frontend and the lifting of a ROM into one unit per bank, with its program files |
| `relift-gb-tools` | `tools` | `relift-gbrom`: the header, checks and `functions` |
| `relift-gb-devices` | `devices` | rustyboi-core's devices behind a C ABI, in a Cargo workspace of its own |

```sh
cargo build --release -p relift-gb-tools
cargo test -p relift-gb-formats -p relift-gb-analyze -p relift-gb
cargo test --manifest-path guests/gb/devices/Cargo.toml --release --locked
relift-gbrom rom.gbc
relift-gbrom functions --summary rom.gbc
```

`relift import rom.gbc`, `relift build` and `relift run` use all of these
(top-level README). Code the analysis does not reach runs on the device
library's CPU, one instruction at a time. The runtime half is
`runtime/guests/gb` and its `exact/` program; `runtime/guests/gb/devtests`
holds the lockstep check and the corpus runner CI uses.
