# Capture a frame with RenderDoc

This page is for a developer who is porting a game with relift and needs to look at one frame on the GPU: which draw made a wrong picture, which guest program ran, what a render target held after a pass. relift triggers RenderDoc's frame capture from inside the program, labels the work it replays, and keeps the names of the guest's programs, so a capture can be mapped back to the guest. Read [Recompile a game](workflow.md) first. The settings used here are in [Debugging settings](../reference/debug-settings.md).

What is checked. The output below is real, from a dev build of a Game Boy test cartridge on Linux with RenderDoc 1.45 (`renderdoccmd` and `qrenderdoc` installed): the capture trigger, the log lines and the list of actions. The labels and image names of the Switch 2 graphics layer come from the source and are shown from it; no Switch 2 game was run for this page.

## What relift does under RenderDoc

The runtime does not link RenderDoc. When the program runs with RenderDoc's library in the process, the runtime looks the library up and starts and ends frame captures at the guest presents you name:

<!-- docgen:code file=runtime/core/capture.c symbol=rl_capture_tool -->
```c
bool rl_capture_tool(void)
{
    static const char *const names[] = { "librenderdoc.so", "renderdoc.dll", NULL };
    const char *layer = getenv("ENABLE_VULKAN_RENDERDOC_CAPTURE");
    return (layer && strcmp(layer, "1") == 0) || rl_hostlib_loaded(names) != NULL;
}
```
<!-- /docgen:code -->

Two settings control it, `renderdoc.capture` (`RELIFT_RENDERDOC_CAPTURE`) and `renderdoc.capture_dir` (`RELIFT_RENDERDOC_DIR`). The capture is `<present>[:<frames>]`, counting guest presents from 1, or `file`; the full description is in [Debugging settings](../reference/debug-settings.md). A capture is written as `<folder>/<app>_capture.rdc`, in the `captures` folder of the user data folder when `renderdoc.capture_dir` is empty. Only Vulkan work is captured: a guest that draws on the CPU shows only the presenter's own passes.

## Capture a frame

There are two ways to put RenderDoc in the process, and both were run on the test program. The first is to enable RenderDoc's Vulkan layer by environment, `ENABLE_VULKAN_RENDERDOC_CAPTURE=1`. This is also the form to use for the Switch 2 graphics layer, together with `RELIFT_NVN_MEMORY=mirror` (RenderDoc hides `VK_EXT_external_memory_host`, so the layer must mirror the game's memory pools, and the shader table must have been made for the run's own `shaders/device.json`):

```text
$ ENABLE_VULKAN_RENDERDOC_CAPTURE=1 RELIFT_RENDERDOC_CAPTURE=20 RELIFT_RENDERDOC_DIR=$PWD/caps \
    ./game --frames 40 --no-save --no-audio
[relift info window] RenderDoc's layer is on: using X11 (:0), as the layer has no Wayland surfaces
[relift info window] SDL x11 video driver
[relift info vulkan] core device up on <GPU name>, queue family 0, 2 queues, with swapchains
[relift info capture] RenderDoc captures go to ~/caps/relift_capture.rdc
[relift info vulkan] a capture tool is loaded: VK_EXT_debug_utils on, for command labels and object names
[relift info present] SDL3 window: swapchain 640x576, 4 images, format 44, fifo, transform 0x1 of 0x1
[relift info capture] RenderDoc capture started before frame 20
[relift info capture] RenderDoc capture ended after present 20 (written)
```

The layer loads RenderDoc's library only when the program makes its first Vulkan instance, and a presenter makes it inside the first present, after the capture setting was first read. The runtime therefore looks for the library again once Vulkan is up:

<!-- docgen:code file=runtime/core/capture.c region=vulkan-up -->
```c
* @brief RenderDoc's Vulkan layer (ENABLE_VULKAN_RENDERDOC_CAPTURE=1) loads its library into the process when a
* Vulkan instance is made, which for a presenter can be inside the first present, after the first look for it. So
* the look waits for the instance when the layer is enabled, and is made again here, once the core device is up or a
* guest's own instance is made; a capture that starts with present 1 starts here when that present is under way.
```
<!-- /docgen:code -->

The second way is to let `renderdoccmd` start the program. RenderDoc then puts its library in the process from the start:

