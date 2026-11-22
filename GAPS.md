# Open work

A menu of work that contributors can pick up, grouped by area. Each entry says what is missing, why it matters, where it lives in the tree, how big it is, what to read first, and how to verify a fix. Every "missing" claim was checked against the code when this page was written; where the check was a name search or a doc statement, the entry says so, and you should re-check before you start.

## How to claim a gap

Open an issue first, name the gap by its heading, and say what you plan to do. Many areas have active work, and the maintainer lands finished work in batches, so a two-line thread avoids two people writing the same thing. Wait for a yes on anything marked medium or large. Small entries marked **good first issue** may go straight to a PR, but still say in an issue that you took one.

Sizes: **small** is under about 300 changed lines or a few days' work, **medium** is up to about 1,500 lines, **large** is more, usually in several PRs.

The rules in [CONTRIBUTING.md](CONTRIBUTING.md) apply to every entry: exact behavior, no per-game hacks, zero interpreted guest instructions, no game data in the repository, permissive licenses only. Public test ROMs are fetched by hash into the CI work dir, never committed; check a ROM's license before CI uses it.

## Coordinate first

These areas have active work. Do not start in them without an issue and an answer from the maintainer; there is likely a branch you would collide with.

| Area | Where |
|---|---|
| The Switch 2 NVN2 graphics layer and its services | `runtime/guests/switch/nvn`, `runtime/guests/switch/srv_*.c` |
| The Maxwell (SM53) shader translator and the Switch 1 NVN path | `gpus/sm53`, `guests/switch/guest/src/nvndriver.rs` |
| The SM86 translator, DLSS and the temporal upscalers | `gpus/nv`, `gpus/sm86`, `runtime/core/post.c`, `runtime/core/upscale.c` |
| Nintendo DS discovery and 3D | `guests/nds/analyze`, `runtime/guests/nds/gpu3d*.c` |
| Nintendo 64 RSP timing (and RDP timing) | `runtime/cpus/rsp`, `runtime/guests/n64`, `guests/n64` |

Smaller entries below that touch these folders say so.

## Index

| Area | Entries |
|---|---|
| 1. Guests | Switch, Switch 2, Game Boy, Game Boy Advance, Nintendo 64, Nintendo DS, Master System family |
| 2. CPU and GPU translators | A64, SM53, SM86, shader scan, Vulkan forwarding |
| 3. Runtime and hosts | Windows, macOS, Linux arm64, Android, iOS, presentation, packaging, web |
| 4. Player features | Anti-aliasing and upscalers, save states, input, launcher |
| 5. Tooling, CI and docs | Test-ROM coverage, CI jobs, commands, docs |
| 6. New guests | WebAssembly, NES, Genesis, PS1, SNES, 3DS, later systems |

---

## 1. Guests

### Switch

#### Missing system services (medium each)

- Missing: the service registry in `runtime/guests/switch` has no handler, by a name search, for the audio input service (`audin:u`, the microphone), the NFC service (`nfc:*`; the amiibo service `nfp:user` exists), local wireless (`ldn:*`), the album and screenshot services (`caps:*`), the DNS resolver (`sfdnsres`), play statistics (`pdm:*`), Bluetooth management (`btm`) and the bad-word filter (`ngc`). Play reports (`prepo:*`), the network setup, name and TLS services (`nifm:u`, `nsd:*`, `ssl`) and sockets (`bsd:*`, over an in-process loopback) answer as a console with no network does. A game that opens one stops with a report naming the service (`rl_report_unknown`, or an error result under `RELIFT_STRICT`).
- Why: each is a stop for a class of games. Several can answer "not available" or "empty" truthfully and exactly, which is what the console does with no hardware or network.
- Where: `runtime/guests/switch/services.c`, `srv_*.c`, `ipc.c`; tests in `runtime/guests/switch/tests/test_srv_*.c`.
- Read first: switchbrew's service pages (command ids and structures), the SDK behavior of the game's own code when a service returns an error.
- Verify: a C test per command through `rl_svc` and IPC like the existing `test_srv_*.c`; `make -C runtime test`. Label any value you infer as inferred and cite where it came from.
- **Good first issue:** `pdm:*` (a report sink that accepts and discards; `prepo:*` in `srv_prepo.c` is the model).

#### Motion and rumble input (medium)

- Missing: the input model declares no motion ("Motion is not declared yet" in `docs/reference/input.md`), and `runtime/host/window_sdl3.c` reads buttons and axes only; no rumble or HD rumble reaches the pad.
- Why: games that read the gyroscope or rumble stop at the HID shared-memory or vibration commands, or play without feedback.
- Where: `core/input/src/native.rs`, `runtime/guests/switch/hid_priv.h`, `srv_hid*.c`, `runtime/host/window_sdl3.c`.
- Read first: switchbrew HID shared memory and sixaxis pages; SDL3's gamepad sensor and rumble API.
- Verify: a scripted sensor port (`RELIFT_INPUT_SCRIPT`) read back through the HID shared memory in a C test.

#### Small stops found by running programs (small each, check first)

Each of these was seen as a stop or a wrong answer in a program, and the cause was not chased. Re-check the current behavior first; a name search is all that was done.

- Thread suspend: no handler for `svcSetThreadActivity` (0x32) was found in `runtime/guests/switch` by a name search; a program that suspends another thread stops with an unknown-svc report. Where: `runtime/guests/switch/svc*.c`, `ksched.c`. Verify: a C test that pauses a thread and sees it not run.
- The loader service for NRO programs (`ldr:ro`) has no handler by a name search, so a program that loads a module at run time through the SDK's own loader stops. Where: `runtime/guests/switch/services.c`; `guests/switch/formats` already reads NRO files. Verify: a synthetic NRO loaded through the IPC commands.
- The shared font fallback: the `Nintendo extension` font (`pl:u`) has no copy shipped with relift, so a program that draws those symbols with no font in the import shows boxes. Where: `runtime/guests/switch/srv_pl*.c`, the fallback fonts found beside the program. Read first: the licensing of any font you would ship; relift ships only open fonts.
- `nvnTextureCompare` of a pointer that is not a texture the layer made stops with "not a texture" (`runtime/guests/switch/nvn/nvn_tex.c`), where the console's driver compares the bytes. Verify: a test that compares a texture with a copy of its object.
- `nvnProgramSetShaders` with a program whose code the layer cannot find an end for stops with "stage 0 has no end" (`nvn_draw.c`). The cause is open: a program in a pool the layer has not seen, or a terminator form the scan misses.
- Two Switch programs draw their picture into the top-left 1280x720 of a 1080p surface. The cause is unknown: the presenter's scaling, or the surface size the layer reports. Check first. Where: `runtime/guests/switch/vk`, `runtime/core/present*.c`.

