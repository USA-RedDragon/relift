# relift C runtime

The C half of relift: lifted code links against it. It has three layers,
mirroring the Rust folders (`core/`, `cpus/`, `guests/`):

| Directory | Layer | What it holds |
|---|---|---|
| `core/` | core | Knows no guest OS and no guest CPU. Guest memory reservation and the exact map at the guest page size, with the host page policy of abi.md section 2; its own permission flags (`RL_VM_R`, `RL_VM_W`, `RL_VM_X`) and an opaque per-block guest tag; the SIGSEGV/SIGBUS handler reporting guest addresses; host threads that carry the CPU layer's context; the clock at the guest's tick rate; the device scheduler for guests that drive devices through registers (`core/include/relift/sched.h`: CPU cycle clocks, devices caught up on access, timed events, interrupt lines, `rl_sync` and `rl_io_read`/`rl_io_write`, and an ordered mode in which CPUs agree on the order of their shared accesses, `rl_sched_order`), which runs every guest CPU on a fiber of one host thread (`core/fiber.h`; the stack switch is host-arch assembly in `core/fiber_x86_64_sysv.S`, `core/fiber_x86_64_win.S` and `core/fiber_aarch64.S`; abi.md section 14); unit registration and the dispatch table; logging, configuration and the unknown-call reporter; bundled code mods chosen at start (`core/include/relift/mods.h`: slot swaps and data patches); host input snapshots (`core/include/relift/input.h`) and host windows (`core/include/relift/window.h`: a window backend, its events, and `rl_window_main`, which runs the program on a second host thread while the first runs the backend's event loop). `core/include/relift/abi_core.h` is the CPU-neutral half of the lifted-code contract. |
| `cpus/arm64/` | CPU | The ARM64 lifted-code ABI, `cpus/arm64/include/relift/arm64/abi.h` (it owns `rl_ctx`), and its runtime entries: FPCR, WFE, CNTPCT, `rl_undefined`, `rl_resume`, the dispatch miss entry, register summaries for crash reports, and the CPU descriptor the core allocates contexts from. Not tied to a guest OS. |
| `cpus/sm83/` | CPU | The SM83 (Game Boy CPU) lifted-code ABI, `cpus/sm83/include/relift/sm83/abi.h` (it owns `rl_ctx`: registers, the flag byte, IME and the EI delay, the `rl_time` clock), and its runtime entries: `rl_undefined`, `rl_trap`, EI/DI/RETI/HALT, the interrupt entry and return hooks (`rl_sm83_attach`), the thread entry `rl_sm83_run`, register summaries for crash reports and the CPU descriptor. It reads the machine's interrupt request and enable state through a callback table, so it names no console. |
| `guests/switch/` | guest | The Horizon kernel (handle table, events, threads, memory, shared and transfer memory, mutexes, condition variables, the address arbiter), `rl_svc`, IPC (HIPC, CMIF with domains, TIPC), `sm:` and the service registry; the NPDM-driven layout; the guest descriptor and the translation between Horizon's permission bits and the core's. |
| `cpus/arm32/` | CPU | The ARMv4T (ARM7TDMI) lifted-code ABI, `cpus/arm32/include/relift/arm32/abi.h` (it owns `rl_ctx`: r0 to r14, the CPSR in parts, the SPSRs and banked registers in relift-cpu-arm32's slot order, the `rl_time` clock, the code fetch wait table and the return address stack of lifted calls), and its runtime entries: the CPSR and mode bank swaps, exception entry and return, the I bit's interrupt mask over scheduler lines (`rl_arm32_set_lines`), the run loop with redirects, nested guest calls, register summaries for crash reports and the CPU descriptor; and the ARM946E-S system control coprocessor of the ARMv5TE profile (`cp15.h`: the registers lifted MCR and MRC reach by key, the TCM areas and the protection regions, what each write changed). It names no console. |
| `guests/gb/` | guest | The Game Boy and Game Boy Color memory map over the core alone (the SM83 layer sits above it, and `guests/gb/exact` joins them): cartridge ROM and RAM behind the memory bank controllers (none, MBC1 with multicart wiring, MBC2, MBC3 with its clock, MBC5 with rumble), video RAM and work RAM banks, echo RAM, OAM, high RAM, battery saves to a host file, and the I/O registers routed to scheduler device slots (`rl_gb_attach`; a slot without a device gets a register-file stub). `postboot.c` holds the registers and I/O defaults each hardware revision hands a cartridge. `gbdev.c` binds the picture, sound, timer, serial, joypad and DMA devices of rustyboi-core to the seven slots, in the exact per-instruction-boundary mode, through the Rust static library in `guests/gb/devices` (`rl_gb_set_bus` hands it the memory too; it is built apart from the C runtime, see `make gbdev-lockstep`). |
| `cpus/mips/` | CPU | The VR4300 (Nintendo 64 CPU) lifted-code ABI, `cpus/mips/include/relift/mips/abi.h` (it owns `rl_ctx`: the general registers, HI and LO, the FPU registers, FCR31 and the CP0 registers in relift-cpu-mips's slot order (`RL_MIPS_SLOTS`, checked by `cpus/mips/tests/ctx_layout.rs`), the `rl_time` clock, the 32-entry TLB and the return address stack of lifted calls), and its runtime entries: exception entry with the VR4300 vectors and ERET, interrupts over the IP bits, Count and Compare from the cycle clock, Random, the TLB instructions and address translation per segment and mode, the FPU slow path (a soft float with the four rounding modes and the VR4300 rules for unimplemented operations, flush to zero and the enables; rules ported from ares, provenance in `fpu.c`), the fast path's FCR31 intrinsics, the CACHE hook, the guard slow paths, the run loop with redirects and the CPU descriptor. It names no console. `mips_fpu_cases` runs COP1 instructions through the slow path for `relift-check --oracle ares lift --c-slow-path`. `cpus/mips/README.md` notes what an R3000A (PlayStation CPU) layer would add. |
| `guests/gba/` | guest | The Game Boy Advance machine (`gba.h`): the memory map with its mirrors, 8-bit write rules and open bus; per-region wait states from WAITCNT and the memory control register with Game Pak bursts and the prefetch buffer; the interrupt controller with its one- and two-cycle delays; HALTCNT (through the BIOS); four timers; four DMA channels; and the display timing (VCOUNT, DISPSTAT, VBlank, HBlank and VCOUNT match), all as devices and events on the scheduler. The ARMv4T layer reports its bus traffic through `rl_gba_load` and `rl_gba_store` (`run/`). The picture unit (`ppu/`: backgrounds, sprites, windows, blending, ported from ares and compared with the NanoBoyAdvance oracle on generated scenes, `check/gba-ppu`) runs as the machine's picture slot device and presents each frame through the window layer. The sound and serial units are register stubs unless a device is supplied. The keypad reads the `p1` port of `rl_gba_ports` (KEYINPUT, KEYCNT and its interrupt). The cartridge backup is SRAM, Flash 64 or 128 KiB (Atmel, Panasonic, Macronix, SST and Sanyo command sets, optional busy time) or EEPROM 512 B or 8 KiB on the DMA3 bit stream, found from the ROM's library string (`rl_gba_save_detect`) and kept as a raw `.sav` that other emulators and flash carts read, written atomically (`rl_gba_save_write`, `rl_gba_save_path` with the `save.dir` setting). Provenance: `NOTICE`. The high-level BIOS (`bios/`, no BIOS image) covers the SWIs 0x00-0x1F and 0x25-0x27 except the sound driver calls, with the results, side registers, written memory and stack frames of the console's BIOS. It also has the IRQ stub at 0x18, the post-boot state and the BIOS region's read values, with approximate cycle costs fitted to console records. Its memory goes through `rl_gba_bus` (`rl_gba_bios_bus` for the machine). `run/` calls `rl_gba_swi` at a SWI and `rl_gba_bios_irq_entry`/`_return` at IRQ entry and at 0x138. Provenance per function: `bios/NOTICE`. |
| `guests/n64/` | guest | The Nintendo 64 machine (`n64.h`): the physical memory map (RDRAM, the RCP registers, the cartridge domains, PIF RAM); MI's interrupt mask and pending bits on scheduler line 2 (the CPU's IP2); the RI and RDRAM registers; PI with both domains' timing, DMA in both directions in ares' block shape and duration, its status and interrupt; the SP registers with DMA to DMEM and IMEM and the semaphore (the RSP does not run yet: a start halts it at once and is counted); VI with its line counter, field and leap pattern, the interrupt at `VI_V_INTR` and a raw scan-out of RGBA5551 or RGBA8888 framebuffers; AI and SI as register stubs (SI DMA moves PIF RAM and raises its interrupt); the high-level boot per boot variant (`rl_n64_boot`: the first MiB at the entry point, the header in DMEM, PI timing from the header, the RI registers and the RDRAM size word at `0x80000318`, `0x800003F0` for 6105; no boot code runs); and the code binding (`code.c`): every lifted run's dispatch slots hold the run layer's entry check until its bytes in RDRAM hash to the run's hash, a DMA into RDRAM or an instruction cache invalidation puts the check back, an entry no run matches is missed code. Provenance: `NOTICE`. |
| `guests/n64/run/` | guest | The program lifted Nintendo 64 code runs in (`librelift_n64_run.a`; a game links it through `relift build`): the machine under the VR4300 layer, device accesses through the machine, the timer event at Compare, the CP0 sync point, the cache hook, the post-boot registers, and `n64main.c`, the program: window or headless, PNG framebuffer dumps, the framebuffer hash and a register and RDRAM report at `--frames N`, `--dump` of RDRAM, `--stats`. |
| `guests/gba/run/` | guest | The program lifted Game Boy Advance code runs in (`librelift_gba_run.a`; a game links it through `relift build`): the machine under the ARMv4T layer, data accesses through the bus, SWI through the HLE BIOS, IRQs through the BIOS stub, the post-boot state, and `gbamain.c`, the program: window or headless, PNG frame dumps, the frame hash and a register and memory report at `--frames N`, and the backup file. |
| `guests/sms/` | guest | The Sega Master System and Game Gear machine, as catch-up devices ported from ares. The machine (`sms.h`): the memory map, the Sega (with cartridge RAM), Codemasters and Korean mappers, the memory control port, the I/O ports, the gamepad with its direction latches, the Game Gear's ports, start button and Master System mode, the pause NMI, the TH latch of the H counter, the FM unit and its audio control port on a Japanese Master System, and the timeline: CPU T-states map to VDP counts with ares' integer clock steps, so a port access sees exactly the VDP work ares has done by then, and the next interrupt or frame edge is known in T-states. The SN76489 sound chip (`psg.h`): three tones, noise with a 16-bit shift register, the Game Gear stereo port, one sample per 16 CPU clocks made by catch-up and drained by the caller, save and restore, checked sample for sample against ares (`check/oracle/ares/psg-compare.py`). The VDP (`vdp.h`): the control and data ports, VRAM and CRAM (6-bit, or 12-bit through the Game Gear's latch), the V and H counters, the line and frame interrupts with the position of the next one, mode 4 at 192, 224 and 240 lines, the TMS9918 graphics modes 1 and 2, sprites, and the picture as VDP colours. The SG-1000's TMS9918A (`tms9918.h`): the ports, the frame interrupt at line 192, graphics 1 and 2, text and multicolor modes, four sprites per line, the fixed palette, run by the machine as an SG-1000: a linear cartridge, 1 KiB of RAM, the TMS9918A and pad ports. The YM2413 FM unit (`ym2413.h`): nine voices or six and the rhythm sounds, one sample per 72 CPU clocks by catch-up, its tables precomputed so every host gets the same samples, checked sample for sample against ares (`check/oracle/ares/ym2413-compare.py`). The mix (`audio.h`): the PSG and the FM unit, muted as the audio control port says, resampled to the host rate by an integer box filter and DC-blocked, the same frames on every host. Provenance: `NOTICE`. |
| `guests/sms/run/` | guest | The program lifted Master System and Game Gear code runs in (`librelift_sms_run.a`): the machine under the Z80 layer with every bus and port access through it (`relift/sms/run.h`), the instruction boundary at every sync point with the NMI and INT entries, HALT as NOP cycles up to the next event, dispatch by the bank each slot shows and by the bytes in work RAM, a shadow stack of host frames that sends a return elsewhere, or into a remapped slot or rewritten RAM, back to the run loop, and the stop report at the first instruction with no lifted body (there is no interpreter). `smsmain.c` is the program: window or headless, the input script, frame CRCs, per-frame state hashes, raw and PNG frames, the sound to the host output (frames paced on its queue) or a WAV, and an instruction log in the form of the ares oracle's. |
| `guests/nds/` | guest | The Nintendo DS machine (`nds.h`) over the core and the ARM32 layer: the ARM9 and ARM7 memory maps with their mirrors, the shared work RAM halves that WRAMCNT gives each CPU (WRAMSTAT on the ARM7), the ARM7's own work RAM, the ARM9's TCMs placed by its CP15 registers (`cpus/arm32/cp15.h`, ITCM over DTCM, load mode), palette, video RAM and OAM as plain storage, both BIOS regions as stubs with no image, the GBA slot's open bus, the I/O registers of each CPU (the interrupt controller with IME, IE and IF acknowledged by writing 1; IPCSYNC with its remote IRQ; the two 16-word IPC FIFOs with their error and enable bits and edge-triggered send-empty and receive-not-empty IRQs; four timers per CPU counting bus cycles with prescalers, count-up and overflow IRQs; the ARM9's division unit (32/32, 64/32 and 64/64 with GBATEK's division by zero and overflow results) and square root unit with their busy times; EXMEMCNT, VRAMCNT, POSTFLG, HALTCNT; register files for the rest) and GBATEK's access costs per CPU, region, width and sequence, with main RAM serving one CPU at a time. One tick is half a bus cycle (ARM9 cycle 1, ARM7 cycle 2). Provenance: `NOTICE`. Both CPUs run on fibers of one host thread in the scheduler's ordered mode (`order.c`): every shared access (I/O, shared work RAM, main RAM, the GBA slot, the ARM7's video RAM window) waits until the other CPU is past it in the order (tick, then the CPU EXMEMCNT bit 15 favors), HALT takes a CPU out of the order until IE & IF wakes it, and interrupts arrive at block boundaries (`rl_nds_irq_poll`). |
| `guests/gb/exact/` | guest | The exact mode lifted Game Boy code runs in (`librelift_gb_exact.a`; a game links it through `relift build`). The device library owns the bus and the instruction boundary: `rl_gb_sync` closes each lifted instruction and opens the next, a shadow stack of the host frames sends a return elsewhere or a bank switch under a direct transfer back to the run loop, and code with no lifted function runs on the library's CPU (`rl_sm83_set_miss`). `gbmain.c` is the program: window or headless, the joypad through the port tables, sound through the audio layer (`--wav` records it), battery saves, PNG frame dumps, corpus case grading (`--case`) and the lockstep check (`--check`); `--help` lists its options. |
| `host/` | host | Optional backends for the core's interfaces: SDL3 audio output and libopus decoding (`core/include/relift/audio.h`), built only with `AUDIO=real`; SDL3 windows, Vulkan surfaces and input (`core/include/relift/window.h`, `input.h`), built only with `VIDEO=sdl3`; VP9 decoding through libvpx (`core/include/relift/video.h`, `host/vp9_libvpx.c`), which `relift build` compiles from the pinned libvpx source it fetches into its cache, with the generic (plain C) configuration in `host/vpx/`; AV1 decoding through dav1d (`host/av1_dav1d.c`), compiled the same way from the pinned dav1d source, with its configuration and license notice in `host/dav1d/`. SDL3 runs the same code on Linux, macOS and Windows; Linux (X11 and Wayland) and macOS have been run. |
| `guests/switch/vk/` | guest | Vulkan forwarding: guest Vulkan calls run on the host driver. `gen/` is generated by `relift-vkgen` from `vk.xml`; the rest is hand-written (see its README). Provenance of `bcdec.c`: `NOTICE`. |
| `charter/` | | The check that core names no guest and no CPU (`make charter`) |
| `tools/gdb/` | | `relift_frames.py`, a gdb frame filter that shows lifted functions by their guest names, the `relift-fibers` and `relift-fiber-bt` commands for parked guest CPUs, and their test (`make gdb-test`) |

Each layer sees only its own headers and those of the layers below it; core
sees no CPU or guest header. The contract is explained in `../docs/abi.md`.

## Build and test

relift drives real builds. For work on the runtime itself:

```sh
make -C runtime -j2 test                     # clang, host target, into runtime/build/host
make -C runtime -j2 CC="zig cc" AR="zig ar" TARGET=aarch64-macos BUILD=build/mac
make -C runtime charter                      # python3, standard library only
make -C runtime -j2 AUDIO=real BUILD=build/audio test   # also builds host/ (pkg-config: sdl3 opus)
make -C runtime -j2 VIDEO=sdl3 BUILD=build/video test   # also builds the SDL3 window backend (pkg-config: sdl3)
make -C runtime gdb-test                     # skips when gdb is missing
make -C runtime sched-bench                  # device scheduler microbenchmark
make -C runtime -j2 CC="zig cc" AR="zig ar" TARGET=x86_64-windows-gnu BUILD=build/win portable
```

`relift build` links `guests/switch/vk` into every game. It also links `host/window_sdl3.c` when it finds SDL3: `RELIFT_SDL3_DIR` (a directory with `include/` and `lib/`), else `<cache>/sdk/sdl3/<arch>-<os>/` (where the pinned prebuilt is unpacked), else `pkg-config sdl3`. Without SDL3 it prints one line and builds headless. On macOS it links MoltenVK directly (`-DRL_VULKAN_LINKED`, no Vulkan loader): `RELIFT_MOLTENVK_DIR` (a directory with `lib/libMoltenVK.dylib`), else `<cache>/sdk/moltenvk/<arch>-<os>/`, else Homebrew's `molten-vk`; `make` does the same with `MOLTENVK_DIR`.

Android builds need API level 30 or newer (`memfd_create`; the build stops with a message below that) and no `-lpthread`, which bionic does not have:

```sh
NDK=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin
make -C runtime -j2 CC=$NDK/aarch64-linux-android30-clang AR=$NDK/llvm-ar BUILD=build/android all
```

`runtime/tools/cross/gbcross.py --target <windows-x64|windows-arm64|android-arm64|android-x64|macos-x64|macos-arm64|ios-arm64|ios-sim-arm64> --relift BIN --work DIR [--run]` cross-builds the runtime and a generated Game Boy test program for one target (zig 0.17.1 or llvm-mingw for Windows, the NDK found through `ANDROID_NDK_HOME`, `ANDROID_NDK_ROOT` or the SDK's `ndk-bundle` and newest `ndk/<version>` for Android, Xcode for Apple targets), runs `release_guard.py` on the result (no hot-load, no `dlopen` or `LoadLibrary` outside `hostlib.c`, no executable memory, no writable and executable segment) and with `--run` runs the program (wine, adb or locally). A missing tool stops the run and names what to install. The `Cross` workflow runs it for Windows x86-64, Android arm64, macOS x86-64 and iOS.

`portable` builds only `crash_test`, `fiber_tests` and `sched_bench`,
which link the host-portable part of the core (crash handler, fibers,
scheduler), so they build for Windows (x86-64 and arm64), where the rest
of the core is not ported yet; copy the `.exe` files over and run them.

The build makes one archive per layer (`librelift_core.a`,
`librelift_arm64.a`, `librelift_sm83.a`, `librelift_switch.a`, `librelift_switch_vk.a`, `librelift_arm32.a`, `librelift_mips.a`, `librelift_gb.a`, `librelift_gba.a`, `librelift_gb_exact.a`, `librelift_gba_run.a`, `librelift_n64.a`, `librelift_n64_run.a`, `librelift_sms.a`, `librelift_nds.a`), the whole runtime as
`librelift_rt.a` and `librelift_rt.so` (or `.dylib`) built with
`RL_BUILDING_RUNTIME`, and one test binary per layer, each linking only its
layer and the ones below:

- `core_tests`: the core alone, against a fake guest and CPU (8 KiB pages,
  a 32-bit address space, a 40.5 MHz clock, big-endian data, a 2-byte code
  granule);
- `arm64_tests`: the A64 layer over the core, with a test guest that is no OS;
- `sm83_tests`: the SM83 layer over the core and the device scheduler, with a synthetic interrupt controller and timer;
- `switch_tests`: the Switch guest, driven through `rl_ctx` and `rl_svc` the
  way lifted code does;
- `arm32_tests`: the ARMv4T layer over the core with hand-written lifted
  functions: banks, exceptions, the I bit's mask, frames and redirects; and
  the CP15 registers with an SDK-style start-up sequence;
- `mips_tests`: the VR4300 layer over the core with hand-written lifted
  functions: the slot layout, exception entry, vectors and ERET, interrupt
  masking, Count and Compare, Random, the TLB and translation, the guard slow
  paths, frames and redirects; and the FPU slow path on special values,
  known answers recorded on ares, rounding modes, enables and flush, with the
  soft float against the host on random operands;
- `sms_tests`: the SN76489: registers, tone and noise, catch-up timing, stereo,
  buffer, save states, and a fixed script against the ares hash; the VDP:
  ports, CRAM, the counters, both interrupts and their predicted positions,
  the frame edge and a mode 4 tile; the machine: the three mappers, cartridge
  RAM, the memory control port, the controller ports, the Game Gear start
  button, the pause NMI, the TH latch, the FM ports and the timeline, and the SG-1000 map and ports; the mix: a second of clocks is a second of frames in any pieces, a
  tone's pitch, FM unmuted and muted; the TMS9918A: the control port, the frame interrupt and its
  prediction, graphics 1 and text pixels, a sprite; the YM2413: reset, timing, key on and a
  fixed script against the ares hash. `sms_ym2413_script` runs a script for
  the comparison with ares. `sms_psg_script`
  runs a script for the comparison with ares.
- `gba_run_tests`: the ARMv4T layer on the GBA machine: bus costs, the
  prefetch flag, IRQ entry and return, VBlankIntrWait;
- `n64_tests`: the N64 machine over the core: the memory map, MI, PI DMA and
  its duration, SP DMA, VI's lines and interrupt, the scan-out and the
  boot, on synthetic ROMs;
- `n64_run_tests`: the VR4300 layer on the N64 machine with hand-written
  lifted functions: the boot registers, binding by hash at the first entry,
  changed bytes as missed code, the cache invalidation and a VI interrupt at
  a sync point;
- `gb_tests`: the Game Boy memory map over the core, with synthetic cartridges,
  a fake time source for the clock and probe devices on the scheduler;
- `gba_tests`: the GBA machine and its BIOS. The tests cover the memory map and
  devices; 365 bus timing records from the console (`tests/console_timing.h`,
  `tests/test_bus_console.c`)
  replayed as bus activity; and timers, interrupts, DMA and display timing
  compared tick by tick against the NanoBoyAdvance oracle (`check/oracle/nba`;
  skipped without the binary), as are the keypad and the SRAM, Flash and EEPROM
  command streams. The picture unit runs hand-built scenes and a generated scene set (`check/gba-ppu`) against the oracle's frame hash. The BIOS is checked against the console's
  results for Div, DivArm, Sqrt, ArcTan and ArcTan2 (`tests/vectors_math.h`)
  and hand-built inputs for the others;
- `nds_tests`: the Nintendo DS machine over the core and the ARM32 layer:
  both memory maps, the shared work RAM banks, the TCMs from CP15, the BIOS
  stubs, the system registers, the interrupt controllers, IPCSYNC and the
  IPC FIFOs, the timers, the division and square root unit, the cost table
  and main RAM contention; and the two CPUs on
  their fibers in the scheduler's ordered mode with hand-written lifted-style
  cores (`test_q3.c`, the q3 scenarios of spike 20-nds: FIFO ping-pong, a
  shared work RAM hand-over, a racy main RAM flag, an IPCSYNC IRQ to a
  halted or busy ARM7 and main RAM contention), each matched against a
  per-instruction lockstep run of the same machine;
- `gb_lockstep` (`make GBDEV_LIB=<librelift_gb_devices.a> gbdev-lockstep GBDEV_ROMS=...`):
  runs ROMs through the slot devices with C instruction bodies and through
  rustyboi's own CPU, comparing registers and dots after every instruction
  and the frame and device state every frame; it needs the Rust library and
  ROMs, so it is not part of `make test`;
- `switch_vk_tests`: the Vulkan forwarding layer through guest structs, thunks and a host Vulkan driver (skipped without one unless `RELIFT_VK_REQUIRE` is set);
- `crash_test`: the crash handler alone (`core/crash.c` and its host-OS
  part), with hand-written lifted functions in a function map that make a
  wild guest store, run an illegal instruction or crash in host code; it
  runs each case as a child and checks the report, including a fault on a
  fiber (the report names the guest CPU) and a fiber's stack overflow
  (guard page on POSIX, STATUS_STACK_OVERFLOW on Windows). It links only
  the crash handler and the fibers, so it also builds on Windows
  (`make portable`);
- `fiber_tests`: the fibers and the scheduler on them, also on Windows:
  registers across switches, a ring of fibers, `longjmp` inside a fiber,
  the Windows TEB stack fields, the FP mode check at a hand-over, and two
  guest CPUs handing the run back and forth repeatably.

`<binary> [substring...]` runs the tests whose names contain a substring;
each test runs in its own process (a fork, or on Apple hosts a new process,
since a forked child cannot reach Metal's shader compiler). `RELIFT_TEST_NOFORK=1`
runs them in one process for a debugger.

## Guest names in gdb

Lifted functions carry C identifiers that are Itanium names inside the
`guest` namespace (`_ZN5guest4main3fooEv`), which lldb, perf and
`addr2line -C` show as `guest::main::foo()`. gdb does not demangle the name
of a function from a C compilation unit, so `bt` shows the raw identifier
unless the frame filter is loaded:

```text
(gdb) source runtime/tools/gdb/relift_frames.py
(gdb) bt
#0  guest::main::foo() () at frames.c:16
(gdb) relift-guest-name _ZN5guest4main3fooEv
guest::main::foo()
(gdb) relift-fibers
#0  guest CPU 0    running      stack 0x7ffff5e00000-0x7ffff6e00000
#1  guest CPU 1    parked       stack 0x7ffff4c00000-0x7ffff5c00000
(gdb) relift-fiber-bt 1
```

`bt` shows only the running fiber. `relift-fibers` lists every live fiber
(each guest CPU of the device scheduler has one) and `relift-fiber-bt N`
backtraces a parked one from the registers its switch saved (x86-64
System V and AArch64 hosts).

## The charter

`charter/check.py` fails when anything under `core/` includes a header of
`cpus/` or `guests/`, or spells a word of its denylist (Horizon, svc, NPDM,
`nn::`, the console's name, Horizon kernel and IPC names, ARM64 system
registers and instruction names, guest register reads, a fixed 4 KiB page,
the 19.2 MHz counter, the 36 and 39-bit widths). `charter/allow.txt` lists the
allowed (file, rule) pairs with a reason each, for host words such as the
arm64 host pc in the fault handler and the fiber switch for 64-bit Arm
hosts (`core/fiber_aarch64.S`, host registers only); an entry that no longer matches fails the
check. Presence controls keep an empty walk or a broken rule from passing: the
include rule must reject includes in `guests/switch` and `cpus/arm64`, every
rule must match its own sample, and the denylist must hit files there.

## Settings

Run-time settings are typed keys declared with `rl_settings_declare`
(`core/include/relift/settings.h`): bool, int and float with a range and a
step, enum, string and key binding. The runtime declares `video`, `language`,
`video.gpu_driver`, `picture.aa`, `picture.aa_quality` and `picture.filter`; the Switch
layer declares `save.dir`, `vk.device`, `vk.keep_attachments`, `vk.decode_bc` and
`kernel.layout`; the Game
Boy layer declares `video.gb_color_correction`; the Nintendo DS layer
declares `save.dir`, `save.type`, `video.nds_layout`, `video.nds_displays`,
`video.nds_integer_scale` and `video.nds_lcd_response`; a game adds its own from the
`[settings]` tables of relift.toml. Precedence, last wins:

1. the declared default;
2. `<user folder>/settings.json`, read only when the program names an app with
   `rl_settings_set_app` and no `--relift-settings` flag is given;
3. each `--relift-settings <file>` (a flat JSON object), in order;
4. each `--relift-set key=value`, in order, whatever its place among the files;
5. the key's environment variable, when it has one (`RELIFT_VIDEO`,
   `RELIFT_PICTURE_FILTER`, `RELIFT_UPSCALER`, `RELIFT_TAA_SHARPNESS`, `RELIFT_SAVE`, `RELIFT_VK_DEVICE`, `RELIFT_VK_KEEP_ATTACHMENTS`,
   `RELIFT_GB_COLOR_CORRECTION`, `RELIFT_NDS_LAYOUT`, `RELIFT_NDS_DISPLAYS`,
   `RELIFT_NDS_INTEGER_SCALE`).

Unknown keys, values of the wrong type or outside the range, and unreadable
files warn and are skipped; they never stop a run. The user folder is
`$XDG_DATA_HOME/<app>` or `~/.local/share/<app>` on Linux, `~/Library/Application
Support/<app>` on macOS and `%APPDATA%\<app>` on Windows. Without `save.dir`, a
program that names an app keeps saves in `<user folder>/save`; otherwise
`./save`.

## Environment switches

| Variable | Effect |
|---|---|
| `RELIFT_TRACE_OS=1` | The guest layer logs every guest OS call and service request with arguments, result and time (Switch: every svc and IPC command) |
| `RELIFT_STRICT=1` | Stop on an unknown guest OS call or service command instead of returning an error; fail a Vulkan device or call that goes past a host limit below the Switch's (see `guests/switch/vk/README.md`) |
| `RELIFT_AUDIO=null` | In an `AUDIO=real` build, play through the null output (drains in real time, no sound) |
| `RELIFT_LANGUAGE=<name or locale>` | The `language` setting: the language of the game's window title, a Switch language name (`French`) or a locale (`ja`, `fr_CA`). Unset: the host's language list. The title and the window icon come from the identity the program registers (`relift/appinfo.h`); a program that registers none keeps the title its code passes and has no icon |
| `RELIFT_VIDEO=headless\|sdl3` | The `video` setting (an environment variable beats the flags). Window backend. Unset: SDL3 when the runtime was built with `VIDEO=sdl3` and a display exists (`WAYLAND_DISPLAY` or `DISPLAY` on Linux, always on macOS and Windows), headless otherwise. `headless` (or `none`) opens no window. `sdl3` skips the display check; it still falls back to headless when SDL cannot start, or when the runtime has no SDL3. A program without Vulkan symbols opens no backend whatever this says |
| `RELIFT_PICTURE_FILTER=default\|integer\|nearest\|sharp_bilinear` | The `picture.filter` setting: how every window (the main one and those on other displays) scales and samples the picture, on the GPU. `default`: the window's own scaling (whole multiples, or filling the window where a guest setting such as `video.nds_integer_scale` asks), sampled nearest at whole multiples, sharp bilinear above 2x when the scale is not whole, and bilinear below 2x (the rule is in [the usage guide](../docs/guides/usage.md#picture-scaling)); `integer`: whole multiples only, nearest; `nearest`: fill the window, nearest (uneven pixels); `sharp_bilinear`: fill the window from a nearest prescale by the largest whole multiple, then bilinear, so pixels stay even and only their edges blend |
| `RELIFT_PRESENT_MODE=fifo\|mailbox` | The present mode of the swapchains the core presenter makes for windows on other displays (`core/present.h`; Android Presentations). Unset: MAILBOX where the surface has it, else FIFO. Either way the game's thread never waits on such a display |
| `RELIFT_VULKAN_LIBRARY=<path>` | Host Vulkan loader to use instead of the system one |
| `RELIFT_VK_MAP=placed\|import\|shadow` | Force how guest-mapped device memory reaches guest memory (see `guests/switch/vk/README.md`) |
| `RELIFT_GB_COLOR_CORRECTION=on\|off` | The `video.gb_color_correction` setting (Game Boy). On (default): the colours the way the console's LCD shows them. Off: the plain expansion of the 5-bit channels, which is what the reference images of the test ROMs use |
| `RELIFT_NDS_LAYOUT=auto\|stacked\|side_by_side\|large_top\|large_bottom\|top_only\|bottom_only` | The `video.nds_layout` setting (Nintendo DS): how the two screens share a window. `auto` (default): stacked in a tall window or with the on-screen controller, else stacked or side by side by fit; `stacked`: the top screen above the bottom one; `side_by_side`; `large_top` and `large_bottom`: that screen at twice the size, the other small; `top_only` and `bottom_only`. The pen is the pointer or finger over the bottom screen's image, wherever it draws |
| `RELIFT_NDS_DISPLAYS=auto\|swapped\|top_both\|bottom_both\|single` | The `video.nds_displays` setting (Nintendo DS). With a second display, `auto` shows the top screen on the first and the bottom screen on the second; `swapped` the reverse; `top_both` and `bottom_both` one screen on both; `single` only the first display |
| `RELIFT_NDS_INTEGER_SCALE=on\|off` | The `video.nds_integer_scale` setting (Nintendo DS). On: each picture grows by the largest whole multiple that fits its window. Off (default): it fills the window with its shape kept |
| `RELIFT_NDS_LCD_RESPONSE=on\|off` | The `video.nds_lcd_response` setting (Nintendo DS). On (default): a pixel that goes to another color for one frame and back is shown as the mean of the two, as the console's slow LCD shows the flicker transparency some games draw. Off: every frame is shown as drawn |
| `RELIFT_UPSCALER=auto\|game\|off\|nearest\|bilinear\|sharp_bilinear\|lanczos\|fsr1\|fsr2\|fsr3\|taa\|dlss` | The `picture.upscaler` setting: the upscaler stage (`core/include/relift/upscale.h`). Where a game asks its GPU driver for an upscaler inside its frame (DLSS on Switch 2), the choice runs there, before the game's UI and post effects: `game` is the game's own upscaler, translated; `auto` (default) is the game's own where this device meets its requirements, else a temporal upscaler picked by GPU class: on integrated and mobile GPUs (VkPhysicalDeviceType other than discrete, and Arm, Qualcomm, Apple and Intel integrated GPUs by vendor) `taa`, the best look for its cost (a third of FSR 2's GPU time); on discrete GPUs `dlss` where NVIDIA's DLSS runs (the console itself runs DLSS, so it is the closest match), else `fsr2`, which on this game's own frames looked better than `fsr3`; the log names the pick and why; `off` copies the picture at its render size; `nearest`, `bilinear`, `sharp_bilinear` (a nearest prescale by the largest whole multiple, then bilinear) and `lanczos` (3 lobes, clamped to the four nearest pixels against ringing) are written for relift. `fsr1` is AMD FidelityFX Super Resolution 1 (EASU, then RCAS sharpening; `third_party/fsr1`). `fsr2` is AMD FidelityFX Super Resolution 2 (`third_party/fsr2`), temporal: it runs where the game's call gives depth and motion vectors, and FSR 1 elsewhere, the window included. `fsr3` is the upscaler of AMD FidelityFX FSR 3.1 (`third_party/fsr3`), temporal in the same way. `taa` is relift's own plain temporal upscaler (`core/shaders/taa.comp`): this frame's jittered pixels around each display pixel, blended with the history the motion vectors reproject, clamped to the variance around it, then sharpened by AMD's RCAS at `picture.taa_sharpness`; bilinear where the call gives no depth and motion vectors. `dlss` is NVIDIA DLSS Super Resolution (DLAA at the game's sizes, preset K) through the NGX library of the NVIDIA driver (`libnvidia-ngx.so.1`, Linux only for now): relift ships no NVIDIA file, so it runs only where the user has put NVIDIA's DLSS library (`libnvidia-ngx-dlss.so.*`, from github.com/NVIDIA/DLSS under NVIDIA's license) in the folder of the running program (the log line names it), the one folder the driver's NGX looks in when relift calls it directly: not the working folder or NGX's models folder, and a program inside a read-only package (an AppImage) cannot take it; once started, NGX stays up until the program exits, since the driver's own shutdown entry point takes an argument NVIDIA's public declaration does not have; the log says which libraries it found where, or why not, and it runs `fsr2` otherwise. For every other guest, a choice other than `auto`, `game` and `off` upscales the window's picture `picture.upscale_scale` times (1 to 8, default 2) ahead of anti-aliasing. Every choice other than `game` changes the picture from the console's; `picture.upscaler` is the stage, while the window's own scaling to its size stays separate |
| `RELIFT_TAA_SHARPNESS=<0 to 1>` | The `picture.taa_sharpness` setting: how strongly AMD's RCAS (`third_party/fsr1`) sharpens what `picture.upscaler=taa` writes, from 0 (no sharpening pass) to 1 (its strongest); default 0.5 |
| `RELIFT_VK_DEVICE=<name part>` | The `vk.device` setting. Host physical device the guest sees (default: the first that has graphics and compute and can present) |
| `RELIFT_VK_KEEP_ATTACHMENTS=auto\|on\|off` | The `vk.keep_attachments` setting. Pass the guest's DONT_CARE attachment load and store ops as LOAD and STORE (default `auto`: on Apple GPUs only; see `guests/switch/vk/README.md`) |
| `RELIFT_VK_DECODE_BC=auto\|on\|off` | The `vk.decode_bc` setting. Create BC1 to BC7 images in an uncompressed host format and decode their uploads on the CPU (default `auto`: only for formats the host GPU cannot sample, such as Apple GPUs through MoltenVK; `on` for every BC format; see `guests/switch/vk/README.md`) |
| `RELIFT_RENDERDOC_CAPTURE=<present>[:<frames>]` | The `renderdoc.capture` setting, for every guest. When the program runs under RenderDoc (its library already loaded), start a frame capture before frame `<present>` (counting guest presents from 1; frame k ends at present k) and end it after `<frames>` frames (default 1). A present is counted where a guest hands a finished frame to the shared presenter (`rl_window_present_rgb`, the GPU present slots) and, for a Switch run with no window, in the graphics layer; it works without a window. Only Vulkan work is captured: a guest that draws on the CPU shows nothing but the presenter's own passes, and a run with no window and no Vulkan device has nothing to capture. See "Capturing with RenderDoc" below |
| `RELIFT_RENDERDOC_DIR=<folder>` | The `renderdoc.capture_dir` setting: where the captures are written, as `<folder>/<app>_capture.rdc`. Empty (default): the `captures` folder of the user data folder (`~/.local/share/<app>/captures` on Linux) |
| `RELIFT_DATA_VERIFY=hash` | Mobile apps: at start, check the game data against the app's `data-manifest.txt` (written by `relift build` for iOS and Android) by SHA-256 as well as by size. Without it only sizes are checked. A missing or short file stops the app with a message naming it (an Android app opens its game data import instead, where the player picks a folder or zip of the data); `relift push --target <t>` copies the data and checks it the same way |
| `RELIFT_KERNEL_LAYOUT=compact\|console` | The `kernel.layout` setting (Switch). `compact` (default): the regions the game's address space type has, packed at the lowest bases, so the host reserves tens of GiB instead of 512 GiB (docs/abi.md section 2); `console`: the console's exact region sizes, for comparing against it |
| `RELIFT_VK_CAPTURE=<dir>[:N]` | Debug: write every Nth presented guest swapchain image (default every one) as `<dir>/frame-<number>-<guest ms>ms.png`. Each capture waits for the GPU, so it slows the game; off when unset |
| `RELIFT_NVN_TRACE=<dir>:<present>[:<frames>]` | Switch 2, debugging: write one frame of the NVN layer's draws and dispatches to `<dir>/frame-<n>/`, starting at the first present at or past `<present>` and covering `<frames>` frames (default 1). `trace.jsonl` has a line per draw or dispatch: the programs, render targets, fixed-function and dynamic state, every bound uniform and storage buffer with its GPU and guest address, the pushed constants, and every texture and image it binds with its host image, format, size, view and swizzle. `bufs/` and `imgs/` hold the bytes the lines name, before and after the command (images: every level and layer, level 0 first, in the host image's format; block formats and volumes only level 0), each file named by a hash so a range that did not change is written once. Every traced command submits the batch before and after it, so the game runs slowly and only the traced frames pay. `RELIFT_NVN_TRACE_GBUFFER=1` starts it at the first frame after one that drew into four or more render targets at once, since the present a scene reaches varies between runs. `tools/nvn_trace.py` reads a trace |
| `RELIFT_INPUT_SCRIPT=<steps>` | Debugging only: hold port controls at guest frames so a run needs nobody at the controls. Comma separated `<frame>[+<frames>]:<port>.<control>[=<value>]`, for example `600:p1.plus,900+10:p1.a,1200+30:p1.lstick_y=-1`; a step holds the control at the value (default 1) for the frames (default 6) and replaces what its bindings read. Switch and Switch 2 count vi vsyncs since boot (1/60 s each, independent of how fast the host presents); GBA, DS and N64 count video frames. `@<path>` follows a file that a driver appends steps to. See docs/reference/input.md |
| `RELIFT_TEXT_ENTRY_PANEL=auto\|always\|never` | The `text_entry.panel` setting: when the text entry panel is shown. `auto` (default) in a run with a window and no input script; `always` also with an input script (the run waits for the player); `never` answers from the settings at once |
| `RELIFT_TEXT_ENTRY_TEXT=<text>` | The `text_entry.text` setting: the answer to every text a game asks for (a name, for one; the Switch software keyboard), given without the text entry panel. Unset, a run with no window or with an input script answers with the game's own initial text, else the `text_entry.default` setting (default `Player`) |
| `RELIFT_LOG=debug\|info\|warn\|error` | Lowest log level printed |
| `RELIFT_SCHED=free\|stats` | Debugging only. `free`: Switch guest threads run freely on host threads, ignoring priorities and cores (the old behavior, for comparisons). `stats`: log scheduler hand-overs, preemptions and the longest preemption delay once a second |
| `RELIFT_HOST_PAGE=16384` | Manage guest memory in 16 KiB host pages on a 4 KiB host, to test the Apple and Android policy |
| `RELIFT_SDMC=<dir>` | Host directory served as the SD card (`./sdmc`, created when missing) |
| `RELIFT_SAVE=<dir>` | The `save.dir` setting. Host directory that holds save data (`./save`): `<program id>/user-<uid>` for account saves, `device`, `bcat`, `temporary` and `cache-<index>` for the other kinds, each created on first open. A mount works on a copy; `Commit` swaps the copy in atomically, and a mount closed without `Commit` loses its writes |
| `RELIFT_ROMFS=<path>` | The program's data storage: a RomFS map (`romfs.map`, reading the files in place in a `.nup`), a plain RomFS image (`romfs.img`) or an extracted romfs tree (`relift run` sets it from the import) |
| `RELIFT_MODS=<list>` | Ordered run-time data mods, `:` separated (`;` on Windows), later wins: Atmosphere `contents/<title id>` folders (or a folder holding `contents/`) and Ryujinx mod folders (or a `mods` folder of them), each with `romfs/` and `cheats/<build id>.txt`. Romfs files layer in front of the base tree (conflicts warn, naming the winner); dmnt cheats run each guest frame (a buffer queued to vi or a Vulkan present). Cheats that write lifted code are reported as needing a rebuild |
| `RELIFT_DLC=<dir>` | Add-on content, `<16 hex digit add-on id>/<index>/` holding `romfs.map`, `romfs.img` or a `romfs/` tree (`relift run` sets it from the import) |
| `RELIFT_NVN_SHADERS=<dir>` | Switch 2: the shader folder, where the runtime writes the host device's capabilities (`device.json`) for the next `relift build` and, with `RELIFT_NVN_RECORD` set, each GPU program the game sets (`programs/<key>.bin`, a development tool for finding what the build's scan of the game's files misses). Default `shaders/` beside the executable, which is the build folder's `shaders/` for the program `relift build` leaves there; a package's launcher and a mobile app point it at the user's data folder, and `relift run` at the build's. Also the table folder unless `RELIFT_NVN_TABLES` is set |
| `RELIFT_NVN_TABLES=<dir>` | Switch 2: where the translation tables (`table.rlspv`, `table.rlspi`) are read from, when not the shader folder: the `shaders/` of an AppImage, an app bundle or the pushed mobile data. No path of the build machine is compiled into the program |
| `RELIFT_NVN_TABLE_STAMP=allow` | Switch 2: the runtime logs the translator stamp in the table folder's `table.stamp` and stops with a named report when it differs from the stamp the build expects (the relift that built the program), or is missing. `allow` runs on anyway and logs a warning, for a deliberate A/B run of another translator's tables. `relift shaders` and `relift build` rewrite the stamp; `scripts/shader-stamp.sh` prints the stamp of a source tree |
| `RELIFT_UNPACED=1` | Switch 2: a measuring aid. A present does not wait for the vsync grid (the game's present interval), so a run shows the host's cost of a frame instead of the 60 Hz pace |
| `RELIFT_NVN_CPU_MOVES=1` | Switch 2: a measuring and checking aid. Texture uploads from guest memory and write-backs to it run on the CPU (deswizzled through a staging buffer) instead of on the GPU through the imported pool |
| `RELIFT_PIPELINE_WARM=registered\|all\|all-background\|off` | Switch 2: the launch shader warm-up (see `guests/switch/README.md`). `registered` (default): the programs the game sets and those earlier launches recorded, the first submit waiting for them for at most 3 s; `all`: also every program of the shader table; `all-background`: as `all`, and the first submit does not wait; `off`: pipelines compile at the draw that first needs them |
| `RELIFT_PIPELINE_WARM_WAIT=1` | Switch 2: the first submit waits for the whole launch warm-up behind the blocking compile screen (the game starts when it finishes; Quit or Esc ends the program), instead of at most 3 s. Without it a warm-up that runs past 1 s shows the small corner card (progress bar, shaders done of the total, time left once a rate is measured) over a plain background until the game's first picture, then over the picture |
| `RELIFT_EARLY_WINDOW_SIZE=<w>x<h>` | Switch 2: the size in pixels of the window the compile screen opens before the game's first picture (default 1280x720; a phone-sized `390x844` shows the tall layout) |
| `RELIFT_WARM_THEME=light\|dark` | The compile screen's palette; default is the host's light or dark setting (dark where the host has none) |
| `RELIFT_PIPELINE_MODE=monolithic` | Switch 2: make whole pipelines at the draw (and warm each stage alone) even on a device that links pipeline libraries |
| `RELIFT_WARM_THREADS=<n>` | Switch 2: warm-up workers (default one per processor, and no more than the available memory holds at 4 GiB each). A set count skips the memory rule, and more compiles at once use up the process's memory mappings faster: at `vm.max_map_count` (65530 on Android) allocations and thread creation fail. The warm-up warns at three quarters of it |
| `RELIFT_PIPELINE_CACHE=<dir>` | Switch 2: where the persistent pipeline cache and the recorded pipelines (`pipeline-cache.bin`, `pipeline-keys.bin`) live. Default `pipeline-cache/` in the app's per-user folder when the app is named, else the shader folder |
| `RELIFT_DEV_SOCKET=<path>` | Dev builds (`-DRL_HOTLOAD`, made by `relift run`): the socket of relift's dev server, which builds a library for each missed address (docs/abi.md section 6). `relift run` sets it; a dev build without it stops at the first miss with exit status 70 |
| `RELIFT_ADDRESS_BITS=36` | Ask the guest layer for this address width. The Switch guest accepts 36 and 39 and uses the 36-bit layout even when the NPDM asks for 39 bits (the fallback when the host cannot reserve 2^39 bytes) |

## Data storage

fsp-srv serves the program's romfs (commands 200 to 202) as a read-only
`IStorage` over one RomFS image (`guests/switch/romfs_image.c`). A plain
image file is served as it is with positioned reads. A RomFS map, which
`relift import` writes for a `.nup`, holds the image's metadata and where
each file's data is in other host files (the `.nup` itself, and a blob of
the files the `.nup` stores zstd compressed); every file it reads is checked
by size and 32 recorded bytes when it opens, so a changed dump fails with a
message instead of serving wrong bytes. Either opens with one or two file
descriptors and no directory scan, whatever the number of files in the
romfs. From an extracted tree the image is built
in memory when the guest first opens the storage:
the 0x50 byte header, the directory and file hash tables and metadata
(entries sorted by name, the name hash libnn uses) and the file data at
0x200-aligned offsets. Only the metadata is held in memory; file data is
read from the host files on each `Read`, so a romfs of many gigabytes needs
no second copy on disk (a tree of tens of thousands of files and several gigabytes opens in under a second and holds a few MiB). Games see
the byte layout of an image made by the standard tools (tables before data),
which libnn needs, but not necessarily the original image's order of entries.
Add-on content opens by add-on id (`OpenDataStorageByDataId`, or
`OpenDataStorageByProgramId` with an id that is not the program's). There is
no separate patch storage: the import already holds the patched tree, so
`OpenPatchDataStorageByCurrentProcess` reports the target as not found, as does
any storage with no tree.

## Known limits

- Shared memory is mapped by mapping the same host pages again. On 16 KiB
  host pages a view that does not fill its host pages needs the rest of
  them free: the kernel reserves that rest (svcQueryMemory reports it as
  free so guard page checks pass, maps into it are refused, and guest
  access to it is not trapped) and refuses with an invalid memory state
  result and a one-time report when another mapping is already there. The object's bytes sit at the host-page offset of its
  first view, so a second view at another offset is refused while the first
  is mapped.
- The Switch guest does not support 32-bit address spaces (the core does).
- Guest threads run on host threads, scheduled the way Horizon schedules
  them (`guests/switch/ksched.c`, a port of the logic of Ryujinx's
  KScheduler): each of the four cores runs one guest thread at a time, the
  highest-priority ready thread whose affinity mask allows that core, and
  threads of equal priority take turns only when one blocks or yields
  (svcSleepThread 0, -1 and -2). A thread waiting in an svc, an IPC request
  or a blocking Vulkan call gives its core up. Where Horizon preempts a
  thread at any instruction when a better one becomes ready, the runtime
  preempts it at its next svc or preemption point (a jump back closing a
  loop that calls, holds another loop or waits on memory, a computed jump,
  a recursive call; docs/abi.md section 14.1); the round robin of
  priorities 59 and 63 runs at svcs. A mutex owner inherits the priority of its waiters.

## Capturing with RenderDoc

RenderDoc hides `VK_EXT_external_memory_host`, so under it the Switch 2 graphics layer must mirror its memory pools (`nvn.memory`). Run the program under RenderDoc's layer, with the mirror forced for a Switch 2 game, and name the frame to capture:

```
ENABLE_VULKAN_RENDERDOC_CAPTURE=1 RELIFT_NVN_MEMORY=mirror RELIFT_RENDERDOC_CAPTURE=1200 <game>
```

The capture is written to `renderdoc.capture_dir`, by default the `captures` folder of the user data folder, as `<app>_capture.rdc`. Open it with `qrenderdoc file.rdc`, or script it: `qrenderdoc --python script.py` with the `renderdoc` module opens the file, lists the draws and reads textures and buffers (run it under `xvfb-run -a` for no window). The in-application API header is vendored in `third_party/renderdoc` (MIT, see its NOTICE); `core/capture.c` finds `RENDERDOC_GetAPI` in the library the layer loaded into the process and links nothing.