```text
$ RELIFT_RENDERDOC_CAPTURE=5 RELIFT_RENDERDOC_DIR=$PWD/caps \
    renderdoccmd capture -w -d $PWD ./game --frames 12
Launching './game' with params: --frames 12
[relift info window] RenderDoc's layer is on: using X11 (:0), as the layer has no Wayland surfaces
[relift info window] SDL x11 video driver
[relift info capture] RenderDoc captures go to ~/caps/relift_capture.rdc
[relift info vulkan] core device up on <GPU name>, queue family 0, 2 queues, with swapchains
[relift info vulkan] a capture tool is loaded: VK_EXT_debug_utils on, for command labels and object names
[relift info capture] RenderDoc capture started before frame 5
[relift info present] SDL3 window: swapchain 640x576, 4 images, format 44, fifo, transform 0x1 of 0x1
[relift info capture] RenderDoc capture ended after present 5 (written)
```

The log lines say each step: X11 chosen, the capture folder, `VK_EXT_debug_utils` turned on, and the capture start and end. The window uses X11 under RenderDoc because the layer has no Wayland surfaces; with no `DISPLAY` there is no swapchain and every frame goes through host memory.

A capture of the first few presents can hold no GPU work: the window makes its swapchain a few presents in, and the frames before it carry none. On the test program a capture of present 5 held only the device's setup in some runs, under either way, and one of present 20 always held the copy, the blit and the present. Pick a present after the `SDL3 window: swapchain` line. If the log says `renderdoc.capture is set but RenderDoc is not loaded into the process`, neither way put RenderDoc in the process: check that RenderDoc's layer is installed (`renderdoc_capture.json` in a Vulkan `implicit_layer.d` folder) or use `renderdoccmd`.

### Capture the next frame by hand

`renderdoc.capture=file` waits for a file named `capture-next` in the capture folder; the next present is captured and the program removes the file:

```text
$ touch ~/caps/capture-next
```

This form was read from the code and not run for this page.

## Read the capture

Open it with `qrenderdoc caps/relift_capture.rdc`, or list it without a window with the script the runtime ships:

<!-- docgen:code file=runtime/tools/rdc_actions.py region=walk -->
```python
def walk(a, depth):
    print("  " * depth + "%d %s" % (a.eventId, a.GetName(sd)))
    for c in a.children:
        walk(c, depth + 1)


for a in ctrl.GetRootActions():
    walk(a, 0)
```
<!-- /docgen:code -->

```text
$ RDC=caps/relift_capture.rdc OUT=actions.txt QT_QPA_PLATFORM=offscreen \
    qrenderdoc --python runtime/tools/rdc_actions.py
$ cat actions.txt
5 => vkQueueSubmit(1)[0]: vkBeginCommandBuffer(ResourceId::133)
7 vkCmdCopyBufferToImage()
10 vkCmdClearColorImage()
12 vkCmdBlitImage()
14 => vkQueueSubmit(1)[0]: vkEndCommandBuffer(ResourceId::133)
15 vkQueuePresentKHR(ResourceId::105)
20 => vkQueueSubmit(1)[0]: vkBeginCommandBuffer(ResourceId::135)
22 vkCmdCopyBufferToImage()
25 vkCmdClearColorImage()
27 vkCmdBlitImage()
29 => vkQueueSubmit(1)[0]: vkEndCommandBuffer(ResourceId::135)
30 vkQueuePresentKHR(ResourceId::105)
32 End of Capture
```

