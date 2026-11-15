# guests/switch

The Nintendo Switch guest. It reads decrypted dumps only and has no keys.

| Crate | Folder | Does |
|---|---|---|
| `relift-switch-formats` | `formats` | romfs, NSO, NRO, NPDM, NUP and IPSwitch `.pchtxt` readers, and the import of a `.nup` or an extracted tree |
| `relift-switch-names` | `names` | Itanium names as host names, the address fallback and the reverse map |
| `relift-switch` | `guest` | The guest over the formats and `relift-cpu-a64`: the CPU feature set, module loading, exception-handling tables, the program description |
| `relift-switch-tools` | `tools` | `relift-nup`, `relift-romfs`, `relift-nso`, `relift-nro`, `relift-npdm` and `relift-ir` |
| `relift-switch-vkgen` | `vkgen` | `relift-vkgen`: generates the Vulkan forwarding C from the pinned `vk.xml` |

```sh
cargo build --release -p relift-switch-tools
cargo test -p relift-switch-formats -p relift-switch -p relift-switch-names
relift-nup ls game.nup
relift-nso show main.nso
```

Tests build synthetic input; no game data is used. The runtime half is
`runtime/guests/switch`: the Horizon kernel, the scheduler, IPC and the
services, and `vk/` for Vulkan forwarding. To build and run a game, use
`relift import` and `relift run` (see the top-level README).
