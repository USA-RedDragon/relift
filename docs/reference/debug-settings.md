# Debugging settings

The runtime's debugging aids are set with environment variables (and, for
those that are also settings, with `--relift-set key=value` or
`settings.json`; see [Settings](configuration.md) and
[Using relift](../guides/usage.md)). This page lists them. Every entry comes from
code: the settings table of the core, the table of environment variables in
[the C runtime](../../runtime/README.md), and the source files, for variables
that read the environment and have no row in that table. A
variable with a doc comment shows the first line of it; read the rest at the
file named. A debugging aid is built into dev builds; the GDB server and the
snapshot are never in a player package.

The guides that use these: [Debug a game with gdb](../guides/debug-gdb.md),
[Capture a frame with RenderDoc](../guides/debug-renderdoc.md) and
[Take a snapshot of a running game](../guides/debug-snapshot.md).

## Core settings

The settings the core declares for debugging. Each entry has the key, the label,
the environment variable, the type and range, and the description, as the
source declares them.

<!-- docgen:code file=runtime/core/settings.c region=debug-settings -->
```c
{
    .key = "renderdoc.capture",
    .type = RL_SETTING_STRING,
    .label = "RenderDoc capture",
    .description = "<present>[:<frames>]: when the program runs under RenderDoc, capture that many frames "
                   "(default 1) from the numbered guest present (counting from 1), including runs with no "
                   "window. file: capture the next present each time a file named capture-next appears in "
                   "the capture folder (the program removes it). Only Vulkan work is captured. Empty: off",
    .env = "RELIFT_RENDERDOC_CAPTURE",
},
{
    .key = "renderdoc.capture_dir",
    .type = RL_SETTING_STRING,
    .label = "RenderDoc capture folder",
    .description = "Where RenderDoc writes the captures renderdoc.capture asks for. Empty: the captures folder of "
                   "the user data folder",
    .env = "RELIFT_RENDERDOC_DIR",
},
{
    .key = "present.capture",
    .type = RL_SETTING_STRING,
    .label = "Window capture (debugging)",
    .description = "Debugging: <folder>[:<every>][:<from>-<to>] writes the window's pictures exactly as they reach "
                   "the screen, after relift's scaling, picture passes and overlays, as "
                   "<folder>/present-<frame>-d<draw>.png (frame: the number of the guest frame handed to the "
                   "window; draw: the number of the swapchain image drawn, counting from 1), every <every>th draw "
                   "(default 1), only draws of the frames from <from> to before <to> when given, with a log line "
                   "each saying what the presenter drew. Each capture waits for the GPU. Empty: off",
    .env = "RELIFT_PRESENT_CAPTURE",
},
{
    .key = "present.flash",
    .type = RL_SETTING_STRING,
    .label = "Flash watch (debugging)",
    .description = "Debugging: <folder>[:<threshold>] reads back every new frame the window shows and, when one "
                   "frame differs sharply from the frames on both sides while those two agree, writes the three as "
                   "<folder>/flash-<frame>/present-<n>.png with flash.json (threshold in luminance steps, default "
                   "10). Each new frame waits for the GPU. Empty: off",
    .env = "RELIFT_PRESENT_FLASH",
},
{
    .key = "debug.gdb",
    .type = RL_SETTING_INT,
    .label = "GDB server port",
    .description = "A port on 127.0.0.1 where a GDB remote-protocol server waits for gdb or lldb, which then see "
                   "the guest's threads, registers and memory by guest address. 0: off",
    .env = "RELIFT_DEBUG_GDB",
    .def_int = 0,
    .min_int = 0,
    .max_int = 65535,
},
{
    .key = "debug.gdb_wait",
    .type = RL_SETTING_BOOL,
    .label = "Wait for the debugger",
    .description = "With debug.gdb set, the game starts only once a debugger attached and continued",
    .env = "RELIFT_DEBUG_GDB_WAIT",
    .def_int = 0,
},
{
    .key = "debug.snapshot",
    .type = RL_SETTING_INT,
    .label = "Diagnostic snapshot presents",
    .description = "In a development build, F12, holding Minus while clicking the right stick, or a "
                   "snapshot-now file in the save folder writes a snapshot folder (memory, GPU, batches, "
                   "threads, the picture and the settings) covering that many presents after the trigger. "
                   "0: no trigger works",
    .env = "RELIFT_DEBUG_SNAPSHOT",
    .def_int = 3,
    .min_int = 0,
    .max_int = 600,
},
```
<!-- /docgen:code -->

