# Diagnostic snapshot

The reference for the dev build's diagnostic snapshot: how to trigger it, what
the folder holds and where each file comes from. The guide is
[Take a snapshot of a running game](../guides/debug-snapshot.md); the setting
is `debug.snapshot` ([Debugging settings](debug-settings.md)). Every excerpt on
this page is read from the source.

## Triggers and the folder

<!-- docgen:code file=runtime/core/snapshot.h region=overview -->
```c
* @brief The diagnostic snapshot, a complement to the GDB server for a development build: one trigger writes one
* folder that says what the process held and did around that moment, without stopping it.
*
* Triggers (all dev builds only, where the debug stub is built in; a player package has none of this code):
*  - F12 on the keyboard, or holding Minus and clicking the right stick on a controller (the window backend calls
*    rl_snapshot_request);
*  - a file named snapshot-now in the save folder (the setting save.dir, else the app's user data folder): created
*    by hand or by `adb shell run-as <package> touch files/snapshot-now` on a phone, taken and removed at the next
*    present.
* The setting debug.snapshot is the number of presents a snapshot covers after the trigger (default 3); 0 turns the
* triggers off. Untriggered, a present costs one atomic load, one clock read, and a file check every half second.
*
* The folder is <save folder>/snapshots/<date>-<time>-p<present>/ and always holds, from the core: index.txt (what
* every file is, in plain words), settings.txt, build.txt, log_tail.txt (the last 500 log lines), picture.png (the
* presented frame), memory.txt and heap.txt (the process's memory, with call sites when the heapsample library
* is preloaded), threads_host.txt (every host thread and what it waits in). A guest registers a provider for its own
* parts (a guest layer: guest threads, NVN batches, the GPU profile, a frame trace, the holdings of the
* Vulkan layer); the core knows no guest, CPU or GPU concept.
```
<!-- /docgen:code -->

### The keyboard key

<!-- docgen:code file=runtime/host/window_sdl3.c region=snapshot-key -->
```c
if (e->key.scancode == SDL_SCANCODE_F12 && e->key.down && !e->key.repeat)
    rl_snapshot_request();
```
<!-- /docgen:code -->

### The controller chord

<!-- docgen:code file=runtime/host/window_sdl3.c region=snapshot-chord -->
```c
/* The diagnostic snapshot's chord: Minus held, then a click of the right stick. */
bool chord = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_BACK) && SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
if (chord && !g_chord[i])
    rl_snapshot_request();
g_chord[i] = chord;
```
<!-- /docgen:code -->

SDL's `BACK` button is the Minus button of a Switch-style pad.

### The trigger file

<!-- docgen:code file=runtime/core/snapshot.c symbol=poll_file -->
```c
static bool poll_file(uint64_t now)
{
    if (now - atomic_load(&S.last_poll_ns) < POLL_NS)
        return false;
    atomic_store(&S.last_poll_ns, now);
    if (rl_setting_int("debug.snapshot") <= 0)
        return false;
    char root[1024], path[1100];
    root_dir(root, sizeof root);
    snprintf(path, sizeof path, "%s/snapshot-now", root);
    struct stat st;
    if (stat(path, &st) != 0)
        return false;
    remove(path);
    return true;
}
```
<!-- /docgen:code -->

