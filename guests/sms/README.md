# guests/sms

The Sega Master System, Game Gear and SG-1000 guest. State: pieces only;
`relift import` does not take a `.sms`, `.gg` or `.sg` file yet.

| Crate | Folder | Does |
|---|---|---|
| `relift-sms-formats` | `formats` | Image size and bank layout (odd sizes, copier header), the `TMR SEGA` header and export BIOS checksum, the Codemasters header and checksum, system detection, a static mapper guess |
| `relift-sms-tools` | `tools` | `relift-smsrom`: `info` prints all of the above as text or JSON |

```sh
cargo build --release -p relift-sms-tools
cargo test -p relift-sms-formats -p relift-sms-tools
relift-smsrom info rom.sms
relift-smsrom info --json rom.gg
```

The system comes from the header's region code; the file extension is used
only when there is no usable header. The mapper guess is a Codemasters
checksum match first, then a byte scan for `ld (nn),a` stores to the mapper
registers; function discovery refines it later. Tests build their input with
`synth`; no game data is used.
