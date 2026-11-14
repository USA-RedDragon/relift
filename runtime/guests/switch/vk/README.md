# Vulkan forwarding (`runtime/guests/switch/vk`)

A Switch game that renders with Vulkan calls `vkGetInstanceProcAddr` from the
`vulkan` module and nothing else in it (see 001-devex section 7). With
forwarding, that module is never lifted: the import resolves to a thunk in this
layer, every entry point the guest looks up is a thunk, and each thunk calls the
host's Vulkan driver.

## Placement

| Part | Where | Why |
|---|---|---|
| Generator | `guests/switch/vkgen` (`relift-switch-vkgen`, binary `relift-vkgen`) | A Switch-guest tool: it knows the guest's ABI (AArch64 LP64, guest addresses). Standalone in the workspace charter, like `relift-switch-tools`: no crate depends on it and it depends only on `relift-cache`. |
| Configuration | `guests/switch/vkgen/vkgen.toml` | The registry pin (tag, URL, sha256), core 1.0 to 1.3, the extension list, the hand-written commands and structs, the dropped types and the shadow-memory hooks. A game repository passes a second file with its extra extensions; lists concatenate. |
| Generated C | `runtime/guests/switch/vk/gen/` (committed, `linguist-generated`) | Types and layout asserts (`vkg_types.h`), the command table (`vkg.h`, `vkg_thunks.c`), the converters (`vkg_convert.c`). CI regenerates them from the pinned `vk.xml` and fails on any difference. |
| Hand-written C | `runtime/guests/switch/vk/*.c` | Handle table, arena, host loading, instance and device creation, memory mapping, callbacks, surfaces. |
| Host library loading | `runtime/core/hostlib.c` | `dlopen` or `LoadLibrary` and the loader names per host (`libvulkan.so.1`, `libvulkan.1.dylib` or `libMoltenVK.dylib`, `vulkan-1.dll`), overridable with `RELIFT_VULKAN_LIBRARY`. The guest layer never names a host library. |

The layer belongs to the guest layer, not a new one: it knows Horizon's
guest (its ABI, its `VK_NN_vi_surface`), and the core charter forbids that in
`runtime/core`. Only the host-library shim sits in core, and it knows no guest
and no CPU.

The generated code needs no host Vulkan headers: `vkg_types.h` is written from
`vk.xml`, so every host target builds the layer the same way.

## Calls

A thunk has the lifted-code signature (`int (rl_ctx *, uint8_t *mem)`) and is
registered as a unit at `rl_vk_config.thunk_base`, one 4-byte slot per command,
so the guest gets ordinary guest addresses and calls them with `BLR`. The thunk
reads its arguments as AAPCS64 passes them: integers and pointers in x0 to x7,
floats in s0 to s7, the rest in 8-byte slots from the guest sp. Results go to
x0.

Guest pointers are guest addresses. The generator sorts every type into one of
two classes and checks it at build time:

- Plain: no pointer, size_t, dispatchable handle, function pointer or platform
  type anywhere inside. Guest bytes are host bytes (guest layout equals host
  layout on every 64-bit little-endian host; the static asserts in
  `vkg_types.h` fail the build otherwise), so the host gets `mem + addr`.
- Converted: everything else. The thunk copies it into a per-thread arena that
  lives for one call, translating each pointer (`mem + addr`), each array of
  structs, each string array, each dispatchable handle and each pNext chain.
  Structs the host writes are copied back field by field after the call
  (handles wrapped, pNext chains walked in parallel). The count-and-array
  protocol copies back at most what the guest asked for.

pNext chains are rebuilt from the sType of each guest struct. A struct the
generator does not know is reported once (`rl_report_unknown`) and dropped from
the chain.

## Physical device

The Switch has one GPU, so the guest sees one physical device and one device
group of it (`vk_device.c`), chosen when the instance first enumerates them.
`RELIFT_VK_DEVICE` picks it by a part of its name. Otherwise it is the first
host device whose queue family 0 has graphics and compute and, when the guest
enabled `VK_NN_vi_surface`, that can present through the surface provider
(`rl_vk_surface_provider.presents`; the headless provider asks the host with a
probe surface, since not every driver presents to `VK_EXT_headless_surface`).