## Documented variables

<!-- docgen:output exec="sh scripts/docs/debug-settings.sh documented" -->
```text
| Variable | What it does |
|---|---|
| `RELIFT_TRACE_OS=1` | The guest layer logs every guest OS call and service request with arguments, result and time (Switch: every svc and IPC command) |
| `RELIFT_PRESENT_TRACE=<file>` | The `video.present_trace` setting: one line per presented frame, `<handed over> <queued> <shown>` in host clock nanoseconds, where the device has VK_KHR_present_wait |
| `RELIFT_GPU_PROFILE=1` | The `gpu.profile` setting: logs where each Switch 2 batch's GPU time goes, the time between batches and the presenter's GPU time. See "Profiling the GPU" below |
| `RELIFT_GPU_PROFILE_DRAWS=1` | Switch 2, debugging, with `gpu.profile`: a timestamp after every draw, so the ranking lists `draw <size> vs <program> fs <program>` with its GPU time and draws per present, and the by-kind line groups draws by target size. Valid where the pass runs in system memory (Turnip on the Adreno 740 does at 960x540: compare a pass's time with its draws' sum); the first draw of a pass also carries its load operations. Slows the game |
| `RELIFT_RENDERDOC_CAPTURE=<present>[:<frames>]` | The `renderdoc.capture` setting, for every guest. When the program runs under RenderDoc (its library already loaded), start a frame capture before frame `<present>` (counting guest presents from 1; frame k ends at present k) and end it after `<frames>` frames (default 1). With `file` it captures the next present each time a file named `capture-next` appears in the capture folder (`touch <folder>/capture-next` from a shell or a script; the program removes it), so a capture can be taken at a moment found by watching. A present is counted where a guest hands a finished frame to the shared presenter (`rl_window_present_rgb`, the GPU present slots) and, for a Switch run with no window, in the graphics layer; it works without a window. Only Vulkan work is captured: a guest that draws on the CPU shows nothing but the presenter's own passes, and a run with no window and no Vulkan device has nothing to capture. See "Capturing with RenderDoc" below |
| `RELIFT_PRESENT_CAPTURE=<folder>[:<every>][:<from>-<to>]` | Debugging: the `present.capture` setting, for every guest. Writes the window's pictures exactly as they reach the screen (the swapchain image after relift's scaling, picture passes and overlays) as `<folder>/present-<frame>-d<draw>.png`, every `<every>`th draw, only for the frames handed to the window from `<from>` to before `<to>` when given (frames count presents, so overlay redraws cannot shift the range), with a log line per file naming the source (slot, host pixels or still), sizes, formats, placement, sampling and the overlay. Each capture waits for the GPU. Unlike `RELIFT_VK_CAPTURE`, which reads the guest's own image, this shows what the presenter made of it |
| `RELIFT_PRESENT_FLASH=<folder>[:<threshold>]` | Debugging: the `present.flash` setting, for every guest. Reads back every new frame the window shows, signs it for the frame jump detector (`core/framejump.h`) and, when one frame differs sharply from the frames on both sides while those two agree, writes the three as `<folder>/flash-<frame>/present-<n>.png` with `flash.json` (the differences; threshold in luminance steps, default 10; at most 8 per run). Each new frame waits for the GPU. The NVN layer's `RELIFT_NVN_FLASH` adds per-image thumbnails for Switch games. Code: `core/flashwatch.c` |
| `RELIFT_RENDERDOC_DIR=<folder>` | The `renderdoc.capture_dir` setting: where the captures are written, as `<folder>/<app>_capture.rdc`. Empty (default): the `captures` folder of the user data folder (`~/.local/share/<app>/captures` on Linux) |
| `RELIFT_DEBUG_GDB=<port>` | The `debug.gdb` setting: a GDB remote-protocol server on `127.0.0.1:<port>`. gdb or lldb attach with `target remote :<port>` and see the guest's threads (`info threads`, with each thread's state and the lifted function at its pc), its registers (A64: x0-x30, sp, pc, cpsr, v0-v31, fpsr, fpcr, tpidr, tpidrro) and its memory by guest address. Attaching or interrupting stops every guest thread at its next safe point (a system call, a runtime entry or a preemption check); `continue` runs it on, `detach` leaves it running. `monitor sym <address>` names the lifted function at a guest address. In the program everywhere but Windows and iOS, except player packages (an AppImage, a signed .app, an iOS app, an APK signed with the project's key), which relift builds with `RL_NO_DEBUG_STUB`; on a phone reach it with `adb forward tcp:<port> tcp:<port>`. 0 (default): off. See "Debugging guest code with gdb" below |
| `RELIFT_DEBUG_GDB_WAIT=1` | The `debug.gdb_wait` setting: with `debug.gdb`, the game starts only once a debugger attached and continued |
| `RELIFT_DEBUG_SNAPSHOT=<n>` | The `debug.snapshot` setting: the number of presents a diagnostic snapshot covers after its trigger (default 3). 0: no trigger works. Built in wherever the GDB server is (dev, lane and debug-key builds, never player packages). See "Diagnostic snapshot" below |
| `RELIFT_VK_CAPTURE=<dir>[:N]` | Debug: write every Nth presented guest swapchain image (default every one) as `<dir>/frame-<number>-<guest ms>ms.png`. Each capture waits for the GPU, so it slows the game; off when unset |
| `RELIFT_VK_CAPTURE_WINDOW=<from>-<to>` | Debug: with `RELIFT_VK_CAPTURE`, capture every present from number `from` up to (not including) `to` instead of every Nth |
| `RELIFT_UPSCALER_DUMP=<folder>:<first>[-<last>]` | Debug: the FSR 2 and FSR 3 upscalers write every image of their own (lock status, reconstructed previous depth, dilated motion vectors, reactive masks, luma history, accumulation, internal upscaled color, relift's transparency and composition mask) and the frame's constants after evaluations `first` to `last` (each upscaler counts its evaluations from 1; at most 16) into `<folder>` as raw files with a manifest; `runtime/tools/fsr_dump.py` makes PNGs of them and prints every image's values at a pixel |
| `RELIFT_NVN_TRACE=<dir>:<present>[:<frames>]` | Switch 2, debugging: write one frame of the NVN layer's draws and dispatches to `<dir>/frame-<n>/`, starting at the first present at or past `<present>` and covering `<frames>` frames (default 1). `trace.jsonl` has a line per draw, mesh draw (kind `mesh`, with its first task and task count as `first` and `count`) or dispatch: the programs, render targets, fixed-function and dynamic state, every bound uniform and storage buffer with its GPU and guest address, the pushed constants, and every texture and image it binds with its host image, format, size, view and swizzle. `bufs/` and `imgs/` hold the bytes the lines name, before and after the command (images: every level and layer, level 0 first, in the host image's format; block formats and volumes only level 0), each file named by a hash so a range that did not change is written once. Every traced command submits the batch before and after it, so the game runs slowly and only the traced frames pay. `RELIFT_NVN_TRACE_GBUFFER=1` starts it at the first frame after one that drew into four or more render targets at once, since the present a scene reaches varies between runs. `tools/nvn_trace.py` reads a trace |
| `RELIFT_NVN_TRACE_WATCH=<guest hex>:<size>` | Switch 2, debugging: watch a guest range. Each change is logged (`nvn-watch`, with the present and the command that first sees it) and, inside a traced frame, is a `watch` line naming the command after which it changed (`cpu` for a change between commands) |
| `RELIFT_NVN_GPU_TRACE=1` | Switch 2, debugging: bind the trace buffer of trace-mode translations (`relift shaders <build> --trace <key prefix>:<condition> --trace-dir <dir>`, run with `RELIFT_NVN_TABLES=<dir>` and `RELIFT_NVN_TABLE_STAMP=allow`). After each dispatch of a traced compute program the layer logs (`gpu-trace`) every instruction whose register write met the condition (NaN, infinity, denormal, or any value of a watched register), with the value, the values it read, the invocation, and which came first in time; at the first dispatch with hits it also logs the present's NVN commands so far and the compute uniform bindings. At most 64 hits a dispatch are shown, in program order: `RELIFT_NVN_GPU_TRACE_LINES=<n>` changes the limit and `RELIFT_NVN_GPU_TRACE_PCS=<from>-<to>` (hex) shows only the instructions in that range. Compute, vertex, fragment, task and mesh programs; each stage records into its own region, so several stages trace in one draw (a vertex hit names its vertex and instance, a fragment hit its pixel). The condition `miss` records each global access whose address lies in no pool or storage slot, which goes to the device as a raw guest address (`miss address low` with the high word as its source); `--trace '*:miss'` traces every program |
| `RELIFT_NVN_CHECK_ADDRS=1` | Switch 2, debugging: before each draw and dispatch, a storage buffer slot of its programs whose range is not inside one current memory pool is logged once per program and slot (`nvn-check`), with the program, stage, slot and range |
| `RELIFT_NVN_FLASH=<dir>[:<threshold>]` | Switch 2, debugging: catch a single wrong frame between correct ones. Each present gets a 64x36 luminance signature; a present that differs from both neighbours by at least `<threshold>` luminance steps (mean absolute difference, default 10) while the neighbours agree saves `<dir>/flash-<n>/`: the presented pictures of presents n - 1 to n + 1, a thumbnail sheet of every image each of them wrote in order (render passes, texture clears, dispatches, copies, uploads, alias syncs, upscaler outputs) with a JSON line per thumbnail, and each present's trace lines (as `RELIFT_NVN_TRACE`'s, plus notes for images made and freed, uploads, copies, alias syncs and write-backs), kept in memory for the last 4 presents and written only on a trigger. Costs a copy and a small readback per present, with no extra submits. `tools/nvn_flash.py <dir>/flash-<n>` names the first image whose thumbnail jumped and the trace lines before it; `tools/flash_loop.sh` runs a game again and again until the trigger fires |
| `RELIFT_NVN_DIFF=<dir>[:<detectors>]` | Switch 2, debugging: differential capture of intermittent GPU corruption. Every compute dispatch of the last 2 frames is recorded into a device-local arena (`RELIFT_NVN_DIFF_MIB`, default 1536 per frame) by copies in the frame's own batches: the bytes of every bound range (storage buffers, user uniform banks, compute bases) and texture the frame had not touched yet, and after each dispatch every range and storage image it may write. `RELIFT_NVN_DIFF_KEYS=<hex prefix>[,...]` limits it to those programs. A trigger (core `trigger.h`) names a bad present: `zband` (thin runs of exact black over content the present before, the default detector), `jump` (a one-frame luminance outlier), `none`, plus `RELIFT_TRIGGER_AT=<present>[,...]`, `RELIFT_TRIGGER_FILE=<path>` (touch it) and the flash trigger. The named frame goes to `<dir>/diff-<present>/` (at most `RELIFT_NVN_DIFF_MAX`, default 3). `tools/nvn_diff.py` replays its dispatches from their captured inputs on the GPU (krun, repeated) and on the SPIR-V interpreter, compares every output word for word with the game's, and names the first divergent dispatch with a verdict: sync (the game alone differs), race (the replays differ), translation (GPU and interpreter differ). `RELIFT_NVN_DIFF_EXACT=<hex prefix>[,...]` also copies every input of those programs just before and again just after them, and the tool names any input that changed between the earlier dispatches and the start, or during the dispatch (a writer outside the captured dispatches). For a translation or race verdict, `tools/spv_log.py` builds a value log of one invocation, and the tool runs it on the GPU and on the interpreter. The first value that differs is reported with its SPIR-V id and guest PC. Images that differ only by unorm rounding are reported as `within_rounding`, and texels that are exact zeros only in the game's output as `zero_only_in_first`. Copies add a barrier around each dispatch, so captured dispatches never overlap |
| `RELIFT_NVN_POOL_LOG=1` | Switch 2, debugging: one `nvn-pool` log line per memory pool made and finalized (object, memory, size, flags, backend, GPU address alignment, GPU address last), per texture the driver initializes or refuses (target, format, size, levels, flags, pool, offset, storage, the driver's storage size and alignment) and per texture id registered (texture, storage, format, size). Two runs with different `nvn.memory` compare with `grep nvn-pool log | sed 's/ gpu 0x.*//'` and diff: the first differing line is the first guest-visible difference |
| `RELIFT_NVN_STATS=<file>` | Switch and Switch 2, debugging: at exit, write which NVN procs the game looked up and called (kind impl, zero, recorded, missing or driver) and, per command, how many were recorded and how each replay went (ok, ignored, partial, stopped, unhandled); lines with a gap are flagged `gap`. `tools/nvn_stats.py <game>=<file> ...` merges several games into one table of the gaps, ranked by how many games hit each |
| `RELIFT_INPUT_SCRIPT=<steps>` | Debugging only: hold port controls at guest frames so a run needs nobody at the controls. Comma separated `<frame>[+<frames>]:<port>.<control>[=<value>]`, for example `600:p1.plus,900+10:p1.a,1200+30:p1.lstick_y=-1`; a step holds the control at the value (default 1) for the frames (default 6) and replaces what its bindings read. Switch and Switch 2 count vi vsyncs since boot (1/60 s each, independent of how fast the host presents); GBA, DS and N64 count video frames. `@<path>` follows a file that a driver appends steps to. See docs/reference/input.md |
| `RELIFT_LOG=debug\|info\|warn\|error` | Lowest log level printed |
| `RELIFT_SCHED=free\|stats` | Debugging only. `free`: Switch guest threads run freely on host threads, ignoring priorities and cores (the old behavior, for comparisons). `stats`: log scheduler hand-overs, preemptions and the longest preemption delay once a second |
| `RELIFT_NVN_BATCH_LOG=1` | Switch 2: one log line per host batch with when it was handed to the driver (host clock) and when the device ran it (device clock, so only differences inside one present mean anything): shows where the GPU waits for the replay and where a present's batches overlap the CPU. Needs the GPU timestamps (`gpu.profile` off is fine) |
```
<!-- /docgen:output -->