#### 32-bit address spaces (medium)

- Missing: "The Switch guest does not support 32-bit address spaces (the core does)" (`runtime/README.md`, Known limits).
- Why: 32-bit programs, including first-party ones, import but cannot run.
- Where: `runtime/guests/switch/kmem.c`, `loader.c`; `guests/switch/guest/src/program.rs`; `cpus/arm32` (the lifter exists, no Switch-flavored guest uses it).
- Read first: switchbrew on process address space types and 32-bit processes; this also needs an AArch32 instruction set check on the Switch's Cortex-A57.
- Verify: a synthetic 32-bit NSO built in tests; `relift-check` for any new instruction form.
- Size note: likely large because it needs the ARM32 lifter on the Switch frontend.

#### Exact preemption (large)

- Missing: where Horizon preempts a thread at any instruction, the runtime preempts at the next svc or preemption point (`runtime/README.md`, Known limits).
- Why: lock-free spin code and timing-sensitive games can behave differently from the console.
- Where: `runtime/guests/switch/ksched.c`, `core/emit` (the preemption check), `docs/abi.md` section 14.
- Read first: the ABI document section on preemption; the cost of a check in call-free loops (they are exempt to keep vectorization).
- Verify: a synthetic two-thread program that depends on a higher-priority wake-up inside a call-free loop; a measured cost in the existing benchmarks.

#### Shared memory on 16 KiB host pages (medium)

- Missing: views that do not fill their host pages need the rest free, and a second view at another offset is refused while the first is mapped (`runtime/README.md`, Known limits).
- Why: Apple and some Android hosts use 16 KiB pages; a game that maps small shared objects at odd offsets stops.
- Where: `runtime/core/vm.c`, `runtime/guests/switch/kmem.c`; test with `RELIFT_HOST_PAGE=16384`.
- Verify: `switch_tests` with `RELIFT_HOST_PAGE=16384` and `65536`, plus new cases for the refused layouts.

### Switch 2

The NVN2 graphics layer and the services it needs are in the coordinate-first list; most of what is open there is in progress, so ask the maintainer before you touch it. Services shared with the Switch are in the Switch section.

#### The software keyboard applet (medium, in progress on branch `swkbd`)

- State: the applet is answered by relift itself (library applet storages, channels and events in `runtime/guests/switch/srv_am_*.c`) with a text-entry panel on the desktop and on phones, or a scripted answer from the settings (`text_entry.text`). It works for a program that asks for a name. Still open: on Android with the on-screen controller in use the panel has no renderer to draw on, so the settings' answer is used; text checks by the application and customized dictionaries are read but not exercised.
- Where: `runtime/guests/switch/srv_am_swkbd.c`, `runtime/core/textentry*.c`, `runtime/host/window_sdl3.c`.

#### Vertex attribute registers past the bound count (small, check first)

- Missing: `nvnCommandBufferBindVertexAttribState` with fewer attributes than a later program reads. relift keeps the register an earlier bind wrote (register persistence); this is inferred, because the GPU macro's code (method 0xe96) is not in any lifted function and was not found.
- Why: a wrong guess changes which attribute a program reads silently. The alternative is that the macro resets the rest.
- Verify: a capture of the draw described in the console-only values list; otherwise the entry stays labeled `inferred:`.

#### Shading-rate image and palettes (medium, in progress)

- Missing: the NVN shading-rate image and its palettes are accepted and drawn at full rate. Next is `VK_NV_shading_rate_image` on hosts that have it, which keeps the picture exact where the console's rate is full and saves work where it is not; a host without it stays at full rate.
- Where: `runtime/guests/switch/nvn`, `runtime/guests/switch/vk`.

#### GPU counter reports (medium, in progress)

- Missing: reports that read a counter at a later point than the command that wrote them are deferred (in progress). 15 counter types map to Vulkan queries; types 9 and 14 have no host counterpart and read 0.
- Verify: `switch_tests` nvn counters; a program that reads each type back.

#### The game's own DLSS, translated (large, paused)

- Missing: the translated DLSS network runs on hosts that pass the device probes (`picture.upscaler=game`), but one kernel hangs NVIDIA GPUs; the cause is not known and it has not been run on those devices since. A program whose kernels the build cannot prove safe must not run on an NVIDIA device. Cooperative-matrix HMMA is in progress on branch `coopmat` (below).
- Where: `gpus/nv`, `gpus/sm86`, `runtime/guests/switch/nvn/nvn_dlss.c`.
- Verify: `kat/` programs and `scripts/ci/mesh-kat.sh`; a supervised run on a device whose reset is cheap.

#### Cooperative-matrix HMMA where bit-exact (medium, in progress on branch `coopmat`)

- Missing: the HMMA instruction runs through a helper function that is exact and slow. `VK_KHR_cooperative_matrix` is used where a device probe (`hmmaCooperativeMatrix`) proves the layout and the arithmetic; the translator handles the 16x8x16 and 16x8x8 half forms only, and the inlined form was larger than the helper, so it is one helper per shape.

#### Speed of a large game on the Switch 2 path (large, in progress)

- State: a Switch 2 program with a large open world runs GPU-bound at about 15% of the console's speed on a desktop GPU; where the GPU time goes is being profiled. The NVN memory mirror (`nvn.memory`, a Vulkan buffer per pool instead of importing the guest's pages) and the RenderDoc frame capture trigger (`RELIFT_RENDERDOC_CAPTURE`) are landing with it.

#### Dull specular highlights on characters (small, in progress)

- State: a Switch 2 program shows flat highlights on characters where the console shows sharp ones; the lighting translation is being checked.

#### A moved or bare build binary loses its data (small)