## Queue families

The guest sees the Switch's layout on every host (`vk_queue.c`): one family,
graphics, compute, transfer and sparse binding, 16 queues, 64 timestamp bits,
a 1x1x1 transfer granularity. This is the family the console's NVIDIA driver
always builds; it adds a transfer family and a compute family only when its
hardware queries allow, so those are not shown. Games size their arrays for the
console: one engine fills a four-entry `VkDeviceQueueCreateInfo` array with
every family it is shown.

The family stands for host family 0, which must have graphics and compute
(true of every desktop and mobile driver seen so far; `vkCreateDevice` fails
with a log line otherwise), so family indices pass through unchanged and a
guest family other than 0 is refused. Guest queue `i` is host queue
`i % n` of the `n` the host has. When the guest creates more queues than the
host has, its queues share host queues, and every queue operation on that
device (`vkQueueSubmit`, `vkQueueSubmit2`, `vkQueueBindSparse`,
`vkQueuePresentKHR`, `vkQueueWaitIdle`, `vkDeviceWaitIdle`) takes the device's
queue lock, which Vulkan's external synchronization rules would otherwise leave
to the guest.

## Handles

Dispatchable handles (instance, physical device, device, queue, command
buffer) are host pointers, so the guest never sees them: it gets ids
(`0x56` in the top byte, a slot and a kind) from a table that also holds the
function table for calls on that handle. Non-dispatchable handles are 64-bit
values and pass through unchanged. A handle the guest did not get from the
layer fails the call with `VK_ERROR_UNKNOWN` and a log line; destroying an
instance or device drops every handle under it.

## Mapped memory

Host pointers reach guest memory only where the spec hands one out:
`vkMapMemory`. The guest gets a guest address in the map window
(`rl_vk_config.map_base`, `map_size`), backed in one of three ways, chosen per
device (spike 0.5; `RELIFT_VK_MAP=placed|import|shadow` forces one):

1. Placed (`VK_EXT_map_memory_placed` with `memoryUnmapReserve`): the driver
   maps the allocation at `mem + addr`; unmapping keeps the range reserved.
   NVIDIA has it; lavapipe and MoltenVK do not.
2. Import (`VK_EXT_external_memory_host`): a host-visible allocation is created
   on guest pages, aligned to `minImportedHostPointerAlignment` (16 KiB on Apple
   M2) and the host page; buffers get the matching external-memory flag.
   A dedicated buffer allocation drops the dedication when the host does not
   require it and is imported; dedicated image and exported allocations fall
   back to shadow.
3. Shadow: the guest gets its own pages, plus a third copy of the bytes both
   sides held at the last synchronization. Map copies host to guest; every
   submit and present moves the bytes the guest changed to the host; every
   completed wait moves the bytes the device changed to the guest; flush and
   invalidate do the same for their ranges. A write survives unless the other
   side wrote the same byte before the next synchronization (a race in the
   guest). Each synchronization compares whole allocations, so shadow is the
   slowest mode.

The pages are given the configured guest memory tag with `rl_vm_set` (through
`rl_vk_config.vm_set` when the guest layer wants its own lock), and unmapped
ranges go back to no access, so a stale guest pointer faults.

## Callbacks

`VkAllocationCallbacks` is never forwarded: the host driver gets NULL and uses
its own allocator. Debug messengers and debug report callbacks are
trampolined: the host gets a host function, and when the driver calls it during
a guest Vulkan call on that thread, the trampoline copies the callback data
below the guest sp (strings, labels, object names with handles translated),
saves the guest registers, calls the guest function through `rl_indirect` and
restores them. A callback on a driver thread with no guest call running is
logged and not delivered.

## `VK_NN_vi_surface`

The host has no `VK_NN_vi_surface`. The layer advertises it when a surface
provider (`rl_vk_config.surfaces`) is configured and the host has the
provider's instance extensions. When the guest enables it, `vkCreateInstance`
enables the provider's host extensions instead, and `vkCreateViSurfaceNN`
calls the provider with the host instance and the guest address of the
`nn::vi::NativeWindowHandle`. Everything after that is plain `VK_KHR_surface`
and `VK_KHR_swapchain` forwarding.