That is a whole Game Boy frame: the lifted program draws on the CPU, so the capture shows the presenter's upload, clear and blit, with no draw of the game's own. (The script writes to a file because qrenderdoc's shell shows no standard output; `QT_QPA_PLATFORM=offscreen` is needed to run without a display.) `renderdoccmd convert -f x.rdc -o x.xml -c xml` writes the whole capture as XML to search.

## Switch 2: labels, names and guest programs

The Switch 2 graphics layer replays the game's NVN command sets as Vulkan. With a capture tool in the process it labels each draw and dispatch with the guest command and the programs, and names each image made for a guest texture or render target:

<!-- docgen:code file=runtime/guests/switch/nvn/nvn_label.c region=overview -->
```c
* @brief Debug labels and object names for a capture tool (RenderDoc): with VK_EXT_debug_utils on (the core turns it
* on only when a capture tool is in the process, rlvk_dev.debug_utils), each draw and dispatch the layer replays gets
* a label that names the guest command (its command set and index in the set, as the game recorded it) and the
* programs it runs, and each host image made for a guest texture or render target gets a name with its guest GPU
* address, format and size. A capture then lists the draws as the game issued them and the images by the game's own
* addresses. Without the extension every call returns at once.
```
<!-- /docgen:code -->

The label text is built here:

<!-- docgen:code file=runtime/guests/switch/nvn/nvn_label.c symbol=nvn_label_draw -->
```c
void nvn_label_draw(VkCommandBuffer cb, const char *kind, const char *const keys[6])
{
    if (!on())
        return;
    static const char *const stage[6] = { "vs", "tcs", "tes", "gs", "fs", "cs" };
    char text[256], k[9];
    int o = snprintf(text, sizeof text, "%s set %llu cmd %u (%s)", kind, (unsigned long long)L.set_seq, L.index,
                     nvn_cmd_name(L.op));
    for (uint32_t s = 0; s < 6 && o > 0 && (size_t)o < sizeof text; s++)
        if (keys[s])
            o += snprintf(text + o, sizeof text - (size_t)o, " %s %s", stage[s], short_key(keys[s], k));
    VkDebugUtilsLabelEXT l = { .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT, .pLabelName = text };
    L.insert(cb, &l);
}
```
<!-- /docgen:code -->

So an event reads `draw set 12 cmd 40 (<NVN call>) vs 1a2b3c4d fs 5e6f7a8b`: the command set (counted from 1 per set replayed), the command's index in the set as the game recorded it, the NVN call, and the first 8 hex digits of each program's key, per stage (`vs`, `tcs`, `tes`, `gs`, `fs`, `cs`). Searching the event browser for `fs 5e6f7a8b` finds every draw that ran that fragment program. Images are named `nvn 0x<GPU address> format <NVN format> <w>x<h>x<d>`, with `depth` and `target` added where they apply, so a resource can be matched to the guest address a trace or a gdb watch reports.

### From a label to the guest program and its translation

The key is the program's identity in the build: `programs/<key>.bin` in the shader folder is the guest program the runtime met (written when `RELIFT_NVN_RECORD` is set; see `RELIFT_NVN_SHADERS` in [Debugging settings](../reference/debug-settings.md)), and the translation of every program is in the table the build makes (`table.rlspv`, indexed by `table.rlspi`). To step a guest program in RenderDoc's shader debugger, translate it with debug info, using the translator's `xlate` example:

```text
$ cargo run -p relift-gpu-sm86 --example xlate -- <programs/key.bin> <out.spv> <device.json> --debug-info
```

The debug info is built so that:

<!-- docgen:code file=gpus/nv/src/translate/debug.rs region=overview -->
```rust
//! SPIR-V debug info (`Options::debug_info`): NonSemantic.Shader.DebugInfo.100
//! whose source is the guest program's disassembly, one line per
//! instruction (`/*pc*/ text ;`, line n for the instruction at byte
//! 16 (n - 1)). Each guest instruction's code follows a `DebugLine` of its
//! line, and each register write a `DebugValue` of the guest register as a
//! local variable named `R0`..`R255`, `UR0`..`UR63`, `P0`..`P6` or
//! `UP0`..`UP6`. RenderDoc's shader debugger then steps the guest program
//! with the values of a capture. Programs on the software warp path get
//! lines but no register values (each lane's registers are an array).
```
<!-- /docgen:code -->

`device.json` must be the one the run wrote, in its shader folder, so the SPIR-V is the one the run used. Opening `<out.spv>` is not the capture's own shader; compare it with the capture's SPIR-V for the same stage when a mismatch is the question.

## Limits

- The in-application trigger looks only for `librenderdoc.so` and `renderdoc.dll`, so a host whose RenderDoc library has another name (macOS, with MoltenVK, or the Android layer) does not find it and logs the warning above. Neither was run for this page.
- Labels and image names are the Switch 2 layer's. Other guests show the core presenter's passes without labels.
- Under the layer a Switch 2 game runs several times slower, and the mirror needs a shader table made for the run's `device.json`: a table made for a device that imports guest memory stops the run with an error saying so.
- A device loss cannot be captured.