- Missing: a built Switch 2 program that is copied away from its build folder, or built as a bare binary, no longer finds its romfs, shader table or pipeline cache, and then fails late or draws nothing. It should find its data, or stop at start with a message naming what is missing (the romfs map's source file already stops clearly).
- Where: `runtime/guests/switch/loader.c`, `runtime/guests/switch/nvn`, `relift/src/` (what `relift build` writes beside the program).
- Verify: build a program, move it, run it: either it runs or it stops at once and says which file it wants.

#### Android game builds (large, waits)

- Missing: Switch 2 programs do not build for Android yet; the work waits until desktop performance is acceptable.

#### Console-only values (small to medium, needs a capture)

- Missing: a few NVN2 and service values cannot be settled from the SDK, switchbrew or the game's code. Where the code picks one, its doc comment says `inferred:` with the source.
- Why: a wrong inferred value is a silent accuracy bug. A capture from real hardware settles it.
- Where: search the C and Rust for `inferred:`.
- Verify: only a hardware capture settles it; otherwise leave the value labeled.

### Game Boy and Game Boy Color

#### Unsupported cartridge mappers (small to medium each)

- Missing: `runtime/guests/gb/cart.c` maps these header types to `RL_GB_MBC_UNSUPPORTED`: MMM01 (0x0B to 0x0D), MBC6 (0x20), MBC7 (0x22, with its accelerometer and EEPROM), the Pocket Camera (0xFC), TAMA5 (0xFD), HuC3 (0xFE) and HuC1 (0xFF). Import of such a ROM reports an unsupported mapper.
- Why: several licensed games use them; MBC7 and HuC3 need new input and real-time-clock or sensor devices.
- Where: `runtime/guests/gb/cart.c`, `mbc.c`, `mem.c`, `rtc.c`, tests in `runtime/guests/gb/tests/test_mbc.c`; `guests/gb/formats`. The devices come from rustyboi-core in `guests/gb/devices`; check whether the device library also needs the mapper.
- Read first: Pan Docs (the MBC pages), gbdev's pages on the less common mappers, SameBoy (MIT) as a behavior reference.
- Verify: synthetic cartridges in `test_mbc.c` for the register map; public mapper test ROMs through `scripts/ci/gb-corpus.sh`.
- **Good first issue:** MMM01 (a banking-only mapper, no new device).

#### Link cable, printer and infrared (medium, check first)

- Missing: not checked. The serial port is a scheduler device slot (`runtime/guests/gb/io.c`), and the built program has no way to connect two programs or a printer.
- Why: link-cable features (trading, two-player) cannot be used.
- Where: `runtime/guests/gb/io.c`, `runtime/guests/gb/gbdev.c`, `runtime/guests/gb/exact/gbmain.c`.
- Read first: Pan Docs on serial transfer; decide a transport (a socket, a pipe) with the maintainer first.
- Verify: two headless programs exchanging a byte; the serial test ROMs in the public suites.

#### Super Game Boy features (small, check first)

- Missing: the `sgb` and `sgb2` hardware models exist (`runtime/guests/gb/postboot.c`); check whether the command packets (borders, palettes, multiplayer) are handled.
- Where: `guests/gb/devices`, `runtime/guests/gb/gbdev.c`.
- Verify: the SGB test ROMs in the public corpus (`samesuite_sgb` is already a CI case); add cases for commands.

### Game Boy Advance

#### The sound unit (large)

- Missing: the GBA machine has no sound unit. `runtime/guests/gba/fifo.c` models the two Direct Sound FIFOs only as far as they time the sound DMA; the bytes are never played, the four PSG channels are register storage, and `runtime/guests/gba/run/gbamain.c` has no audio output or `--wav`.
- Why: a GBA program is silent. Sound is the largest missing piece of the machine.
- Where: `runtime/guests/gba/io.c` (the `apu` slot, `RL_GBA_SLOT_APU`), `fifo.c`, `runtime/guests/gba/run/gbamain.c`; model it on the NDS sound unit (`runtime/guests/nds/sound.c`) and the Master System PSG (`runtime/guests/sms/psg.c`).
- Read first: GBATEK sound sections; ares (ISC) for ported behavior with its notice per file.
- Verify: the NanoBoyAdvance oracle already reports audio hashes (`relift-check --oracle nba rom`); compare sample for sample on public test ROMs, as the PSG is compared with ares (`check/oracle/ares/psg-compare.py`).

#### Cartridge GPIO: clock, solar sensor, gyroscope, tilt, rumble (medium)

- Missing: no handling of the cartridge GPIO port was found in `runtime/guests/gba` or `guests/gba/formats` (a name search for GPIO and RTC).
- Why: games with a real-time clock or a sensor read garbage or stop.
- Where: `runtime/guests/gba/mem.c`, `cart` handling in `runtime/guests/gba`, `guests/gba/formats`.
- Read first: GBATEK "GBA Cartridge GPIO Port"; the input model needs a sensor port for tilt and gyro.
- Verify: a synthetic ROM driving the port; the oracle's RTC behavior through `relift-check --oracle nba`.

#### Serial and link cable (medium)

- Missing: the serial slot is a register file with masks (`runtime/guests/gba/io.c`, `serial_set`): no transfer happens, no interrupt fires.
- Why: link features, multiboot from another unit, and wireless adapters cannot work.
- Read first: GBATEK serial communication sections; decide the transport with the maintainer.
- Verify: two headless programs exchanging data; the oracle's serial behavior.

#### Sound-driver BIOS calls and the remaining SWIs (small each)

- Missing: the high-level BIOS (`runtime/guests/gba/bios/swi.c`) handles SWIs 0x00 to 0x19, 0x1F and 0x25 to 0x27. The sound driver calls (0x1A to 0x1E) and the numbers in between and after are not implemented: `rl_report_unknown` notes the call and does nothing (`runtime/guests/gba/run/cpu.c`).
- Why: a game that uses the BIOS sound driver loses its sound or misbehaves.
- Read first: GBATEK BIOS function pages only. The BIOS is high-level by design: never read, disassemble or translate a console BIOS dump; effects at call entry and exit (registers, memory written, cycles, IRQ timing) come from public documents and console timing records.
- Verify: hand-built inputs per call in `runtime/guests/gba/tests`, as the other SWIs have.

#### Missed code: hot-load and discovery recall (medium)

- Missing: missed code stops the program with a report; hot-loading is not available for this guest (`guests/gba/README.md`). Discovery recall on public test ROMs and homebrew is not measured in CI.
- Why: every miss is a stop. A measured recall number keeps regressions out.
- Where: `guests/gba/analyze`, `cpus/arm32/analyze`, `relift/src/gba.rs`, `relift/src/dev.rs`.
- Verify: executed-instruction traces from an oracle compared with discovery's list in `check/` only; zero false code (no static instruction overlapping an executed one at another offset).

#### Timing accuracy (medium to large, open ended)

- Missing: the BIOS call costs are approximate, fitted to console records; bus timing is replayed against 365 console records and the NanoBoyAdvance oracle, but PPU memory-access stalls and the prefetch buffer are modeled to that oracle's level.
- Where: `runtime/guests/gba/contend.c`, `dma.c`, `ppu/`, `bios/`; checks in `runtime/guests/gba/tests`.
- Read first: GBATEK timing sections; hardware test ROMs (a public timing suite) with console results where they exist.
- Verify: new console timing records or public timing ROMs as test vectors.

### Nintendo 64

Gaps outside the coordinate-first RSP timing work.

#### Controllers 2 to 4 and accessories (medium)

- Missing: only port `p1` exists (`core/input/src/native.rs`). The Controller Pak is absent (the pad answers pak commands as a console with none, `runtime/guests/n64/pad.c`), and so are the Rumble Pak, Transfer Pak, mouse, keyboard and VRU.
- Why: multiplayer games and games that save to a pak cannot be played.
- Where: `runtime/guests/n64/pif.c`, `pad.c`, `save.c`; `core/input/src/native.rs`; the generated `runtime/guests/n64/ports_gen.c`.
- Read first: n64brew's joybus and PIF pages, libdragon's controller code (public domain) for protocol behavior.
- Verify: `runtime/guests/n64/tests/test_si.c` additions; a public joybus test ROM.
- **Good first issue:** controllers 2 to 4 as ports with the same standard pad (small).

#### Boot variants and checksums (small)

- Missing: the checksum check is computed only for four boot variants (6101, 6102, 6103, 6106); the others report `not checked`, and a boot code whose hash is unknown boots through the high-level path (`guests/n64/README.md`).
- Where: `guests/n64/formats/src/cic.rs`, `runtime/guests/n64/machine.c` (`rl_n64_boot`).
- Read first: n64brew's CIC and boot pages.
- Verify: synthetic headers in `guests/n64/formats` tests; the PIF boot state per variant in `runtime/guests/n64/tests`.
- **Good first issue.**

#### RDP exactness and timing (large; coordinate)

- Missing: the RDP runs on parallel-rdp (`runtime/guests/n64/rdp/prdp.cpp`) at the picture level; the command timing that decides when DP interrupts and the full sync land is an open design question.
- Why: control flow of games that wait on the RDP depends on it.
- Where: `runtime/guests/n64/rdp`, `runtime/guests/n64/sp.c`.
- Verify: the ares software RDP as oracle (`check/oracle/ares`), RDRAM hashes at fixed frames under scripted input.

#### 64DD and other peripherals (large)

- Missing: nothing under `runtime/guests/n64` or `guests/n64` handles the 64DD.
- Where: new files in `runtime/guests/n64`, `guests/n64/formats`.
- Read first: n64brew's 64DD pages; this needs a new cartridge format reader.
- Verify: synthetic disk images in format tests.

#### TLB-mapped code discovery (small, check first)

- Missing: not checked. The analyzer follows the boot code and the DMA fixed point; verify what it does with code that runs through TLB-mapped addresses, and add a synthetic ROM that does.
- Where: `guests/n64/analyze`, `runtime/cpus/mips` (the TLB).

### Nintendo DS

The discovery and 3D work is in the coordinate-first list; these are the rest.

#### Wi-Fi (large)

- Missing: only the power and wait-state registers exist (`runtime/guests/nds/sysreg.c`, `io.c`); the Wi-Fi unit has no registers or MMIO window, though `RL_NDS_IRQ_WIFI` exists in `nds.h`. The firmware image's Wi-Fi calibration block is generated (`firmware.c`).
- Why: games that probe the unit or use local multiplayer stop or misbehave.
- Where: `runtime/guests/nds/`, a new `wifi.c`; ARM7 MMIO map in `io.c`.
- Read first: GBATEK "DS Wireless Communications"; the network transport is a design decision for the maintainer.
- Verify: register behavior against public Wi-Fi test ROMs and GBATEK; an RF-less "no peers found" path first.

#### Microphone (small to medium, check first)

- Missing: a name search found no microphone input. The SPI bus has the power, firmware and touch controller devices (`runtime/guests/nds/spi.c`); check whether the touch controller's auxiliary and microphone channels return a plausible level, and add a host capture.
- Read first: GBATEK "DS Touch Screen Controller (TSC)" and microphone pages.
- Verify: a synthetic ROM that samples the channel; the host side through the SDL audio input.

#### DSi mode (large)

- Missing: relift imports and runs DS cartridges only; a name search found no DSi mode (new CPU features, memory, SD card, camera, 3DS-style SoC registers).
- Where: `guests/nds/formats` (header fields), `runtime/guests/nds`, `guests/nds/guest`.
- Read first: GBATEK DSi sections; this is a large plan, discuss scope in an issue.

#### Backup chip coverage (small, check first)

- Missing: `runtime/guests/nds/backup.c` models EEPROM, FRAM and FLASH by command behavior and the `save.type` setting. Check the chip list in GBATEK against it (Flash sizes, infrared cartridges with their own backup commands, NAND-style backup in the few cartridges that use it).
- Verify: command-level tests in `runtime/guests/nds/tests`, and the DS end-to-end tests in `relift/tests/e2e_nds.rs`.
- **Good first issue** (a table audit with tests).

#### Cartridge slot accessories (small to medium)

- Missing: the GBA slot is open bus (`runtime/guests/nds/mem.c`); no rumble pak, RAM expansion or other slot-2 devices.
- Read first: GBATEK "DS Cartridge Slot 2"; useful for games that detect the slot.

#### Public test ROM coverage (medium)

- Missing: the only NDS test ROM in CI is the timing ROM built with devkitPro (`scripts/ci/nds-timing-sdk.sh`); there is no CPU, DMA, IPC or GPU public-ROM job.
- See "Test-ROM coverage" in section 5.

### Master System, Game Gear and SG-1000

#### Code discovery (large)

- Missing: there is no `guests/sms/analyze` crate. `relift build` takes its code from the executed-instruction trace of an oracle (`RELIFT_SMS_TRACE`) and fails without one, so no ROM builds on its own.
- Why: it blocks every user of this guest.
- Where: new `guests/sms/analyze`, modeled on `guests/gb/analyze` (banks, trampolines, tables) over the lifted Z80 IR; the consumer is `guests/sms/guest` (`code::Code` is the report's shape) and `relift/src/sms.rs`.
- Read first: SMS Power's mapper and memory control pages, the Z80 user manual for flow, `cpus/z80/README.md`.
- Verify: recall on public test ROMs and homebrew against oracle traces in `check/oracle/ares` (traces are only a check, never an input), 0 false code; then CI jobs like `z80-zexdoc`.
- Known hard cases: bank numbers read from RAM tables, computed jumps through index registers, byte-loop copies to RAM, return-address tricks.

#### RAM code whose opcode bytes change (medium)

- Missing: a program that rewrites opcode bytes in RAM while it runs stops with a report (`guests/sms/README.md`); operands read live are already supported.
- Where: `guests/sms/guest`, `runtime/guests/sms/run`, `core/emit` entry checks, the variants approach used by the GBA guest (`rl_gba_cpu_variant`).
- Verify: a synthetic ROM that patches an opcode in RAM, against the oracle's trace.

#### More mappers, SC-3000 and peripherals (medium)

- Missing: the mappers are Sega, Codemasters and Korean (`guests/sms/formats`); `import` takes `.sms`, `.gg` and `.sg` only. There is no SC-3000 keyboard, light phaser, paddle or sports pad (`core/input/src/native.rs` declares one pad).
- Read first: SMS Power's hardware pages.
- Verify: synthetic ROMs for each mapper and port; public peripheral test ROMs.

---

## 2. CPU and GPU translators

### A64: forms outside the generated set (small to medium)

- Missing: `cpus/arm64/gen/forms.txt` lists 1,518 encodings; FMULX and FRECPX are absent. A form joins the list only after `relift-check forms` passes it and its mutation control on an oracle that implements the feature. Other forms of the Armv8.0 profile may be missing; compare the decoder's encoding names with the list.
- Why: an encoding with no template lifts through the hand-written lifter or stops.
- Where: `cpus/arm64/gen`, `cpus/arm64/src/lift`, `check/`.
- Read first: Arm's AARCHMRS and sail-arm (the generator inputs), `cpus/arm64/gen/README.md`.
- Verify: `scripts/ci/all.sh generated`; `relift-check forms <name>` with `--mutate` against QEMU or Unicorn.

### SM53: Maxwell lowering coverage (large; coordinate)

- Missing: most Maxwell forms in `gpus/sm53/LOWERING.md` still stop with "no semantic check yet"; the table is the current count.
- Why: Switch 1 shaders stop at the first unlowered form.
- Where: `gpus/sm53/src/lower`; the table test `RELIFT_BLESS=1 cargo test -p relift-gpu-sm53 --test lowering`.
- Verify: each form needs evidence of identical behavior on both GPUs, as the table's reasons describe.

### SM86 translator (large; coordinate)

- Missing: what the translator cannot reproduce on a host is rejected or listed in `HostReport::inexact` (`gpus/nv/README.md`); check the open cases there before you start.
- Verify: `kat/` known-answer programs on a real GPU and on lavapipe (`scripts/ci/mesh-kat.sh`), `spirv-val`.

### Shader scan: more containers (medium)

- Missing: `relift-shaders scan` knows a fixed set of containers (`guests/switch/shaders/src/scan.rs`). A game with another container reports no shaders.
- Where: `guests/switch/shaders/src`.
- Verify: synthetic containers built in the tests; no game data.

### Vulkan forwarding gaps (medium)

- Missing: `runtime/guests/switch/vk/README.md` ("Not done yet") lists commands taking file descriptors or platform handles, image-backed host-visible memory on the import path and guest callbacks from driver threads. Its statement that `relift build` does not compile this folder is out of date (see the docs entry).
- Where: `runtime/guests/switch/vk`, generated types by `relift-vkgen`.
- Verify: `switch_vk_tests` on lavapipe and a real driver.

---

## 3. Runtime and hosts

### Windows arm64: built, never run (medium)

- Missing: Windows arm64 programs for the Game Boy, GBA, N64 and Master System guests build in CI (`cross-*-windows-arm64`) but no job runs one.
- Where: `.github/workflows/cross.yaml`, `scripts/ci/gbcross.sh`, `runtime/core/fiber_aarch64.S` (Windows branch), `runtime/core/crash_win.c`.
- Read first: the Windows arm64 ABI notes on x18 and the TEB, GitHub's `windows-11-arm` runner.
- Verify: a CI job that runs the cross-built program and the portable runtime tests on an arm64 runner.

### Windows: Switch shared memory (large)

- Missing: `rl_vm_map_shm` returns -1 on Windows (`runtime/core/vm.c`), so a Switch program cannot map its shared memory objects.
- Why: the Switch guest cannot run on Windows.
- Where: `runtime/core/vm.c`, `runtime/guests/switch/kmem.c`.
- Read first: `VirtualAlloc2` placeholders and `MapViewOfFile3`; views are 64 KiB aligned, so the alias granule differs from the 4 KiB protection page (see `docs/abi.md` section 2).
- Verify: `switch_tests` shared-memory cases under wine and on a Windows runner.

### Windows: the runtime test suite (medium)

- Missing: `make portable` builds three of the test binaries for Windows; the rest of the suite uses POSIX (`fork`, `sys/wait.h`), and the runtime README says the rest of the core is not ported there yet. Verify the current state by building `make -C runtime test` for a Windows target.
- Where: `runtime/core/tests/runner.c`, `runtime/Makefile`.
- Verify: all layer test binaries pass under wine in the `cross` job.

### Windows: commit accounting (small, check first)

- Missing: not measured. `os_protect` in `runtime/core/vm.c` falls back to committing the whole range when a protect fails on reserved memory, so large guest heaps may count against the commit limit at once.
- Verify: a test that maps a 4 GiB region, touches one page and reads the process's committed size.

### macOS x86-64 (medium)

- Missing: Game Boy, GBA, N64 and Master System programs build in CI and a GBA program ran under Rosetta; Switch and DS programs on this target have not been tried, and no job runs any program.
- Where: `.github/workflows/cross.yaml` (`macos-x64`), `runtime/Makefile`.
- Verify: a CI job on an Intel runner (while one exists) or under Rosetta that runs the programs.

### Linux arm64 (small)

- Missing: relift builds for Linux arm64 in CI (`image-aarch64-unknown-linux-gnu`); no job builds the runtime or runs programs there.
- Verify: a CI job on an arm64 Linux runner running `make -C runtime test` and a built test program.
- **Good first issue.**

### Android: the on-screen controller for more guests (medium)

- Missing: on-screen layouts exist for the Switch, Game Boy and GBA (`runtime/guests/*/touchpad.c`); there are none for the N64, DS or Master System family, though their ports exist.
- Why: those apps need an external controller on a phone.
- Where: `runtime/core/touchpad*.c`, a new `touchpad.c` per guest, `docs/reference/input.md`.
- Read first: `docs/reference/input.md` (the on-screen controller section); the existing layouts as templates. Layouts are a design: propose a mockup in the issue.
- Verify: `runtime` tests for placement and touch rules; screenshots from a device.

### Variable refresh rate presentation (medium, owed)

- Missing: the presenters pace to a fixed 60 Hz vsync grid; no host asks for variable refresh (adaptive sync or each platform's equivalent).
- Where: `runtime/core/present*.c`, `runtime/host/window_sdl3.c`, the NVN present in `runtime/guests/switch/nvn`.
- Verify: a presented-frame timing log on a display that has the feature; the picture bit-identical.

### Progress screen during shader warm-up (small, owed)

- Missing: the launch warm-up of a Switch program's shaders (see the shader warm-up page in the docs) runs with a blank window on every host; it needs a progress screen with a count, drawn by the presenter before the game's first frame.
- Where: `runtime/core/present*.c`, `runtime/guests/switch/nvn` (the warm-up), `runtime/host`; on Android the activity.
- Verify: a headless run reports the progress events; a screenshot of the screen on each host.

### Android and iOS: packaging polish (medium)

- Missing: check the packaging guide against the build: Android and iOS apps of the DS and Master System guests, an exported `.ipa`, store-ready metadata and a release signing flow.
- Where: `relift/src/android.rs`, `relift/src/ios.rs`, `docs/guides/packaging.md`.
- Verify: `relift build --target android-arm64` and `ios-arm64` on a game you own; `apksigner verify`, `codesign --verify`.

### Android x86-64 and iOS simulator in CI (small)

- Missing: the cross matrix builds `android-arm64` and `ios-arm64` only, though `relift build` also takes `android-x64` and `ios-sim-arm64`.
- Where: `.github/workflows/cross.yaml`, `scripts/ci/gbcross.sh`, `runtime/tools/cross/gbcross.py`.
- Verify: the new matrix rows pass under 15 minutes.
- **Good first issue.**

### Cross builds for the Switch (small)

- Missing: the cross matrix covers `gb`, `gba`, `n64`, `nds` and `sms`; the Switch runtime is not built for Windows, Android or iOS in CI.
- Where: `.github/workflows/cross.yaml`, `scripts/ci/all.sh`, `runtime/tools/cross/gbcross.py`.
- Verify: new `cross-nds-*` jobs; the release guard (`scripts/ci/release-guard.sh`) on each.

### Web: a WebAssembly host (large)

- Missing: `core/emit-wasm` exists with a differential test against the IR interpreter, but `relift` does not link it and no runtime or shell targets WebAssembly.
- Why: a browser build would need no install.
- Where: `core/emit-wasm`, `core/emit`, `docs/abi.md` section 15; a runtime for a 32-bit host.
- Read first: the ABI notes on single-memory modules; the WebAssembly proposals for stack switching (the DS needs two CPUs).
- Verify: the existing differential tests, then a Game Boy test ROM run in a browser or node.

---

## 4. Player features

### Anti-aliasing beyond FXAA and SMAA (medium)

- Missing: `picture.aa` offers off, FXAA and SMAA (`runtime/core/post.c`, `runtime/third_party`); there is no MSAA; a temporal choice is part of the upscaler work below. The Nintendo 64 picture options (`n64.picture.*`) use parallel-rdp's own upscale and supersample controls.
- Why: players of the Switch games that render through Vulkan have only the post filters.
- Where: `runtime/core/post.c`, `runtime/core/include/relift/post.h`, `runtime/guests/switch/vk`.
- Read first: how the picture pipeline runs on the host's compute queue; temporal anti-aliasing needs motion vectors and jitter that only some guests provide, so MSAA on the guest's own render passes may be the practical path. Every option stays off by default and says it changes the picture.
- Verify: golden images (`--shown-png`) with the option on and off, and bit-identical output with it off.

### Upscalers: temporal and vendor (medium, in progress)

- State: the upscaler stage (`runtime/core/upscale.c`, `picture.upscaler`) has relift's spatial choices (nearest, bilinear, sharp bilinear, Lanczos) and AMD FSR 1, in the frame at the Switch 2 driver's DLSS calls and in the window. Temporal upscalers are in progress on a branch: FSR 2 (the default for now), FSR 3.1, Arm ASR, a temporal anti-aliasing choice and MetalFX on Apple hosts. NVIDIA DLSS and Intel XeSS are loaded at run time from the vendor's library on the host only and never shipped. The game's own translated DLSS is under Switch 2.
- Why: lower internal resolutions on weak hosts.
- Where: `runtime/core/upscale.c`, `runtime/core/include/relift/upscale.h`, `runtime/core/shaders`, `runtime/guests/switch/nvn/nvn_dlss.c`.
- Read first: AMD FSR 1 and 2 (MIT) for the existing ports; check the license of any other before you write code. The temporal ones need motion vectors, depth and jitter, which only a guest that passes them (Switch 2's DLSS call) provides.
- Verify: unit tests for the stage contract in `runtime/core/tests/test_post.c`; golden images; the pipeline never starts with every option off.

### Save states (large)

- Missing: nothing in `runtime` saves or loads machine state; only the checker uses the Game Boy device library's own state format (`check/src/gb/machine.rs`).
- Why: a common player feature, and useful for faster test setup.
- Where: the scheduler (`runtime/core/sched.c`, fibers), each CPU context, each guest machine.
- Read first: design first in an issue. Lifted code runs on host stacks, so a state can be taken only at a sync point or frame edge where no host frame holds guest state; the hardware-register guests (GB, GBA, SMS, N64, DS) are the realistic start.
- Verify: save, load and compare frame hashes with an uninterrupted run, for each guest.

### Input remapping UI (medium)

- Missing: bindings are settings (`bind.<port>.<control>`); the Android on-screen controller has a layout editor, but desktop has no in-game screen for remapping keys and pads (no rebind code in `runtime/core` or `runtime/host`).
- Where: `runtime/core/input.c`, `runtime/host/window_sdl3.c`, `core/input`, settings write-back in `runtime/core/settings.c`.
- Read first: `docs/reference/input.md`.
- Verify: a scripted capture test; the setting written back to the settings file as the touch pad's layout is.

### A launcher or frontend (large)

- Missing: relift builds game programs; there is no launcher in this repository. A built program describes itself (`--relift-info`, `relift info`: identity, settings schema, input ports, mods, save), which is the interface a launcher reads.
- Why: a library, per-game settings screens and a controller setup need one program for every guest.
- Where: `core/info`, `relift/src/info.rs`, `docs/reference/configuration.md`. Decide with the maintainer whether it lives in this repository (it then needs a charter row) or in its own.
- Verify: a launcher run against the self-descriptions of a synthetic program of each guest.

---

## 5. Tooling, CI and docs

### Test-ROM coverage per guest

#### Game Boy corpus growth (small)

- Missing: `runtime/guests/gb/devtests/ci_cases.toml` has 42 cases from public suites (one or a few per suite); the full corpus runs locally with `corpus.sh`.
- Where: `runtime/guests/gb/devtests`, `scripts/ci/gb-corpus.sh`.
- Verify: new cases pass under the 15-minute limit.
- **Good first issue.**

#### GBA public suites in CI (medium)

- Missing: no job runs a public GBA test suite through relift. The checks are synthetic ROMs and the oracle comparisons.
- Where: a new `scripts/ci/gba-corpus.sh` modeled on `gb-corpus.sh`; `relift/tests/e2e_gba.rs`.
- Read first: the licenses of the available suites (some are MIT, some copyleft); each ROM is fetched by hash.
- Verify: pass and fail protocol per ROM, and that each ROM failing is caught.

#### N64 public suites in CI (medium)

- Missing: public CPU, RSP, VI and RDP ROMs match the oracle in test runs, but no CI job runs them.
- Where: a new `scripts/ci/n64-corpus.sh`; `relift/tests/e2e_n64.rs` (synthetic only).
- Verify: RDRAM and picture hashes against the ares oracle's recorded values; licenses first.

#### Nintendo DS public suites in CI (medium)

- Missing: see the DS entry; only the timing ROM runs in CI.
- Where: a new `scripts/ci/nds-corpus.sh`.

#### Z80 and Master System ROMs (small)

- Missing: `z80-sst` and `z80-zexdoc` check the CPU; no public VDP or SMS test ROM job exists.
- Where: `scripts/ci/`, `relift/tests/e2e_sms.rs`, `relift/tests/e2e_sg.rs`.

### CI jobs

#### Compile the SDL3 audio host for Windows, macOS and Android (small)

- Missing: `runtime/host/audio_sdl3.c` is built only with `AUDIO=real` and no CI job compiles it for any target, so a Windows, Apple or Android break goes unnoticed.
- Where: `runtime/Makefile`, `runtime/tools/cross/gbcross.py`, `.github/workflows/cross.yaml`.
- Verify: a job that compiles it for each target; a deliberate error fails it.
- **Good first issue.**

#### shellcheck over `scripts/ci` (small)

- Missing: `scripts/ci/workflows.sh` runs actionlint with shellcheck turned off (`-shellcheck=`), and no job runs shellcheck on `scripts/ci/*.sh`.
- Where: `scripts/ci/workflows.sh`, `.github/workflows/ci.yaml`.
- Read first: pin a shellcheck release by version and SHA-256 in `scripts/ci/lib.sh`, as the other tools are; fix what it finds rather than suppressing.
- Verify: the job passes, and fails on a planted quoting error.
- **Good first issue.**

#### Windows arm64 link with a newer Rust (small, check first)

- Missing: reported but not reproduced here: a newer Rust toolchain makes the Windows arm64 link fail because the standard library calls `WaitOnAddress`, which needs `synchronization.lib`. The Windows link lists in `relift/src/gb.rs`, `gba.rs`, `n64.rs` and `build.rs` name `ws2_32`, `userenv`, `ntdll` and `bcrypt` but not `synchronization`.
- Why: it blocks the Renovate toolchain bump for the cross jobs.
- Verify: reproduce with the toolchain bump, add the library, and watch `cross-*-windows-arm64`.

### Commands

#### `relift check` and `relift where` (small to medium)

- Missing: both parse and stop with "not implemented" (`relift/src/main.rs`; `docs/guides/usage.md` says so). `relift-check` is a separate program in `check/`.
- Where: `relift/src/cli.rs`, `relift/src/main.rs`; `check/README.md` for the checker.
- Verify: `relift/tests/cli.rs` cases. `where` maps a host address, a guest address and a symbol (see `relift/src/report.rs` and the runtime's guest-name mapping).

#### `relift coverage` and `relift diff` for every guest (medium)

- Missing: both are Switch-only; other guests use `relift-gbrom functions`, `relift-gbarom functions` and the N64 and DS tools.
- Where: `relift/src/coverage.rs`, `relift/src/diff.rs`, each guest's `tools`.
- Verify: `relift/tests/coverage.rs` cases on synthetic ROMs.

### Documentation

#### Pages and stale statements (small each; good first issues)

- Missing: `guests/nds` has no README, and the site outline (`docs/nav.md`) omits the N64 and Master System guest pages, the DS, `cpus/z80`, `cpus/rsp` and the `check` guest tables. The README's tool list names only `relift-gbrom` and `relift-gbarom`, not `relift-n64rom`, `relift-ndsrom`, `relift-smsrom` or `relift-shaders`. `docs/guides/usage.md` and `docs/start/03-first-game.md` say `import` takes Switch dumps and `.gb`, `.gbc`, `.gba` ROMs only.
- Stale statements to fix: `guests/n64/README.md` opens by saying the RDP and audio are not there, while the same page and the runtime document both; `cpus/arm32/README.md` says no guest links the ARMv5TE profile, and the DS guest does; `runtime/guests/switch/vk/README.md` says `relift build` does not compile that folder or `runtime/host`, and `runtime/README.md` says it does; the `check/README.md` table lists four guests. `docs/reference/input.md` native ports table lists no N64 or Master System ports.
- Where: the files named; `docs/nav.md`.
- Verify: `scripts/ci/all.sh docs`. Facts that come from the code go through docgen markers.

---

## 6. New guests

A new guest is a new folder under `guests/`, a CPU description in `cpus/` when none exists, and runtime devices under `runtime/`. The Game Boy guest is the model for a small hardware-register guest: `cpus/sm83` (decoder, opcode table, lifter, `sail/sm83.sail` and a `gen` crate that generates the templates), `guests/gb/{formats,analyze,guest,tools}`, `runtime/cpus/sm83` and `runtime/guests/gb`, with the corpus job in `scripts/ci/gb-corpus.sh`. Each new guest needs charter rows (`relift/tests/workspace_charter.rs`), a registry entry in the `relift` crate, a README, and the zero-interpreted-instructions goal from the start. Open an issue with a plan before you write code; a new guest is several PRs.

Where no Sail model of a CPU exists, relift writes its own: `cpus/sm83/sail/sm83.sail` and `cpus/z80/sail/z80.sail` are our own, on the shared Sail frontend `core/sail`, while `cpus/mips` and `cpus/arm32` come from published models plus an overlay.

### WebAssembly (large, not started)

- Missing: relift lifts no WebAssembly module. This is the guest that runs `.wasm` programs, apart from the host entry in section 3 that would run relift's own output in a browser.
- Plan: a `guests/wasm` crate with a module reader and validation, lifting of the instruction set to relift's IR (structured control flow maps directly, linear memory is one region, imports are a host-call table), and a runtime layer with the WASI calls a program needs. Zero interpreted instructions as for every guest.
- Read first: the WebAssembly core specification and its test suite (Apache-2.0); `core/emit-wasm` for the opposite direction.
- Verify: the specification's tests run through a build, with output equal to a reference engine's.

### NES (large)

- Missing: everything. There is no 6502 anywhere in the tree.
- Plan: a 6502 CPU crate in `cpus/` (for example `cpus/m6502`) with its own Sail model and generator, without decimal mode as on the 2A03; `guests/nes/{formats,analyze,guest,tools}` with iNES and NES 2.0 import; `runtime/cpus/m6502` and `runtime/guests/nes` for the PPU, APU, controllers and mappers as catch-up devices on the scheduler.
- Read first: the nesdev wiki (CPU, PPU timing, APU, mapper pages), the 6502 and 2A03 behavior notes (unofficial opcodes are a decision point).
- Verify: SingleStepTests 6502-style known answers for the CPU (check its license), public nesdev test ROMs (blargg's suites and similar) through a `nes-corpus` job, frame hashes against an oracle in `check/`, and the standing zero-interpreted-instructions goal.

### Sega Genesis and Mega Drive (large)

- Missing: no 68000 and no guest. `cpus/z80` and `runtime/cpus/z80` already exist and cover the sound processor.
- Plan: a 68000 CPU crate (a published Sail model under a permissive license plus an overlay is the route others took), `guests/genesis`, and runtime devices for the VDP, YM2612, PSG, mappers and the two-CPU bus arbitration; the Z80 program is uploaded at run time, which is a discovery problem.
- Read first: Sega Retro and the plutiedev hardware pages, the 68000 programmer's manual, ares (ISC) as an oracle and a source for ported device behavior.
- Verify: 68000 known-answer sets (check licenses), public Genesis test ROMs, frame and audio hashes against ares.

### Sony PlayStation (large)

- Missing: `cpus/mips/src/r3000a` is a stub (the MIPS I decode table and the integer class) with no runtime layer, no GTE (the geometry coprocessor), and no guest.
- Plan: finish the `r3000a` profile (load delay slots, no FPU, no TLB, the GTE as COP2), a `runtime/cpus/mips` profile layer, then `guests/ps1` with the GPU, SPU, CD-ROM, DMA and memory card as devices; the BIOS is high level.
- Read first: nocash PSX specs (psx-spx), `cpus/mips/README.md` (it notes what an R3000A layer adds).
- Verify: public CPU and GTE test ROMs, an emulator oracle in `check/`.

### Super NES (large)

- Missing: no 65816, no SPC700, no guest.
- Plan: two CPU crates and a two-processor machine like the DS's (`runtime/guests/nds/order.c` shows the ordered scheduler), the PPU and DSP, cartridge coprocessors later.
- Read first: the SNES development wiki, anomie's docs.
- Verify: public CPU test ROMs and SingleStepTests-style sets, frame and audio hashes against an oracle.

### Nintendo 3DS (large)

- Missing: no ARMv6K profile in `cpus/arm32` (the generator is sail-arm's AArch32 part plus overlays), no guest.
- Plan: an operating-system-service guest like the Switch (kernel objects and services replaced, the Switch's design carries over), an ARM11 profile, and a PICA200 graphics translator to Vulkan. Reference emulators are GPL: read them for facts only.
- Read first: 3dbrew.
- Verify: public homebrew test programs, and checks like `relift-check` for the CPU profile.

### Later systems (large each, no plan yet)

Dreamcast (SH-4 and PowerVR), GameCube and Wii (PowerPC), PlayStation 2 (R5900 MIPS and the vector units), Xbox 360 and PS3 (PowerPC with vector extensions) and Wii U are possible guests; none has code in the tree. Start with an issue that argues the CPU description and the first public test ROM you can run.