The file is looked for at most every
<!-- docgen:const file=runtime/core/snapshot.c name=POLL_NS print=value -->`500000000`<!-- /docgen:const -->
nanoseconds. It lives in the folder this function names (the root is
`save.dir`, else the app's user folder, else the working folder):

<!-- docgen:code file=runtime/core/snapshot.c symbol=root_dir -->
```c
static void root_dir(char *out, size_t n)
{
    if (S.root_set) {
        snprintf(out, n, "%s", S.root);
        return;
    }
    const char *save = rl_setting_str("save.dir");
    char user[900];
    const char *app = rl_settings_app();
    if (save && *save)
        snprintf(out, n, "%s", save);
    else if (rl_settings_user_dir(user, sizeof user, app && *app ? app : "relift"))
        snprintf(out, n, "%s", user);
    else
        snprintf(out, n, ".");
}
```
<!-- /docgen:code -->

On a phone, `adb shell run-as <package> touch files/snapshot-now` makes the
file in the save folder of a debug-key app.

## Limits

| Limit | Value |
|---|---|
| Most providers (layers that add files) | <!-- docgen:const file=runtime/core/snapshot.c name=MAX_PROVIDERS print=value -->`8`<!-- /docgen:const --> |
| Most files an index lists | <!-- docgen:const file=runtime/core/snapshot.c name=MAX_FILES print=value -->`64`<!-- /docgen:const --> |
| Log lines kept in `log_tail.txt` | <!-- docgen:const file=runtime/core/snapshot.c name=LOG_LINES print=value -->`500`<!-- /docgen:const --> |
| Presents past the window a provider that is not ready is waited for | <!-- docgen:const file=runtime/core/snapshot.c name=LATE_PRESENTS print=value -->`60`<!-- /docgen:const --> |

## Files

Every file the runtime writes with a fixed name, from the calls that announce
it to `index.txt`. A file appears only when its layer is in the program: the
core's files are in every run of a dev build, the Switch layer's in a Switch
program, and the Switch 2 graphics layer's where it draws.

<!-- docgen:output exec="sh scripts/docs/snapshot-files.sh" -->
```text
| File | Written by | What index.txt says it is |
|---|---|---|
| `build.txt` | core | The build stamp: the version, the game, the code mods and the build digest the program registered. |
| `heap.txt` | core | Heap call sites: not available in this run (the heapsample library was not preloaded). |
| `log_tail.txt` | core | The last 500 log lines before the snapshot ended, oldest first. |
| `memory.txt` | core | The process's memory from the host: the kernel's totals, its mappings by resident size and the allocator's counters. |
| `settings.txt` | core | Every setting's effective value and where it came from (default, env or set). |
| `threads_host.txt` | core | Every host thread: its id, name, kernel state, what it sleeps in and the CPU time it used. |
| `heap.txt` | core | The heapsample dump: the call sites that hold the most live heap bytes (read it with tools/heapsample/report.py). |
| `picture.png` | core | The picture the window showed when the snapshot ended. |
| `batches.txt` | Switch 2 graphics layer | Per present: how many batches there were and where each ended (the call that forced the batch out), the NVN commands replayed and the host Vulkan calls by command, in total and per draw. |
| `gpu.txt` | Switch 2 graphics layer | The GPU profile of each present of the window: time per render pass and per draw (named by its programs and target size), dispatches, copies and the layer's own work, plus the gaps between batches and the presenter's own GPU time. |
| `memory_nvn.txt` | Switch 2 graphics layer | What the NVN layer holds: pipelines, pipeline libraries, shader modules, command sets, mirrors, staging buffers, and the textures by size with the largest listed. |
| `threads_guest.txt` | Switch layer | Every guest thread: state, core, priority, the lifted function at its saved pc and what it waits on (svc, address, event), as the GDB server's info threads gives, taken without stopping the guest. |
```
<!-- /docgen:output -->

The Switch 2 graphics layer also writes a frame trace of one present into
`trace/frame-<n>/`, as `trace.jsonl` (every NVN command with what it bound) and
`order.jsonl` (the present's submits, event waits and signals in queue order),
in the format `RELIFT_NVN_TRACE` writes; `runtime/tools/nvn_trace.py` reads
both. Which files a given snapshot holds is listed in its own `index.txt`.

`heap.txt` has two forms: the heap call sites when the heapsample library is
preloaded, and one explaining line when it is not.

## What a layer adds

A guest adds its own files with a provider: hooks for the start of the window,
each present and the end, and an optional `ready` hook that keeps the window
open until the provider has what it needs.

<!-- docgen:code file=runtime/core/snapshot.h symbol=rl_snapshot_provider -->
```c
typedef struct rl_snapshot_provider {
    /** @brief Short name for the log. */
    const char *name;
    /** @brief A snapshot starts: arm what only a triggered window should pay for. presents is the window length. */
    void (*begin)(void *user, const char *dir, uint32_t presents);
    /** @brief A present of the window finished (index 1 to presents). */
    void (*present)(void *user, const char *dir, uint32_t index);
    /**
     * @brief Whether the provider has what it needs from the window: asked at each present once the window's presents are
     * done, and the window stays open until every provider that has this hook says true (or 60 more presents pass).
     */
    bool (*ready)(void *user);
    /** @brief The window is over: write the provider's files into dir (each announced with rl_snapshot_file) and disarm. */
    void (*end)(void *user, const char *dir);
    void *user;
} rl_snapshot_provider;
```
<!-- /docgen:code -->
