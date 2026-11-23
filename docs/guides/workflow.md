# Recompile a game

This page answers "how do I recompile a game with relift?" from the start: whether your console is supported, what to bring, the commands with the output they print, what to do when the program stops, and how to report what you find. A line with only `...` marks output that this page cuts. The output shown is real: it comes from the `relift` version below, on Linux, run on public test ROMs and on a synthetic program that the tests build. No commercial game was used. The times come from a 32-thread desktop; yours will differ.

The version these outputs come from, and the one to put in a report:

<!-- docgen:output crate=relift bin=relift args="--version" -->
```text
relift 0.1.0
```
<!-- /docgen:output -->

relift is in early development. A game that builds and runs is a good result, and a game that stops early is the expected one. Most of this page is about the second case.

## 1. Is my console supported?

[The README's guest table](../../README.md#guests) lists every console relift can lift, its CPU and how far it has got. Each guest has a page with the details:

| Console | Page |
|---|---|
| Nintendo Switch and Switch 2 | [guests/switch](../../guests/switch/README.md) |
| Game Boy and Game Boy Color | [guests/gb](../../guests/gb/README.md) |
| Game Boy Advance | [guests/gba](../../guests/gba/README.md) |
| Nintendo 64 | [guests/n64](../../guests/n64/README.md) |
| Master System, Game Gear and SG-1000 | [guests/sms](../../guests/sms/README.md) |
| Nintendo DS | the README table only; `import` and `build` take a decrypted `.nds` |

Read the state column as it is written. "Test programs run" means programs that relift's own tests build; "a retail game reaches gameplay in test runs" means one game was seen to get there, not that yours will. Master System, Game Gear and SG-1000 builds need an emulator trace today because their code discovery is not written, so they are not a starting point.

If your console is not in the table, relift cannot recompile it, and no setting changes that.

## 2. What you need

- The tools: [Install](../start/01-install.md) builds relift from the repository. You need Rust (pinned by the repository), clang or `zig cc`, and ninja. SDL3 and libopus are optional; without them the program runs headless and silent.
- A dump of a game that you own, made legally and already decrypted. relift reads decrypted inputs only, holds no keys and ships no game data. How to dump a game is outside this project; the guest pages say what shape the file must have.

| Console | Bring | Shape |
|---|---|---|
| Switch, Switch 2 | a `.nup`, or a folder with `exefs/` and `romfs/` (optionally `base/`, `updates/<version>/`, `dlc/`) | decrypted; an NSP, NSZ, XCI, XCZ or NCA is refused ([layout](../../guests/switch/README.md#building-a-game)) |
| Game Boy, Game Boy Color | a `.gb` or `.gbc` ROM | as dumped; IPS, UPS and BPS patches apply with `--mods` |
| Game Boy Advance | a `.gba` ROM | as dumped; no BIOS image is needed |
| Nintendo 64 | a ROM (`.z64`, `.v64`) | any byte order ([guests/n64](../../guests/n64/README.md)) |
| Nintendo DS | a `.nds` ROM | decrypted, with a valid header CRC |

Run `relift` in an empty folder, one folder per game. The folder holds one `relift.toml`; everything relift builds goes to the cache directory (`~/.cache/relift` by default).

## 3. Import, build and run

### Import

The first command, in the project folder, is `import`. This one imports a Game Boy test ROM, dmg-acid2, which the CI fetches by hash from a public test ROM release:

```text
$ relift import ~/roms/dmg-acid2/dmg-acid2.gb
imported DMG-ACID2 (none, 32768 bytes, Game Boy), into ~/.cache/relift/projects/acid/import; wrote relift.toml, 0.0 s
```

`import` wrote `relift.toml` and copied what it needs into the cache. The file names the guest and records where the ROM came from:

```text
# The project file, written by relift import.

[game]
guest = "gb"
title = "DMG-ACID2"
version = 0
import_dir = "projects/acid/import"
rom = "~/roms/dmg-acid2/dmg-acid2.gb"
rom_sha256 = "464e14b7d42e7feea0b7ede42be7071dc88913f75b9ffa444299424b63d1dff1"
model = "dmg"
```

Importing the same file again is harmless; it does not rewrite `relift.toml`:

```text
$ relift import ~/roms/dmg-acid2/dmg-acid2.gb
imported DMG-ACID2 (none, 32768 bytes, Game Boy), into ~/.cache/relift/projects/acid/import, 0.0 s
```

The wrong input gets a message that says what relift wanted. These are the common ones.

An encrypted container (here an NSP header), which relift will not read:

```text
$ relift import game.nsp
relift import: game.nsp: relift reads decrypted dumps only (a .nup file or an extracted exefs/romfs tree); convert an encrypted dump with a separate tool first
# exit status 1
```

A file that is not any kind of dump (here random bytes):

```text
$ relift import junk.bin
relift import: malformed: junk.bin: not a .nup file (unknown magic) and not a dump directory; relift reads decrypted dumps only (a .nup file or an extracted exefs/romfs tree); convert an encrypted dump with a separate tool first
# exit status 1
```

A folder that has no `exefs/` or `romfs/`:

```text
$ relift import empty
relift import: malformed: empty: not a dump directory: want base/ and updates/<version>/ holding exefs/ and romfs/, or exefs/ and romfs/ directly
# exit status 1
```

A dump folder whose `exefs/` has no `main.npdm`:

```text
$ relift import baddump
relift import: baddump
  exefs extract  0 files, 0 bytes  0.0 s
relift import: malformed: the exefs has no main.npdm, so the program ID is unknown
# exit status 1
```

A missing file, and `--update` on a ROM (updates are a Switch concept):

```text
$ relift import nothere.gb
relift import: i/o: nothere.gb: No such file or directory (os error 2)
# exit status 1
```

```text
$ relift import --update 3 ~/roms/dmg-acid2/dmg-acid2.gb
relift import: --update is for Switch dumps; a cartridge ROM has no updates
# exit status 1
```

### Build

`relift build` makes a shipped build. The first build of a project also compiles the runtime and, for Game Boy, builds the device library with cargo, so it is the slow one:

```text
$ relift build
relift build: acid (DMG-ACID2, 32 KiB, linux-x86_64)
  rom analyze  21 functions, 1 unresolved  0.0 s
  rom lift     85 functions in 6 units, 0 not lifted, 17 left to dispatch, 41 nop sleds  0.0 s
window and audio: SDL3 (pkg-config); opus: libopus (pkg-config)
      compile  60 files  1.4 s
built 85 functions, 0 instructions not lifted, 8.9 s in all
  output: ~/.cache/relift/projects/acid/build/host/acid
# wall time 8.89 s
```

The lines are the stages. `rom analyze` finds functions, `rom lift` turns them into C, and `compile` runs the C compiler over the lifted code and the runtime. A Switch program shows `discover`, `lift`, `emit` and `compile` per module instead, and a `cold` line for the extra copy of the code that dispatch can enter at any instruction.

The second build finds everything in the cache and compiles nothing:

```text
$ relift build
relift build: acid (DMG-ACID2, 32 KiB, linux-x86_64)
  rom analyze  21 functions, 1 unresolved  0.0 s
  rom lift     85 functions in 6 units, 0 not lifted, 17 left to dispatch, 41 nop sleds  0.0 s
window and audio: SDL3 (pkg-config); opus: libopus (pkg-config)
      compile  0 files  0.0 s
built 85 functions, 0 instructions not lifted, 0.1 s in all
  output: ~/.cache/relift/projects/acid/build/host/acid
# wall time 0.08 s
```

A change that touches a few functions reuses the emitted shards that did not change: a Switch rebuild prints `emit  1 shard, 0 overrides, 1 of 2 reused` and compiles only what is new ([Use relift](usage.md#projects-and-the-cache)).

### Run

`relift run` builds a development build if needed and opens a window. To run without a window, call the built program yourself: `--headless` opens none, `--frames N` stops after N frames and prints the frame hash, and `--png FILE` writes the last frame. `RELIFT_AUDIO=null` plays through a null output, and `RELIFT_VIDEO=headless` opens no window on the guests that have one.

```text
$ ./acid --headless --frames 15 --png ~/acid/last.png
relift-gb: pass: frame 5f8fc47e01df7f2d serial 
relift-gb: audio: 16657 frames at 44100 Hz, peak 8781, hash 769514bc8a4bb134
```

The line `pass` is the program's own verdict for a run with no test case; for a game it only means that the run ended without a stop. `--png-dir DIR --png-every N` writes a series of frames, `--wav FILE` records the sound and `--input` scripts the pad:

```text
$ ./acid --headless --frames 30 --png-dir frames --png-every 10
relift-gb: pass: frame 5f8fc47e01df7f2d serial 
relift-gb: audio: 27732 frames at 44100 Hz, peak 8781, hash 2060f1dc721874f4
```

```text
last.png:               PNG image data, 160 x 144, 8-bit/color RGB, non-interlaced
frames/frame000010.png: PNG image data, 160 x 144, 8-bit/color RGB, non-interlaced
frames/frame000020.png: PNG image data, 160 x 144, 8-bit/color RGB, non-interlaced
```

## 4. What usually happens next: it stops

Discovery cannot see every code address: a pointer the program computes, a jump table it does not recognize, code it copies into RAM. A build that reaches such code has no lifted function for it. That is missed code, and it is the normal state of an early recompiler, not a sign that you did something wrong.

To show a stop with a public ROM, this section builds Blargg's `cpu_instrs` Game Boy test with `RELIFT_GB_SUPERSET=none`, which turns off the extra lifting of every decodable address. The ROM copies code to RAM and jumps into it, which discovery misses.

```text
$ relift build
relift build: stop (CPU_INSTRS, 64 KiB, linux-x86_64)
  rom analyze  45 functions, 1 unresolved  1.0 s
  rom lift     5,143 functions in 2,046 units, 0 not lifted, 0 left to dispatch, 1 nop sled  0.2 s
window and audio: SDL3 (pkg-config); opus: libopus (pkg-config)
      compile  2,605 files  3.2 s
built 5,143 functions, 0 instructions not lifted, 4.5 s in all
  output: ~/.cache/relift/projects/stop/build/host/stop
# wall time 4.48 s
```

Running it:

```text
$ ./stop --headless --frames 300
[relift error stop] missed code: no lifted code at guest 0x0b8f (ROM bank 0); no lifted code at the ROM address (opcode 0xf5); report written to ~/reports/relift-stop-20261118-084359-3147136.txt
relift stop report 1
kind: missed code
summary: no lifted code at guest 0x0b8f (ROM bank 0); no lifted code at the ROM address (opcode 0xf5)
relift: 0.1.0
game: gbc version 0
code mods: none
build: ebda5b2d6869a7d5
guest: gb
module: rom0 base 0x0 size 0x4000 build -
guest layer: gb
cpu: sm83
image: 8c5e12f41e0ba5bbca796944f92ffe6de28809198682c4332e38d1b3cf56fcf2
pid: 3147136
thread: 0
time: 1794991439
transfer: jump
target: 0xb8f rom0+0xb8f
target code: not lifted (outside every lifted function)
target where: ROM bank 0, file offset 0xb8f
target words 0xb8f: f5 e0 01 3e 81 e0 02 f5
...
  #14 0x55b7bf01a635 game+0xda635
end of report
relift-gb: fail: the run stopped before a verdict (status 70)
relift-gb: audio: 2984 frames at 44100 Hz, peak 0, hash 0000000000000000
# exit status 70
```

Three things to read here:

- The program exited with status `70`, and `[relift error stop]` gives the kind, a one-line summary and the report file. The report is all the program leaves behind.
- The report is plain text. `RELIFT_REPORT_DIR` sets its folder; otherwise it goes to `reports` in the program's user folder (under `~/.local/share/` on Linux, the app's files folder on Android, Documents on iOS).
- The last lines are the Game Boy runner's own summary: `fail: the run stopped before a verdict (status 70)`.

### The stop report

| Field | Meaning |
|---|---|
| `kind`, `summary` | what happened: `missed code` is code with no lifted function; the other kind is an instruction the runtime could not run |
| `relift`, `build`, `image` | the relift version, the build's id and a hash of the program image; they identify the build when you report it |
| `guest`, `cpu`, `module` | which guest layer and CPU stopped, and the module (a Switch NSO, a Game Boy bank, a GBA ROM) with its base and size |
| `transfer` | how execution got there, such as `call` or `jump` |
| `target`, `target code` | the guest address that had no code, with its module and offset, and whether it lies inside a lifted function |
| `target where` | which segment or bank holds it; a target in no module is a wild pointer, not missed code |
| `target words` | the first words at the target |
| `site`, `site function`, `site words` | the instruction that made the transfer, the lifted function it is in, and the words around it |
| `caller function` | the lifted function that called the target (calls only; a jump leaves no frame) |
| `registers` | the guest registers at the stop |
| `guest backtrace`, `host backtrace` | the lifted frames (tail calls leave none) and the host stack |

`relift report FILE`, run in the project folder, adds the guest code around the site and the target and the lifted C at the site. Here the site is code in RAM at `0xd601` that jumps to ROM bank 0:

```text
$ relift report ~/reports/relift-stop-20261118-084359-3147136.txt
missed code: no lifted code at guest 0x0b8f (ROM bank 0); no lifted code at the ROM address (opcode 0xf5)
  relift: 0.1.0
  game: gbc version 0
  build: ebda5b2d6869a7d5
  guest: gb
  code mods: none
  image: 8c5e12f41e0ba5bbca796944f92ffe6de28809198682c4332e38d1b3cf56fcf2

transfer: jump
  target: 0xb8f rom0+0xb8f
  target code: not lifted (outside every lifted function)
  target where: ROM bank 0, file offset 0xb8f
  site: 0xd601

guest code at the site:
  => 0xd601: c3 8f 0b  Jp None, Imm16
     0xd604: ff        Rst Vector(56), None
     0xd605: 95        Sub R8(A), R8(L)
     0xd606: 03        Inc R16(Bc), None
     0xd607: 20 20     Jr Cond(Nz), SignedImm8

guest code at the target:
  => 0x0b8f: f5        Push None, R16(Af)
     0x0b90: e0 01     Ld HighImm8, R8(A)
     0x0b92: 3e 81     Ld R8(A), Imm8
     0x0b94: e0 02     Ld HighImm8, R8(A)
     0x0b96: f5        Push None, R16(Af)

...

added 000:0b8f to ~/.cache/relift/projects/stop/import/learned.txt; a development build (relift build --dev, relift run) lifts it; a release build leaves the learned file out
```

The last line is the action. A Game Boy project gets the target in `learned.txt` in its import directory; a Switch or Game Boy Advance project gets it as a hint in the `[hints]` table of `relift.toml`. `relift report --no-hint FILE` only prints.

### What a release build does with it

A release build (`relift build`) is what you ship, and it never runs code that was not lifted. For a Game Boy project it leaves the learned file out, so rebuilding changes nothing:

```text
$ relift build
relift build: stop (CPU_INSTRS, 64 KiB, linux-x86_64)
  rom analyze  45 functions, 1 unresolved  1.0 s
  rom lift     5,143 functions in 2,046 units, 0 not lifted, 0 left to dispatch, 1 nop sled  0.1 s
window and audio: SDL3 (pkg-config); opus: libopus (pkg-config)
      compile  0 files  0.0 s
built 5,143 functions, 0 instructions not lifted, 1.3 s in all
  output: ~/.cache/relift/projects/stop/build/host/stop
```

```text
$ ./stop --headless --frames 300
[relift error stop] missed code: no lifted code at guest 0x0b8f (ROM bank 0); no lifted code at the ROM address (opcode 0xf5); report written to ~/reports/relift-stop-20261118-084401-3147608.txt
```

That is by design: a stop in a release build means the build is incomplete. The way to complete it is the development build.

### The development build and hot-loading

What a development build does with missed code depends on the guest:

| Guest | Release build | Development build |
|---|---|---|
| Switch, Switch 2 | stops with a report | `relift run` hot-loads: relift lifts the missing code and everything newly reachable from it, compiles a shared library, and the program loads it and goes on; the address is added to `[hints]` in `relift.toml` |
| Game Boy, Game Boy Color | stops with a report | `relift build --dev` runs unlifted code on the device library's CPU, one instruction at a time, and records it in `learned.txt`; the next `--dev` build lifts it |
| Game Boy Advance | stops with a report | no hot-load yet; `relift report` adds the target as a hint and the next build lifts it |
| Nintendo 64, Master System, Game Gear | stops with a report | no hot-load yet |

The Game Boy case with the same ROM. A development build reads `learned.txt`:

```text
$ relift build --dev
relift build: stop (CPU_INSTRS, 64 KiB, linux-x86_64)
  rom analyze  47 functions, 1 unresolved, 1 seen at run time  1.0 s
  rom lift     5,145 functions in 2,046 units, 0 not lifted, 0 left to dispatch, 1 nop sled  0.1 s
window and audio: SDL3 (pkg-config); opus: libopus (pkg-config)
      compile  50 files  1.1 s
built 5,145 functions, 0 instructions not lifted, 10.5 s in all
  output: ~/.cache/relift/projects/stop/build/host/stop
# wall time 10.47 s
```

Run with `--stats`, the program counts what it ran on the CPU instead of lifted code (the lines are cut to the two that matter):

```text
$ ./stop --headless --frames 300 --stats
relift-gb: pass: frame 2524576692a9
interpreter rom 13 entries 110 instructions, ram 0 entries 0 instructions
```

The run passed, and `interpreter rom 13 entries 110 instructions` is the count of unlifted instructions it ran. The program recorded each place it entered in `learned.txt`. Rebuilding lifts them, and the count drops:

```text
$ relift build --dev
  rom analyze  53 functions, 3 unresolved, 11 seen at run time  1.3 s
built 5,155 functions, 0 instructions not lifted, 2.5 s in all
```

```text
$ ./stop --headless --frames 300 --stats
relift-gb: pass: frame 2524576692a9
interpreter rom 1 entries 38 instructions, ram 0 entries 0 instructions
```

One more round:

```text
$ relift build --dev
  rom analyze  61 functions, 3 unresolved, 15 seen at run time  1.3 s
built 5,163 functions, 0 instructions not lifted, 2.3 s in all
```

```text
$ ./stop --headless --frames 300 --stats
relift-gb: pass: frame 2524576692a9
interpreter rom 0 entries 0 instructions, ram 0 entries 0 instructions
```

After three development builds the ROM runs with no instruction on the interpreter. `--interp-log FILE` lists every instruction that did run there and why, and `--no-learn` keeps a run from writing the learned file. A development build writes to the same output path as a release build, so run `relift build` again before you ship.

The Switch case is shorter. The synthetic program here has a call through a computed address that discovery cannot follow. By default a Switch build also emits a cold copy of every function that dispatch can enter at any instruction (`emit.cold`, [Settings](../reference/configuration.md)), so this program runs from the cold copy and does not stop. To see the stop, the next outputs build it with `--emit.cold=false`:

```text
$ relift --emit.cold=false build
relift build: tiny (1 module, linux-x86_64)
  main discover 0 functions  0.0 s
  main lift     1 function, 0 unlifted  0.0 s
  main emit     1 shard, 0 overrides, 1 of 1 reused  0.0 s
window and audio: SDL3 (pkg-config); opus: libopus (pkg-config)
       compile  2 files, clang -O2, -j4  0.1 s
built 1 function, 0 instructions not lifted, 0.2 s in all
  output: ~/.cache/relift/projects/tiny/build/host/tiny
# wall time 0.15 s
```

```text
$ ./tiny
[relift info kernel] compact 39-bit layout: address space 0x8000000-0x818000000, 37249 MiB reserved with guards
[relift error stop] missed code: no function at guest 0x8000040; report written to ~/reports/relift-stop-20261118-084412-3162286.txt
relift stop report 1
kind: missed code
summary: no function at guest 0x8000040
relift: 0.1.0
game: switch 0100000000001234 version 0
code mods: none
build: c4ea7fbfe6053411
guest: switch
program: 0100000000001234
module: main base 0x8000000 size 0x2020 build 0000000000000000000000000000000000000000000000000000000000000000
guest layer: switch
cpu: arm64
image: 1d2a4728fc31ebb5f88b3633e5ce3aba6d2ad26aef8ba7afa6f56571fa553e92
pid: 3162286
thread: 0
time: 1794991452
transfer: call
target: 0x8000040 main+0x40
target code: not lifted (outside every lifted function)
...
  #10 0x55ffa2fd90c5 game+0x2b0c5
end of report
# exit status 70
```

`relift report` on a Switch report also says what analysis knew about the site, and writes the hint:

```text
$ relift report ~/reports/relift-stop-20261118-084412-3162286.txt
missed code: no function at guest 0x8000040
  relift: 0.1.0
  game: switch 0100000000001234 version 0
  build: c4ea7fbfe6053411
  guest: switch
  code mods: none
  image: 1d2a4728fc31ebb5f88b3633e5ce3aba6d2ad26aef8ba7afa6f56571fa553e92

transfer: call
  target: 0x8000040 main+0x40
  target code: not lifted (outside every lifted function)
  target where: main text segment
  site: 0x800001c main+0x1c
  site function: 0x8000000 main+0x0 _ZN5guest4main11fn_00000000E +0x1c
  caller function: 0x8000000 main+0x0 _ZN5guest4main11fn_00000000E (host _ZN5guest4main11fn_00000000E+0x89)

guest code at the site:
     0x800000c: mov	x9, #4
     0x8000010: mul	x8, x8, x9
     0x8000014: adr	x10, #-20
     0x8000018: add	x8, x10, x8
  => 0x800001c: blr	x8
     0x8000020: svc	#0x27
     0x8000024: subs	x19, x19, #1
     0x8000028: b.ne	#-36
     0x800002c: svc	#0x7
     0x8000030: b	#0
     0x8000034: udf	#0
     0x8000038: udf	#0

guest code at the target:
  => 0x8000040: mov	x1, #17
     0x8000044: ret
     0x8000048: udf	#0
     0x800004c: udf	#0
     0x8000050: udf	#0
     0x8000054: udf	#0
     0x8000058: udf	#0
     0x800005c: udf	#0

guest backtrace:
  #0 0x8000000 main+0x0 _ZN5guest4main11fn_00000000E

...

what the build knew about the target:
  0x8000040 is not among the build's function entries

what analysis says about the site:
  analysis left this computed call unresolved: reason pointer, target computed

added main+0x40 to the hints in relift.toml; the next relift build lifts it
```

```text
[hints]
# Code that relift run hot-loaded or relift report added; relift build lifts it as roots.
code = [
    "main+0x40",
]
```

`relift run` does the same without the report. It builds a development build, and when the program reaches the missing code it asks relift for it:

```text
$ relift --emit.cold=false run
relift build: tiny (1 module, linux-x86_64)
  main discover 1 function  0.0 s
  main lift     1 function, 0 unlifted  0.0 s
  main emit     1 shard, 0 overrides, 1 of 1 reused  0.0 s
window and audio: SDL3 (pkg-config); opus: libopus (pkg-config)
       compile  7 files, clang -O2, -j4  0.2 s
built 1 function, 0 instructions not lifted, 0.2 s in all
  output: ~/.cache/relift/projects/tiny/build/host/dev/tiny
[relift info kernel] compact 39-bit layout: address space 0x8000000-0x818000000, 37249 MiB reserved with guards
relift run: hot-loaded 0x8000040 (main+0x40): 1 function, 0 unlifted, lift 5 ms, compile 60 ms
[relift info hotload] loaded 0x8000040: 1 functions in 64.8 ms (relift 64.8 ms, load 0.05 ms, register 0.002 ms), 1 libraries so far: 1 functions
hello from relift
hello from relift
hello from relift
hot-loaded 1 target; added to the hints in relift.toml, so the next relift build lifts them
# wall time 0.25 s
```

The program went on and printed its three lines. The hint is in `relift.toml`, so the next release build lifts the target:

```text
$ relift --emit.cold=false build
relift build: tiny (1 module, linux-x86_64)
  main discover 2 functions  0.0 s
  main lift     1/2 functions (50%)  0 unlifted  0.0 s
  main lift     2 functions, 0 unlifted  0.0 s
  main emit     1 shard, 0 overrides, 1 of 2 reused  0.0 s
window and audio: SDL3 (pkg-config); opus: libopus (pkg-config)
       compile  3 files, clang -O2, -j4  0.1 s
built 2 functions, 0 instructions not lifted, 0.2 s in all
  output: ~/.cache/relift/projects/tiny/build/host/tiny
```

A development program run by hand cannot hot-load, and says so:

```text
$ ./dev/tiny
[relift info kernel] compact 39-bit layout: address space 0x8000000-0x818000000, 37249 MiB reserved with guards
[relift error hotload] RELIFT_DEV_SOCKET is not set: this dev build runs only under relift run
[relift error stop] missed code: no function at guest 0x8000040
relift stop report 1
# exit status 70
```

## 5. Check progress

Four commands tell you where a build stands:

| Command | Does | Guests |
|---|---|---|
| `relift coverage` | what discovery could not resolve | Switch; for the others `relift-gbrom functions`, `relift-gbarom functions` or `relift-n64rom functions` |
| `relift diff FUNCTION` | one lifted function against the reference interpreter in a live run | Switch; needs gdb |
| `relift info [BINARY]` | the built program's self-description, without running it | every guest |
| `relift where` | map between a host address, a guest address and a symbol | not implemented yet |

`relift coverage` lists, per module, the functions found, the instructions that did not lift, and every indirect call or jump left to the dispatcher with its reason (`slot`, `table` or `pointer`). The hints column counts what earlier runs recorded:

```text
$ relift coverage
  main discover 1/2 functions (50%)  0.0 s
  main discover 2 functions, 1 transfer unresolved  0.0 s
module  base       functions  unlifted  via slots  slot  table  pointer  hints
main    0x8000000          2         0          0     0      0        1      1

main: 1 transfer unresolved
  0x800001c  main+0x1c  fn_0  call  pointer  computed

main: 1 hint
  0x8000040  main+0x40  lifted
```

`--json` prints the same data for a script and `--module NAME` limits it to one module. An unresolved transfer is not an error; it is where a miss could come from.

`relift diff main+0x30 --count 3` builds a development build, runs it under gdb and, at each call of the function, runs it again in the interpreter from the same registers and live memory. A mismatch names the first register or page that differs. The exit status is 1 after a mismatch and 2 when no call was compared. The gdb output is trimmed here:

```text
$ relift diff --count 3 main+0x30
0x8000030: match, 6 instructions, 2 pages
0x8000030: match, 6 instructions, 2 pages
0x8000030: match, 6 instructions, 2 pages
relift-diff: _ZN5guest4main11fn_00000030E call 1: MATCH steps=6 pages=2
relift-diff: _ZN5guest4main11fn_00000030E call 2: MATCH steps=6 pages=2
relift-diff: _ZN5guest4main11fn_00000030E call 3: MATCH steps=6 pages=2
relift-diff: SUMMARY _ZN5guest4main11fn_00000030E match=3
```

`relift info` prints the document the program prints with `--relift-info`: identity, run-time settings, input ports with their bindings, mods and the save kind:

```text
$ relift info | python3 -m json.tool | head -20
{
    "relift": {
        "protocol": 2,
        "version": "0.1.0"
    },
    "identity": {
        "platform": "switch",
        "title_id": "0100000000001234",
        "names": {
            "default": "tiny"
        },
        "default_language": "default",
        "version": "0",
        "modules": [
            {
                "name": "main",
                "build_id": "0000000000000000000000000000000000000000000000000000000000000000"
            }
        ],
        "base_mismatch": false
```

`relift where` and `relift check` are parsed but not implemented. They say so, and what to use instead:

```text
$ relift where 0x8000040
relift where: not implemented yet; a stop report names the module, the guest address and the offset of a miss, and `relift coverage` lists every address as module+offset
# exit status 1
```

## 6. Settings and input

relift's own settings come from `relift.toml` in the working directory, then `RELIFT_*` environment variables, then flags; later wins. They control the build: `cache-dir`, `jobs`, `toolchain.*`, `emit.*` and `package.*`. [Settings](../reference/configuration.md) lists every key. Per-game data lives in the project's `relift.toml`: the guest and model, the `[hints]` that relift writes, and any `[settings.<key>]` tables a game declares for itself.

The built program has run-time settings of its own, and `relift info` lists each with its type, default and range. In order, last wins: the default, `<user folder>/settings.json`, `--relift-settings FILE`, `--relift-set key=value`, then the key's environment variable (`RELIFT_VIDEO`, `RELIFT_SAVE` and the others in [the C runtime](../../runtime/README.md)). A wrong key or value never stops a run; it warns and is skipped:

```text
$ ./tiny --headless --frames 2 --relift-set nosuch=1 --relift-set picture.aa=bogus
[relift warn settings] unknown setting nosuch; ignored
[relift warn settings] picture.aa: 'bogus' is not one of off, fxaa, smaa; ignored
relift-gb: pass: frame e2a0ea132b4a3b25 serial 
relift-gb: audio: 7058 frames at 44100 Hz, peak 8781, hash d389f0b1fcedae74
```

The input ports and their default keys and buttons come from the same document. For the Game Boy ROM above:

```text
$ ./tiny --relift-info | (a summary)
identity: {"platform": "gb", "names": {"default": "acid"}, "default_language": "default", "version": ...
runtime settings: video, language, video.gpu_driver, gpu.math, picture.aa, picture.aa_quality ...
game settings: []
port p1 - Game Boy pad
  a       gamepad:east, key:x
  b       gamepad:south, key:z
  start   gamepad:start, key:enter
  select  gamepad:back, key:backspace
  up      gamepad:dpad_up, gamepad:left_y+, key:up
  down    gamepad:dpad_down, gamepad:left_y-, key:down
  left    gamepad:dpad_left, gamepad:left_x-, key:left
  right   gamepad:dpad_right, gamepad:left_x+, key:right
save: {"kind": "none", "changes_format": false}
```

Rebind a control with `--relift-set bind.p1.a=gamepad:east,key:f`, or the same key in `settings.json` ([Input ports and bindings](../reference/input.md)). A bad binding is skipped with a warning:

```text
$ ./tiny --headless --frames 3 --relift-set bind.p1.a=bogus
[relift warn settings] bind.p1.a: 'bogus' is not a comma separated list of host inputs such as gamepad:south,key:z; ignored
relift-gb: pass: frame 5f8fc47e01df7f2d serial 
relift-gb: audio: 7797 frames at 44100 Hz, peak 8781, hash caad2be6bce25634
```

`--input FRAME:BUTTON,...` presses buttons at frames in a headless Game Boy run (`--input 5:start`). A name the runner does not know stops the run:

```text
$ ./tiny --headless --frames 20 --input 5:bogus
relift-gb: case: unknown button bogus
# exit status 2
```

## 7. What you can and cannot do

relift has no per-game patches, tables or annotations, and you cannot add them. The goal is that a game recompiles with no manual work: no hand-written address lists, no per-title fixes. That is why the tools above only record addresses that the program itself reached (`[hints]`, `learned.txt`) and never ask you to write one.

What you can do: build, run, change settings and bindings, build ROM patches in with `--mods`, and find out why a build stops.

What you cannot fix yourself: a miss. Each miss is a gap in discovery or in the runtime, and the fix is a general one in relift. That is where a report helps.

### Report a stop

Open an issue at [github.com/USA-RedDragon/relift/issues](https://github.com/USA-RedDragon/relift/issues) with:

- `relift --version`, your host (OS and CPU) and the guest.
- The commands that you ran, in order.
- The stop report file, as it was written.
- The output of `relift build` up to the point that matters.

Never attach game data:

- the ROM, the NUP, the exefs or romfs files, or any other part of the dump;
- the project's import folder in the cache, the lifted C (`c/` folders), or the built program or AppImage;
- save files, RDRAM dumps (`--dump`), screenshots of a commercial game, or a recording of its sound.

A stop report holds register values, addresses and a few instruction words around the stop. It names the program (the Switch title id, the image hash) and holds no file from the game. If you do not want the title id public, say so in the issue and send what you can.

If you are unsure whether something counts as game data, leave it out ([Data rules](../../CONTRIBUTING.md#data-rules)).

## 8. Package for your host

`relift build` also packages the program for the host: an AppImage on Linux, a signed `.app` on macOS, and with `--target` an APK for Android or an app for iOS. [Package a game for each host](packaging.md) has the details. The failures you will meet first are a missing tool or toolchain, and they say what is missing.

No C compiler:

```text
$ PATH=<a folder with no tools> relift build
relift build: acid (DMG-ACID2, 32 KiB, linux-x86_64)
  rom analyze  21 functions, 1 unresolved  0.0 s
  rom lift     85 functions in 6 units, 0 not lifted, 17 left to dispatch, 41 nop sleds  0.0 s
relift build: no clang or zig on PATH
# exit status 1
```

A `toolchain.*` setting that names nothing (a path that is not a file, or a bare name that is not on `PATH`):

```text
$ relift --toolchain.clang /nonexistent/clang build
relift build: toolchain.clang: /nonexistent/clang is not a file; fix the setting (relift.toml, RELIFT_TOOLCHAIN_CLANG, --toolchain.clang) or clear it to search PATH
# exit status 1
```

```text
$ relift --toolchain.ninja no-such-ninja build
relift build: toolchain.ninja: no-such-ninja is not on PATH; fix the setting (relift.toml, RELIFT_TOOLCHAIN_NINJA, --toolchain.ninja) or clear it to search PATH
# exit status 1
```

No `appimagetool`. An AppImage is not made, and the build still finishes:

```text
$ PATH=<clang, ninja, pkg-config, cargo only> relift build
relift build: acid (DMG-ACID2, 32 KiB, linux-x86_64)
  rom analyze  21 functions, 1 unresolved  0.0 s
  rom lift     85 functions in 6 units, 0 not lifted, 17 left to dispatch, 41 nop sleds  0.0 s
window and audio: SDL3 (pkg-config); opus: libopus (pkg-config)
      compile  60 files  1.5 s
built 85 functions, 0 instructions not lifted, 1.6 s in all
  output: ~/.cache/relift/projects/acid/build/host/acid
no AppImage built: appimagetool is not on PATH (get it from https://github.com/AppImage/appimagetool/releases or your package manager, or set toolchain.appimagetool; package.appimage=never silences this)
```

An Android build before the SDL3 and Opus libraries for it are built, and an iOS build on a host that is not a Mac:

```text
$ relift build --target android-arm64
relift build: ~/.cache/relift/sdk/sdl3/aarch64-android/lib/libSDL3.so: not found; build SDL3, libopus and libadrenotools for android-arm64 with `python3 runtime/tools/cross/android_deps.py --cache ~/.cache/relift`
# exit status 1
```

```text
$ relift build --target ios-arm64
relift build: target ios-arm64: builds on macOS with Xcode
# exit status 1
```

A project that has lost its import (a different cache directory, or a cleared cache):

```text
$ relift build
relift build: ~/.cache/relift/projects/acid/import/rom.gb: No such file or directory (os error 2); run relift import again
# exit status 1
```

## 9. When things go wrong

| Symptom | What it means | What to do |
|---|---|---|
| `relift import: ... relift reads decrypted dumps only` | an NSP, NSZ, XCI, XCZ or NCA, which is encrypted | convert the dump to a `.nup` or an extracted `exefs/` and `romfs/` tree with a separate tool; see the guest page |
| `import: malformed: X: not a .nup file (unknown magic) and not a dump directory` | the file is not a container relift reads, or is damaged | check that the file is whole and the right one; a ROM needs its usual extension or a valid header |
| `import: malformed: X: not a dump directory: want base/ and updates/...` | a folder without `exefs/` and `romfs/` | point `import` at the folder that holds them, or at `base/` |
| `import: --update is for Switch dumps` | `--update` on a cartridge ROM | drop `--update` |
| `import: i/o: X: No such file or directory` | the path is wrong | check the path |
| `build: relift.toml: not found in this folder` | you are not in a project folder | `relift import <dump>` here, or change folder |
| `build: ...rom.gb: No such file or directory; run relift import again` | the project's import is not in this cache directory | import again, or point `--cache-dir` at the old one |
| `build: no clang or zig on PATH` | no C compiler | install clang or zig, or set `toolchain.clang` |
| `build: toolchain.X: P is not a file` or `is not on PATH` | a `toolchain.*` setting names nothing | fix the key or clear it |
| `no AppImage built: appimagetool is not on PATH` | Linux without `appimagetool` | install it, or set `package.appimage=never` |
| `build: target ios-arm64: builds on macOS with Xcode` | an iOS build on another host | build on a Mac |
| `build: ...libSDL3.so: not found; build SDL3, libopus ...` | an Android build without the prebuilt libraries | run the `android_deps.py` command in the message |
| `build: lifting failed (exit status: N)` after another line | the lift step failed; the line above it is the cause | fix that line first |
| `window and audio: SDL3 not found, building headless and silent` | no SDL3 or libopus | install them to get a window and sound; a headless build works without |
| `[relift error stop] missed code: ...` and exit status 70 | the program reached code that was not lifted | read the report (section 4), then `relift report`; a Switch program hot-loads under `relift run` |
| `[relift error stop] ...; the report file could not be written` | the report folder is not writable | set `RELIFT_REPORT_DIR` to a folder that is |
| `[relift error hotload] RELIFT_DEV_SOCKET is not set: this dev build runs only under relift run` | a development program run by hand | run it with `relift run`, or build a release build |
| `[relift error hotload] relift could not build code for 0x...: ... is not in the code of any module` (or `no function could be lifted at ...`) | the target is not code: a wild pointer or data | look at `target where` in the report; report it, since it is a bug in the guest layer or in relift |
| `relift report: X: not a stop report` | the file is not a report | pass the `relift-stop-*.txt` file the program wrote |
| `... is already in learned.txt; ... a release build leaves the learned file out` | a Game Boy release build never uses the learned file | `relift build --dev` and run that build |
| `relift-gb: fail: the run stopped before a verdict (status 70)` | the Game Boy run ended on a stop | the stop report is above it |
| `[relift warn settings] ... ignored` | an unknown run-time setting or a bad value | check the key and type with `relift info` |
| `relift-gb: unknown option`, `unknown model`, `case: unknown button` | a bad flag, model or button name | `game --help` lists them |
| `relift where` or `relift check`: `not implemented yet` | these commands are parsed only | use `relift diff` or `relift coverage` as the message says |
| `relift diff: X is not a host name, module:symbol or module+0xoffset` | relift has no function by that name | use `module+0xoffset` from a report or `relift coverage` |
| `relift diff` exits 2 | no call of the function was compared | the program never called it; pick another function or run longer |

## 10. How to help

[Contributing](../../CONTRIBUTING.md) says how to build, check a change and send it, including the rule that no game data enters the repository. [Open work](../../GAPS.md) is a menu of gaps with their size, where they live and how to verify a fix; open an issue naming the gap before you start. Fixing a miss that your game showed, in general and with a public test, is the most direct way to move your game forward.

Next: [Package a game for each host](packaging.md), [Use relift](usage.md) for the commands in one place, and [Settings](../reference/configuration.md).
