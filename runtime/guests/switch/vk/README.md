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
   Dedicated and exported allocations fall back to shadow.
3. Shadow: the guest gets its own pages. Map copies host to guest; every submit
   and present copies guest to host; every wait that completes copies host to
   guest; flush and invalidate copy their ranges. Writes the guest makes between
   a submit and the wait for it are lost, so shadow is the last resort.

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
  offscreen runs. Lavapipe and NVIDIA have it.
- SDL3 (the player, after this lane): the `vi` service owns the host window. A
  layer that opens an `nn::vi` layer records which `SDL_Window` backs which
  native window handle (the handle is the guest address the guest later puts in
  `VkViSurfaceCreateInfoNN.window`). The provider's `instance_extensions` are
  `SDL_Vulkan_GetInstanceExtensions`, and `create` looks the window up and calls
  `SDL_Vulkan_CreateSurface(window, instance, NULL, &surface)`. The player loads
  Vulkan through SDL (`SDL_Vulkan_LoadLibrary`, then
  `SDL_Vulkan_GetVkGetInstanceProcAddr` into `rl_vk_config.gipa`), so the surface
  and the forwarding layer use the same loader, the bundled MoltenVK on macOS.
  Window size and the swapchain extent follow the host window. The guest's
  1280x720 or 1920x1080 is the requested extent; when the host surface reports a
  fixed `currentExtent`, the guest sees that extent.

## Tests

`tests/test_vk.c` drives the thunks the way lifted code does: guest structs in a
synthetic guest memory image, arguments in the context, thunk addresses from
`vkGetInstanceProcAddr` and calls through the dispatch table, against the host's
real driver. It covers instance and physical devices with pNext chains, a GPU
clear of an image copied into a mapped buffer and read back through guest
memory in each map mode, guest-written mapped memory copied by the GPU, faults on
unmapped ranges, a debug messenger calling guest code, `VK_NN_vi_surface` to a
headless swapchain with a present, and bad handles and unknown structs. With no
host Vulkan the tests are skipped unless `RELIFT_VK_REQUIRE` is set; CI sets it
on Linux, where it installs Mesa's lavapipe. `RELIFT_VK_TEST_DEVICE` limits the
device tests to devices whose name contains it.

## Regenerating

```sh
curl -fsSLo vk.xml "$(sed -n 's/^url = "\(.*\)"$/\1/p' guests/switch/vkgen/vkgen.toml)"
cargo run -p relift-switch-vkgen -- --xml vk.xml --config guests/switch/vkgen/vkgen.toml --out runtime/guests/switch/vk/gen
```

The generator refuses a `vk.xml` whose sha256 differs from the pin. Changing
the pin, the configuration or the generator means regenerating and committing
`gen/` in the same change.

## Not done yet

- The SDL3 provider and the `vi` service side (above).
- Commands that take file descriptors or platform handles (`*_fd`,
  `VK_KHR_display`), which a guest cannot use on the host: their lookups return
  NULL.
- Image-backed host-visible memory on the import path (images are not created
  with the external-memory flag).
- Guest callbacks from driver threads.
