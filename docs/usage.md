# Using relift

How a game project is built and run with `relift`, for every guest. The
per-guest details (import layouts, the built program's flags) are in
`guests/switch/README.md`, `guests/gb/README.md` and `guests/gba/README.md`;
packaging for each host is in `docs/packaging.md`.

## Projects and the cache

Run `relift` in an empty project folder. Each project holds one `game.toml`;
artifacts go to the cache directory (`cache-dir`, `docs/configuration.md`).

The cache is relocatable: copy or hard-link it to another path, or move the
project, and the next build reuses every lifted file and object (Ninja runs
nothing). Build commands name files by paths relative to the build
directory, the runtime sources are copied into the cache, and debug info
records `.` as its compilation directory, so the program is byte-identical.
What stays absolute: a cartridge ROM's path (the save sits beside it) and
SDL3 library search paths outside the cache. relift writes every cache file
as a new file renamed over the old one, so hard-linked copies stay
independent; Ninja rewrites `.ninja_log` and `.ninja_deps` in place, so a
hard-linked cache must copy those two.

## Commands

| Command | Does |
|---|---|
| `relift import <dump>` | Makes a project from a decrypted Switch dump (`.nup` or extracted tree) or a cartridge ROM (`.gb`, `.gbc`, `.gba`) |
| `relift build` | A shipped build: the runtime linked in, no code loaded at run time, packaged for the host (`docs/packaging.md`). `--target android-arm64`, `android-x64`, `ios-arm64` or `ios-sim-arm64` builds a phone app; `--mods` builds code mods or ROM patches in; `--dev` makes a Game Boy development build (`guests/gb/README.md`) |
| `relift run` | Builds a dev build if needed and runs it |
| `relift push --target <t>` | Copies the project's game data (romfs, DLC) onto a phone for the app `relift build --target` made and checks every file against the app's data manifest |
| `relift report <file>` | Explains a stop report and adds the missed target as a root for the next build |
| `relift info [binary]` | Prints a built program's self-description without running it |
| `relift coverage` | Switch projects: what discovery could not resolve |
| `relift diff <function>` | Switch projects: one lifted function against the reference interpreter in a live run |

`relift check` and `relift where` are parsed but not implemented yet; they
stop with "not implemented".

## Missed code and stop reports

Discovery cannot see every code address (a pointer the program computes, a
jump table it does not recognize). A shipped build that reaches such code,
or stops on any other fatal guest error, writes a stop report and exits with
status 70. The report is a plain-text file `relift-stop-<date>-<time>-<pid>.txt`
in `RELIFT_REPORT_DIR` if set, else in `reports` in the app's user folder
(on Android in the app's files folder, on iOS in its Documents, where the
Files app shows it). `relift report <file>`, run in the project directory,
prints the transfer site and target with the guest instructions around them
and what the build knew about the target, and adds the target as a root: a
hint in `game.toml` (Switch, GBA) or a line in the learned file (Game Boy).
`--no-hint` only prints. The format and the folder rules are in
`docs/abi.md` section 11, "Stop reports".

Under `relift run` a Switch program asks relift for the missed code instead:
relift lifts it and everything newly reachable from it, compiles one shared
library, the program loads it and goes on. relift adds each such address to
the `[hints]` list in `game.toml`, and the next `relift build` lifts it with
the rest (`docs/abi.md` section 6). Hot-loading is not available for the
Game Boy Advance yet; the Game Boy dev build runs such code on the device
library's CPU (`guests/gb/README.md`).

## Self-description

A built program prints its own description with `--relift-info`: identity,
run-time settings, input ports and bindings, mods and save kind. `relift
info` prints the same document without running the program. A launcher
reads it to build its settings and input screens (`docs/input.md`).

## Coverage and diff (Switch)

`relift coverage` (`--json` for a machine-readable form, `--module <name>`
for one module) lists, per module, the functions found, the instructions
that do not lift, each indirect call or computed jump left to the dispatcher
with its reason (`slot`, `table` or `pointer`), and the hint addresses
earlier dev runs recorded in `game.toml`. For a Game Boy ROM use
`relift-gbrom functions`, for a GBA ROM `relift-gbarom functions`.

`relift diff <function>` builds a dev build, runs it under gdb and, at each
call of the function, runs the same function in the reference interpreter
from the same registers and live guest memory. It compares the registers and
every page the interpreter touched (`--count`, `--skip`, `--engine`). It
needs gdb and exits 1 on a mismatch and 2 when no call was compared.
