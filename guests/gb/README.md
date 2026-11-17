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

The runtime half is `runtime/guests/gb` and its `exact/` program;
`runtime/guests/gb/devtests` holds the lockstep check and the corpus runner
CI uses (`runtime/guests/gb/devtests/ci_cases.toml` lists the cases).

## Building a game

```sh
relift import some-game.gbc
relift build          # a release build
relift build --dev    # a development build
relift run            # a development build, run in a window
```

`relift import` takes a cartridge ROM as it is and writes `game.toml` with
`guest = "gb"`. `relift build --mods hack.ips` applies IPS, UPS or BPS
ROM-hack patches in order before discovery. A build also needs cargo: it
builds the device library (rustyboi-core) into the cache. `relift build`
prints the path of the built program; call it directly for a headless run,
with the options below.

The devices come from rustyboi-core (`devices`), driven at every instruction
boundary; lifted code runs the instruction bodies (`docs/abi.md` section 13,
"The exact Game Boy mode"). `relift build` also lifts every address that
starts a decodable instruction in the fixed window and in each bank
discovery found code in; `RELIFT_GB_SUPERSET=none` turns that off and `all`
takes every bank.

- A release build (`relift build`) links the device library without its CPU
  interpreter. The first instruction with no lifted code writes a stop
  report and ends the run (`docs/guides/usage.md`); `relift report` adds its target
  to the learned file for dev builds.
- A development build (`relift build --dev`, and `relift run`) runs code with
  no lifted body on the device library's CPU one instruction at a time until
  it reaches lifted code. The program adds what it ran that way to
  `learned.txt` in the project's import directory (`--learn FILE`,
  `--no-learn`), and the next dev build lifts it; `--interp-log FILE` lists
  every such instruction and why it ran there. Release builds leave the
  learned file out.

The built program takes `--headless`, `--model dmg|mgb|sgb|cgb|agb|...`,
`--frames N` (stop after N frames and print the frame hash and the serial
output), `--input` (a scripted joypad, `frame:buttons,...`), `--png FILE`,
`--png-dir DIR --png-every N`, `--wav FILE` (record the sound, 44100 Hz
stereo, and print its length, peak and hash), `--no-audio`, `--case FILE`
(grade a test ROM by a corpus case spec, sound cases included), `--save FILE`,
`--no-save` and `--stats`; a development build also takes `--check`
(lockstep against the device library's own CPU). `--help` lists them. In a
window the sound goes to the host output, and the frames are paced on that
output's queue. The battery save is a raw `.sav` next to the imported ROM,
the format other emulators read.