Providers:

- Headless (`rl_vk_headless_surfaces`, `VK_EXT_headless_surface`): tests and
  offscreen runs. Lavapipe presents to it; NVIDIA's Linux driver 615 does not.
- Window (`rl_vk_window_surfaces`, `vk_window.c`): the player. It needs a
  window backend that is up (`core/include/relift/window.h`; SDL3 in
  `runtime/host/window_sdl3.c`, built with `VIDEO=sdl3`). Each `vi` layer the
  guest draws on gets one resizable host window, opened at the layer's size
  (`SetLayerSize`, else the display resolution) when the guest creates its
  first surface on it. The guest's `nn::vi::NativeWindowHandle` points into
  SDK data, so the provider maps a handle it has not seen to the layer the
  guest opened or created last (`rl_vi_render_layer`) and remembers the pair.
  The provider's `instance_extensions` are the backend's
  (`SDL_Vulkan_GetInstanceExtensions`) and `create` calls
  `SDL_Vulkan_CreateSurface`. The player loads Vulkan through the backend
  (`SDL_Vulkan_LoadLibrary`, then `SDL_Vulkan_GetVkGetInstanceProcAddr` into
  `rl_vk_config.gipa`), so the surface and the forwarding layer use the same
  loader, the bundled MoltenVK on macOS. Window close asks the guest to exit
  through `am` (`rl_am_request_exit`; a second close quits at once), and
  focus changes go to `rl_am_set_focus`. Keyboard and gamepads reach `hid`
  through `rl_input_publish`. SDL3 handles macOS and Windows the same way; only
  Linux (Wayland and X11) has been run so far.

## Swapchains

A Switch title never sees its window change, so the guest's swapchain is not
the host's. `vkCreateSwapchainKHR` (`vk_swap.c`) makes guest images of the
guest's extent and format (with `TRANSFER_SRC` added) and a host swapchain on
the host surface. `vkGetPhysicalDeviceSurfaceCapabilities*KHR` report the
layer's size as `currentExtent`, two to eight images and identity transform.
Acquire returns a guest image and acquires a host image with the guest's
semaphore and fence; present submits a blit of the guest image into that host
image (letterboxed, linear filter when the format allows) after the guest's
wait semaphores, then presents the host image. The host swapchain is recreated
when the provider's size generation changes (`rl_vk_surface_provider.extent`)
or the host reports out of date or suboptimal, while no guest image is held;
the guest never sees `VK_ERROR_OUT_OF_DATE_KHR` from a resize. Guest swapchain
handles are ids from the layer (`0x57` in the top byte). Commands on
swapchains the layer does not implement (`vkGetSwapchainStatusKHR`, shared,
counter and timing queries, `vkWaitForPresentKHR`) look up as NULL.

## Loading

`vk_window.o` registers `rl_vk_switch_hook()` from a constructor (`rl_switch_register_auto_hook`) and `rl_switch_main` adds it by itself; a program calls nothing. A build must link the object (`librelift_switch_vk.a` only with `-Wl,--whole-archive`, `-force_load` or `/WHOLEARCHIVE`, since nothing else references it).
Before it starts anything, it reads the dynamic section of every module (MOD0)
and enables forwarding when a dynamic symbol is `vkGetInstanceProcAddr` or
`vkGetDeviceProcAddr`, imported (the game's main module) or defined (the
`vulkan` module, which is then replaced). The hook declares this with its
`wants` function; the rule lives here, the loader only asks. A program with
none of those symbols starts no hook: no thunk page, no mapped-memory window,
no window backend (it runs on the calling thread). A program may still add the
hook itself with `rl_switch_add_hook`; it is never added twice. When the process
is up, the hook claims a page after the modules for the thunk table and a map
window above the layout (both marked
inaccessible with `rl_hos_reserve*`, so the guest's own allocations never land
there), starts the layer and registers the thunk unit; without host Vulkan it
does nothing and the guest's own Vulkan runs. While loading, the loader reads
each module's dynamic section through MOD0 and asks the hook about symbols:

- An undefined symbol that names a forwarded command (a module's import of
  `vkGetInstanceProcAddr`) has its `GLOB_DAT`, `JUMP_SLOT` and `ABS64`
  relocations written with the thunk's guest address.
- A function a module defines with such a name (the `vulkan` module's
  exports) gets an `RL_FN_OVERRIDE` entry at its address, so a call that
  reaches the module's address anyway (a guest dynamic linker that binds the
  import to the module later) runs the thunk.

### Where the game's calls are intercepted

At the Vulkan API, at the importing module's boundary, not below the guest's
Vulkan driver. A title's main module imports `vkGetInstanceProcAddr` and
nothing else from the SDK's `vulkan` module; that module is the Vulkan loader
plus Nintendo's driver, which reaches the GPU through the ICD and `nvdrv`
(`/dev/nvhost-*` ioctls and GPU command buffers). Forwarding at the import
skips the guest loader and driver entirely:

- The API boundary is documented and stable (`vk.xml`), so the thunks are
  generated and every command the guest can look up is covered; the lower
  layer is a private ioctl and command-buffer interface.
- Below the driver the work is GPU command streams for a Maxwell GPU and
  Maxwell shader binaries, which is the separate Maxwell emulation path, not
  forwarding. Forwarding instead keeps the host driver's speed and its shader
  compiler (with SPIR-V the guest hands in).
- The guest driver would also have to be lifted and run (large, hand-tuned,
  with its own threads), only to produce command streams the host then has to
  decode again.
- The `vulkan` module's exports are replaced as well as resolved, so the
  boundary holds even when a module calls them directly.

## Tests

`tests/test_vk_auto.c` runs three synthetic modules through `rl_switch_main` with no
hook call: one importing `vkGetInstanceProcAddr` (thunks reach its GOT), one
defining `vkGetDeviceProcAddr` (thunk page reserved) and one with no Vulkan
symbol (nothing reserved, GOT untouched).

`tests/test_vk.c` drives the thunks the way lifted code does: guest structs in a
synthetic guest memory image, arguments in the context, thunk addresses from
`vkGetInstanceProcAddr` and calls through the dispatch table, against the host's
real driver. It covers instance and physical devices with pNext chains, a GPU
clear of an image copied into a mapped buffer and read back through guest
memory in each map mode, guest-written mapped memory copied by the GPU, faults on
unmapped ranges, a debug messenger calling guest code, `VK_NN_vi_surface` to a
headless swapchain with a present and a host resize that recreates the host
swapchain behind the guest's, and bad handles and unknown structs. With no
host Vulkan the tests are skipped unless `RELIFT_VK_REQUIRE` is set; CI sets it
on Linux, where it installs Mesa's lavapipe. `RELIFT_VK_DEVICE` picks the
device they run on.

`demo/vk_present.c` is a synthetic program that goes the whole way: a
hand-written lifted module whose only import is `vkGetInstanceProcAddr`, loaded
by `rl_switch_main` with the hook, which creates a `vi` stray layer and sets
its size over IPC, then an instance, a `VK_NN_vi_surface` surface, a device and
a swapchain, clears and presents `RELIFT_DEMO_FRAMES` frames (60 by default)
and reads the last one back through mapped memory. `make test` runs it
headless (`RELIFT_VIDEO=headless`); `make VIDEO=sdl3 BUILD=build/video vk-present`
shows it in a window.

## Regenerating

```sh
curl -fsSLo vk.xml "$(sed -n 's/^url = "\(.*\)"$/\1/p' guests/switch/vkgen/vkgen.toml)"
cargo run -p relift-switch-vkgen -- --xml vk.xml --config guests/switch/vkgen/vkgen.toml --out runtime/guests/switch/vk/gen
```

The generator refuses a `vk.xml` whose sha256 differs from the pin. Changing
the pin, the configuration or the generator means regenerating and committing
`gen/` in the same change.

## Not done yet

- `relift build` does not compile this folder or `runtime/host` yet.
- Commands that take file descriptors or platform handles (`*_fd`,
  `VK_KHR_display`), which a guest cannot use on the host: their lookups return
  NULL.
- Image-backed host-visible memory on the import path (images are not created
  with the external-memory flag).
- Guest callbacks from driver threads.
