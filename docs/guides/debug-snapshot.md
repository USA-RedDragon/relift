# Take a snapshot of a running game

This page is for a developer who is porting a game with relift and wants to know what the process holds and is doing right now, without stopping it: memory, threads, the last log lines, the settings in effect, and on Switch 2 the GPU time per pass and draw. A dev build writes one folder of plain files on a key press. The [reference](../reference/snapshot.md) lists the triggers and every file, read from the source; [Debugging settings](../reference/debug-settings.md) has the `debug.snapshot` setting.

The folder below is real: a dev build of a Game Boy test cartridge on Linux, triggered with the file. The Switch layer's and the Switch 2 graphics layer's files are described from the source and were not produced for this page.

## Trigger one

A snapshot needs a dev build (`relift run`, `relift build --dev`, a lane or debug-key build); a player package has none of the code. Three triggers:

- **Keyboard:** F12 in the window.
- **Controller:** hold Minus and click the right stick.
- **File:** create `snapshot-now` in the save folder, which the next present takes and removes. On a phone: `adb shell run-as <package> touch files/snapshot-now` for a debug-key APK.

The save folder is `save.dir` (`RELIFT_SAVE`), else the app's user folder. Nothing marks the screen. The log confirms it:

```text
$ RELIFT_SAVE=$PWD/save ./game --frames 900 --no-audio &
$ touch save/snapshot-now
[relift info snapshot] snapshot started at present 151: 3 presents into ~/save/snapshots/20261125-152542-p151
[relift info snapshot] snapshot written to ~/save/snapshots/20261125-152542-p151 (8 files, 0.1 s)
```

`debug.snapshot` (`RELIFT_DEBUG_SNAPSHOT=<n>`) is the number of presents the snapshot covers after the trigger, 3 by default. 0 turns every trigger off. The file is looked for twice a second and a present costs an atomic load and a clock read untriggered. The exact trigger code is on the [reference page](../reference/snapshot.md).

## The folder

`<save folder>/snapshots/<date>-<time>-p<present>/`, here:

```text
$ ls save/snapshots/*
build.txt  heap.txt  index.txt  log_tail.txt  memory.txt  picture.png  settings.txt  threads_host.txt
```

`index.txt` says what each file is, in the run's own words; start there. The list of files each layer can add is the table on the [reference page](../reference/snapshot.md#files). What the files from the test run held:

- `settings.txt`: one line per setting, `video.gpu_driver = system (default)`: the value and where it came from (default, env or set). Use it to check which settings the run really had.
- `build.txt`: `relift: 0.1.0`, the game and its guest, `code mods: none` and the build digest.
- `threads_host.txt`: a table of every host thread with its kernel state and what it sleeps in (`tid name state waiting in (wchan) cpu ticks`). A thread in `R` with ticks climbing is the busy one.
- `memory.txt`: the kernel's totals for the process (`/proc/self/status` first), its mappings by resident size, and the allocator's counters.
- `heap.txt`: without the heapsample library it says so, in two lines. To get call sites that hold the live heap, preload it: `LD_PRELOAD=<build>/heapsample.so`, built from `runtime/tools/heapsample/heapsample.c`, and read the file with `runtime/tools/heapsample/report.py`.
- `log_tail.txt`: the last 500 log lines, oldest first, ending with the snapshot's own start line.
- `picture.png`: the frame the window showed when the snapshot ended.

On a Switch program `threads_guest.txt` adds every guest thread: state, core, priority, the lifted function at its saved pc and what it waits on, as `info threads` shows in [gdb](debug-gdb.md) but without attaching. A Switch 2 program adds `gpu.txt` (the GPU profile of each present of the window, per pass and per draw, from `gpu.profile` and `RELIFT_GPU_PROFILE_DRAWS` turned on for the window only), `batches.txt` (why each batch ended, the NVN commands replayed and the host Vulkan calls per present), `memory_nvn.txt` (what the NVN layer holds, by size) and `trace/frame-<n>/` (a frame trace of one present in the format `RELIFT_NVN_TRACE` writes: `runtime/tools/nvn_trace.py` and the replay tools read it). The profile and the trace slow the presents they cover and are turned off when the window ends.

## When to use it

Use a snapshot first for "what is the game doing and holding right now": a hang (host and guest threads and what each waits on), memory growth (`memory.txt`, `heap.txt`), a slow frame (the GPU profile), or a report that needs the settings and build. Use [gdb](debug-gdb.md) when you need to stop the guest or read its registers and memory, and [RenderDoc](debug-renderdoc.md) when a picture is wrong.

## What is not there

- No on-screen sign that a snapshot was taken; read the log.
- The window backend that handles F12 and the chord is SDL; a run without a window has the file trigger only.
- `threads_guest.txt` and the GPU files appear only for the layers that are in the program, and no GPU files were produced for a program that is not a Switch 2 game.