## Variables with a source comment only

<!-- docgen:output exec="sh scripts/docs/debug-settings.sh source" -->
```text
| Variable | Read in | First line of its doc comment |
|---|---|---|
| `RELIFT_AUDIO_STATS` | `runtime/core/audio.c` | RELIFT_AUDIO_STATS=1 (anything but empty or 0): logs each stream's rate, frames written, underruns, drops and queue depth periodically. |
| `RELIFT_COMPILE_CHECK_FAULT` | `runtime/guests/switch/nvn/tools/compile_check.c` | A self-test aid: RELIFT_COMPILE_CHECK_FAULT=<index>:crash or <index>:hang makes the child die or stall on that |
| `RELIFT_FP_SLOW_STATS` | `runtime/cpus/arm64/fp_slow.c` | RELIFT_FP_SLOW_STATS=1 counts the calls of rl_fp_slow by instruction (its register fields cleared) and FPCR, and prints |
| `RELIFT_GUEST_WRITE_WATCH` | `runtime/guests/switch/nvn/nvn_watch.c` | The guest write watch, a debug tool: RELIFT_GUEST_WRITE_WATCH=<addr>:<len>[,<addr>:<len>...] (hex with 0x, |
| `RELIFT_N64_TRACE` | `runtime/guests/n64/run/n64main.c` | RELIFT_N64_TRACE=<file>: a development trace of the first VI frame and cycle of every lifted call site, written to the file (in the build with RL_N64_TRACE). |
| `RELIFT_NVN_DIFF_EXACT` | `runtime/guests/switch/nvn/nvn_diff.c` | RELIFT_NVN_DIFF_EXACT: programs whose every input is copied just before and again just after them. |
| `RELIFT_NVN_DIFF_KEYS` | `runtime/guests/switch/nvn/nvn_diff.c` | the dispatch's place in the submit order are kept on the CPU. RELIFT_NVN_DIFF_KEYS=<hex prefix>[,...] limits the |
| `RELIFT_NVN_DIFF_MAX` | `runtime/guests/switch/nvn/nvn_diff.c` | RELIFT_NVN_DIFF_MAX=<n>: the most captures one run saves (default 3). |
| `RELIFT_NVN_DIFF_MIB` | `runtime/guests/switch/nvn/nvn_diff.c` | (RELIFT_NVN_DIFF_MIB each, default 1536) by commands in the frame's own batches, with no extra submit: |
| `RELIFT_NVN_DUMP_UPLOAD` | `runtime/guests/switch/nvn/nvn_vk.c` | RELIFT_NVN_DUMP_UPLOAD=format:width:height:path writes the guest bytes of the first image of that format and size |
| `RELIFT_NVN_GPU_TRACE_LINES` | `runtime/guests/switch/nvn/nvn_draw.c` | Which hits a GPU trace report shows: at most RELIFT_NVN_GPU_TRACE_LINES of them (default 64), those whose |
| `RELIFT_NVN_GPU_TRACE_PCS` | `runtime/guests/switch/nvn/nvn_draw.c` | instruction lies at RELIFT_NVN_GPU_TRACE_PCS=<from>-<to> (hex, inclusive; default every one), so a report can reach |
| `RELIFT_NVN_IMAGE_WATCH` | `runtime/guests/switch/nvn/nvn_vk.c` | RELIFT_NVN_IMAGE_WATCH=format:width:height logs each write to a host image of that format and size (or whose size |
| `RELIFT_NVN_SET_LOG` | `runtime/guests/switch/nvn/nvn_cmd.c` | RELIFT_NVN_SET_LOG=1: a log line when a command set ends recording and when it is submitted, to read how long a set waits between. |
| `RELIFT_NVN_TRACE_GBUFFER` | `runtime/guests/switch/nvn/nvn_trace.c` | RELIFT_NVN_TRACE_GBUFFER (set to any value): with RELIFT_NVN_TRACE, trace only the presents whose frame drew a wide-target pass (the G-buffer pass of a deferred renderer). |
| `RELIFT_NVN_TRACE_WATCH_POISON` | `runtime/guests/switch/nvn/nvn_trace.c` | RELIFT_NVN_TRACE_WATCH_POISON=<hex word>: the word the watched range is filled with at each present (0: off). |
| `RELIFT_PIPE_STATS` | `runtime/guests/switch/nvn/nvn_pipe.c` | RELIFT_PIPE_STATS names this key's fragment program: the pipeline is made whole with statistics capture. |
| `RELIFT_STATS` | `runtime/guests/linux/start.c` | Logs how many instructions ran in the cold copy when RELIFT_STATS=1. |
```
<!-- /docgen:output -->

## Log level

`RELIFT_LOG` and the `[relift <level> <area>]` prefix of each line are
described in [the C runtime](../../runtime/README.md). A line with the area
`gdb`, `capture`, `snapshot`, `vulkan` or `window` names what the debugging
aids above did in a run.
