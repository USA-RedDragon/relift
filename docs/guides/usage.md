# Using relift

The commands of `relift`, for every guest. For the whole path from a dump to a
running game, with real output and what to do when the program stops, read
[Recompile a game](workflow.md) first. The
per-guest details (import layouts, the built program's flags) are in
[Switch](../../guests/switch/README.md), [Game Boy](../../guests/gb/README.md) and
[Game Boy Advance](../../guests/gba/README.md); packaging for each host is in
[Package a game for each host](packaging.md).

## Projects and the cache

Run `relift` in an empty project folder. Each project holds one `game.toml`;
artifacts go to the cache directory (`cache-dir`, [Settings](../reference/configuration.md)).

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
| `relift build` | A shipped build: the runtime linked in, no code loaded at run time, packaged for the host ([Package a game for each host](packaging.md)). `--target android-arm64`, `android-x64`, `ios-arm64` or `ios-sim-arm64` builds a phone app; `--mods` builds code mods or ROM patches in; `--dev` makes a Game Boy development build ([Game Boy](../../guests/gb/README.md)) |
| `relift run` | Builds a dev build if needed and runs it |
| `relift push --target <t>` | Copies the project's game data (romfs, DLC) onto a phone for the app `relift build --target` made and checks every file against the app's data manifest |
| `relift report <file>` | Explains a stop report and adds the missed target as a root for the next build |
| `relift info [binary]` | Prints a built program's self-description without running it |
| `relift coverage` | Switch projects: what discovery could not resolve |
| `relift diff <function>` | Switch projects: one lifted function against the reference interpreter in a live run |

`relift check` and `relift where` are parsed but not implemented yet; they
stop with "not implemented yet" and say which command to use instead.

## Optimization of lifted code

A release build compiles lifted C at <!-- docgen:const crate=relift name=build::RELEASE_LIFTED_OPT -->`-O2`<!-- /docgen:const -->:

<!-- docgen:doc crate=relift name=build::RELEASE_LIFTED_OPT -->
The optimization of lifted C in a release build: `relift build` for this
host or for any `--target`, and the packages made from it (AppImage,
macOS app, APK, iOS app, Windows program). A release build is made
rarely and kept, so it pays for the smaller, faster code: a Nintendo DS
game's 650 lifted files hold 469 MB of x86-64 code at -O2 against 898 MB
at -O0, and run 3000 frames in 21.2 s against 27.4 s, for 4660 CPU
seconds of compiling against 1040 at -O0 and 4150 at -O1. A dev build
(`relift run`, `relift build --dev`) compiles lifted C at its guest's
own choice, the `LIFTED_OPT` of each guest module, which favors compile
time.
<!-- /docgen:doc -->

The dev build of each guest:

**Game Boy**: <!-- docgen:const crate=relift name=gb::LIFTED_OPT -->`-O0`<!-- /docgen:const -->.

<!-- docgen:doc crate=relift name=gb::LIFTED_OPT -->
The optimization of lifted code in a dev build (`relift build --dev`,
`relift run`). In the exact mode the device library runs at every access
and boundary, so the lifted bodies are a small part of the run: a Game
Boy Color game with 256 MBC5 banks runs 2400 frames in 8.1 s with its
lifted code at -O0, -O1 or -O2 alike, while -O0 compiles in a quarter of
the time of -O2 (1.7 s against 6.8 s on 8 jobs). A release build still
uses `crate::build::RELEASE_LIFTED_OPT`, for the smaller program.
<!-- /docgen:doc -->

**Game Boy Advance**: <!-- docgen:const crate=relift name=gba::LIFTED_OPT -->`-O0`<!-- /docgen:const -->.

<!-- docgen:doc crate=relift name=gba::LIFTED_OPT -->
The optimization of lifted code in a dev build (`relift run`); a release
build uses `crate::build::RELEASE_LIFTED_OPT`. Every access is a bus
call, so the lifted bodies are a small part of a run, and -O0 compiles
fastest.
<!-- /docgen:doc -->

**Nintendo DS**: <!-- docgen:const crate=relift name=nds::LIFTED_OPT -->`-O0`<!-- /docgen:const -->.

<!-- docgen:doc crate=relift name=nds::LIFTED_OPT -->
The optimization of lifted code in a dev build (`relift run`), as on the
GBA: every access is a bus call, and -O0 compiles fastest. A release
build uses `crate::build::RELEASE_LIFTED_OPT`.
<!-- /docgen:doc -->

**Master System**: <!-- docgen:const crate=relift name=sms::LIFTED_OPT -->`-O0`<!-- /docgen:const -->.

<!-- docgen:doc crate=relift name=sms::LIFTED_OPT -->
The optimization of lifted code in a dev build (`relift run`), as on the
GBA: -O0 compiles fastest. A release build uses
`crate::build::RELEASE_LIFTED_OPT`.
<!-- /docgen:doc -->

**Nintendo 64**: <!-- docgen:const crate=relift name=n64::LIFTED_OPT -->`-O1`<!-- /docgen:const -->.

<!-- docgen:doc crate=relift name=n64::LIFTED_OPT -->
The optimization of lifted CPU code in a dev build (`relift run`): -O1
runs a frame about twice as fast as -O0 and within a few percent of -O2,
which builds the most. A release build uses
`crate::build::RELEASE_LIFTED_OPT`.
<!-- /docgen:doc -->

Lifted RSP code of a Nintendo 64 program is always built at
<!-- docgen:const crate=relift name=n64::RSP_OPT -->`-O2`<!-- /docgen:const -->. A Switch program's lifted code is
built at -O2 in both builds, and its cold copy at -Os.

## Missed code and stop reports

Discovery cannot see every code address (a pointer the program computes, a
jump table it does not recognize). A shipped build that reaches such code,
or stops on any other fatal guest error, writes a stop report and exits with
status <!-- docgen:const file=runtime/core/include/relift/abi_core.h name=RL_EXIT_SOFTWARE -->`70`<!-- /docgen:const -->. The report is a plain-text file `relift-stop-<date>-<time>-<pid>.txt`
in `RELIFT_REPORT_DIR` if set, else in `reports` in the app's user folder
(on Android in the app's files folder, on iOS in its Documents, where the
Files app shows it). `relift report <file>`, run in the project directory,
prints the transfer site and target with the guest instructions around them
and what the build knew about the target, and adds the target as a root: a
hint in `game.toml` (Switch, GBA) or a line in the learned file (Game Boy;
only development builds read it).
`--no-hint` only prints. The format and the folder rules are in
[the ABI contract](../abi.md#stop-reports), section 11, "Stop reports".

Under `relift run` a Switch program asks relift for the missed code instead:
relift lifts it and everything newly reachable from it, compiles one shared
library, the program loads it and goes on. relift adds each such address to
the `[hints]` list in `game.toml`, and the next `relift build` lifts it with
the rest ([the ABI contract](../abi.md), section 6). Hot-loading is not available for the
Game Boy Advance yet; the Game Boy dev build runs such code on the device
library's CPU ([Game Boy](../../guests/gb/README.md)).

## Self-description

A built program prints its own description with `--relift-info`: identity,
run-time settings, input ports and bindings, mods and save kind. `relift
info` prints the same document without running the program. A launcher
reads it to build its settings and input screens ([Input ports and bindings](../reference/input.md)).

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
