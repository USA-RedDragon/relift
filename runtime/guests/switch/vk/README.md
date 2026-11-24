# Vulkan forwarding (`runtime/guests/switch/vk`)

A Switch game that renders with Vulkan calls `vkGetInstanceProcAddr` from the
`vulkan` module and nothing else in it (see 001-devex section 7). With
forwarding, that module is never lifted: the import resolves to a thunk in this
layer, every entry point the guest looks up is a thunk, and each thunk calls the
host's Vulkan driver.

## Placement

| Part | Where | Why |
|---|---|---|
| Generator | `gpus/apigen` (`relift-gpu-apigen`, binary `relift-vkgen`) | A guest-neutral GPU-API generator; today it emits the Switch guest's ABI (AArch64 LP64, guest addresses). No crate depends on it and it depends only on `relift-cache`. |
| Configuration | `gpus/apigen/vkgen.toml` | The registry pin (tag, URL, sha256), core 1.0 to 1.3, the extension list, the hand-written commands and structs, the dropped types and the shadow-memory hooks. A game repository passes a second file with its extra extensions; lists concatenate. |
| Generated C | `runtime/guests/switch/vk/gen/` (committed, `linguist-generated`) | Types and layout asserts (`vkg_types.h`), the command table (`vkg.h`, `vkg_thunks.c`), the converters (`vkg_convert.c`). CI regenerates them from the pinned `vk.xml` and fails on any difference. |
| Hand-written C | `runtime/guests/switch/vk/*.c` | Handle table, arena, host loading, instance and device creation, memory mapping, callbacks, surfaces. |
| Host library loading | `runtime/core/hostlib.c` | `dlopen` or `LoadLibrary` and the loader names per host (`libvulkan.so.1`, `libvulkan.1.dylib` or `libMoltenVK.dylib`, `vulkan-1.dll`), overridable with `RELIFT_VULKAN_LIBRARY`. Apple builds link MoltenVK instead (`RL_VULKAN_LINKED`) and load nothing. The guest layer never names a host library. |

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
(`rl_vk_surface_provider.presents`; the headless provider has none, every
device takes it).

## Device properties

The guest sees what the console's NVIDIA driver reports, on every host
(`vk_props.c`, values in `vk_switch.c`, read by running the console's driver
in a lifted program with forwarding off): `vkGetPhysicalDeviceProperties` and
the property structs it fills in a `vkGetPhysicalDeviceProperties2` chain
(name "NVIDIA Tegra/Nintendo Switch (nvgpu)", Vulkan 1.1.92, NVIDIA's vendor
and driver ids), the limits, the features, the memory types and heap (one
2000 MiB device-local heap; two device-local types, one host-visible coherent,
one host-visible cached), the format features, the extensions with the
console's spec versions and `vkEnumerateInstanceVersion`. A struct the
console's driver does not know (`VkPhysicalDeviceVulkan12Properties`, for one)
comes back as the guest passed it. Three things are the host's: the pipeline
cache UUID, the device and driver UUIDs, and `timestampPeriod`, since they
describe the host's own caches, memory and timestamps.

Where the host can do more than the Switch, the guest sees the Switch's value
and takes the paths it takes on the console. Where it can do less:

- Limits a call checks: `maxBoundDescriptorSets`, `maxPushConstantsSize`,
  image dimensions and array layers, framebuffer size. The guest sees the
  Switch's value; a `vkCreatePipelineLayout`, `vkCreateImage` or
  `vkCreateFramebuffer` that goes past the host's limit fails, with a log line
  naming the limit, the host and both values.
- Descriptor counts per stage and per set: the guest sees the Switch's value.
  A `vkCreateDescriptorSetLayout` or `vkCreatePipelineLayout` that goes past
  the host's limit is logged once per limit, naming the host and both values,
  and forwarded, since drivers such as MoltenVK report lower counts than they
  accept. `RELIFT_STRICT` makes the call fail instead.
- Any other limit (compute workgroup size on NVIDIA, sample counts and compute
  shared memory on lavapipe, line width range on both): the guest sees the
  host's value, with a warning naming it. `RELIFT_STRICT` makes
  `vkCreateDevice` fail instead.
- Features, extensions and format features are the Switch's that the host also
  has, each missing one logged once, so a game that checks one takes its
  fallback. Enabling a feature or extension the host lacks fails
  `vkCreateDevice` with a log line naming it.

Each Switch memory type stands for one host type: the host-access bits it
needs, then a type the device's buffers and images may use (a probe buffer
and image made right after `vkCreateDevice` tell which; the Adreno 740 driver
has host-visible types no buffer or image may use), then a heap at least as
large as the Switch's, then device-local memory for the device-only types and
system memory for the mapped ones (they are mapped into guest memory, which
placed and imported memory needs). `memoryTypeBits` in memory requirements
are rewritten to Switch type indices and `vkAllocateMemory` takes them back to
the host's; requirements that keep no Switch type are logged as an error.
A command's failing result is logged once per command and result, with the
guest address it returns to.

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

## Descriptor pools

The console driver does not hold a descriptor pool to its per-type counts,
and games size pools for it (one allocates 16,384 sets of a layout from a pool
created for 1,024 descriptors of that type). Drivers that enforce the counts
(MoltenVK) fail those allocations with `VK_ERROR_OUT_OF_POOL_MEMORY`. When a
host pool runs out, `vk_desc.c` creates another host pool with the guest's
parameters behind the same guest pool, doubling the counts if one copy cannot
hold the allocation, and allocates from it. Freeing a set goes to the host
pool it came from; resetting or destroying the guest pool applies to all of
them.

Set layouts reach the host with their bindings sorted by binding number, and
the binding flags of `VkDescriptorSetLayoutBindingFlagsCreateInfo` permuted to
match. Any order is valid, but Mesa's Turnip reads the immutable samplers of a
binding given out of order past their end and crashes.

## Secondary command buffers

The console driver runs a secondary command buffer handed straight to
`vkQueueSubmit`, and a game does that during loading; MoltenVK refuses it
(`VK_NOT_READY`) and drops the work. `vk_submit.c` hands the host a primary
of its own instead that runs the secondary with `vkCmdExecuteCommands`,
recorded again whenever the guest has begun the secondary again since.
Secondaries that continue a render pass are passed through with a warning.
A secondary the guest begins without inheritance info, which the console
driver accepts and Vulkan does not, is used as a primary: the game records
whole render passes in it, which Vulkan allows only in a primary, and the
Adreno 830 driver faults in `vkCmdExecuteCommands` running such a secondary
through a wrapper. Such a begin records into a primary twin of the buffer
(allocated from the same pool on first use), which a submit takes as it is;
a later begin with inheritance info goes back to the secondary, and a
`vkCmdExecuteCommands` of a buffer recorded as a primary is logged as an
error.

## Formats

### 24-bit depth

The console has `VK_FORMAT_D24_UNORM_S8_UINT` and
`VK_FORMAT_X8_D24_UNORM_PACK32`, and games rely on them: one views an image as
D24S8 after the queries told it to use another depth format, and a game built
on The Forge stops when D24S8 reports no sampling. AMD GPUs (RADV, AMDVLK) and
Apple GPUs have no 24-bit depth. On such a host (`vk_format.c`) the queries
answer for `D32_SFLOAT_S8_UINT` and `D32_SFLOAT`, and images, image views,
render pass attachments, image format lists, pipeline and inheritance
rendering formats reach the host with those formats. Every 24-bit depth value
is exact in 32-bit float, so depth tests and sampled depth match. A constant
depth bias is in units of 2^-24 on D24 but of the primitive's float ulp on
D32; on a substituted attachment the layer gives it as an absolute offset of
constant x 2^-24 through `VK_EXT_depth_bias_control` (float representation,
exact), in pipelines with static bias and for `vkCmdSetDepthBias`, whose
values each command buffer keeps and gives again at each render pass,
subpass and dynamic rendering begin. Depth-aspect copies from a buffer go
through the decode path below (24-bit words to float, at submit); stencil
copies are unchanged. Depth-aspect copies to a buffer give D32 float texels,
not packed 24-bit ones, and are logged once.

### Storage image formats

A storage image or texel buffer access on the console takes its format from
the view bound (Maxwell's formatted surface loads and stores carry none), and
games declare one format in the shader and bind a view of another (one game's
SMAA edge pass declares rg32f on an `R8G8B8A8_UNORM` view). In Vulkan that is
undefined: NVIDIA and llvmpipe write all four components in the view's
format, RADV only the shader format's two. With the formatless storage
features on the host, `vk_spirv.c` rewrites each storage `OpTypeImage` format
to Unknown and adds the two formatless capabilities, so every host uses the
view's format. Image types that atomics use keep their format, and so do
image types written with fewer than four components (DXC writes a `float2`
as two), since a formatless write must cover every component of the view.

Open question for a hardware test: what the console stores in B and A when a
shader writes two components through a two-component format to an RGBA8
view (zero and one, zero and zero, or the old texel). Today RADV writes zero
and zero, NVIDIA its own choice; that game's SMAA reads only R and G.

### Compressed formats the host cannot sample

The console samples BC1 to BC7 (and R4G4); Apple GPUs through MoltenVK, and
some Adreno and Mali drivers, cannot. A game that checks the format features
then stops. The guest sees the Switch's features for those formats anyway,
as far as the host has them for the format that stands in for each
(`vk_decode.c`): BC1, BC2, BC3 and BC7 become `R8G8B8A8_UNORM` or `_SRGB`, BC4
`R8_UNORM` or `_SNORM`, BC5 `R8G8_UNORM` or `_SNORM`, BC6H
`R16G16B16A16_SFLOAT`, R4G4 `R8G8_UNORM`. Image format queries answer for the
stand-in, with one sample; `textureCompressionBC` stays the Switch's and is
not enabled on the host.

`vk_image.c` creates an optimal-tiling image of such a format in its
stand-in (linear images keep their format) and views of it in the stand-in
of the view's format. The guest's memory requirements are those of the
compressed texels; the host image gets its own allocation at bind time, freed
with the image, so the guest's allocation goes unused. A copy from a buffer to
such an image (`vkCmdCopyBufferToImage` and its `2` form) is recorded as a
copy from a host-visible staging buffer the layer owns, and at each
`vkQueueSubmit` or `vkQueueSubmit2` of the command buffer (or of a primary
that executes it) the region is decoded with bcdec (`bcdec.c`, MIT, see
`NOTICE`) from the guest buffer's bytes into the staging buffer: through the
guest's mapping of its memory, or a host mapping of host-visible memory the
guest has not mapped. A buffer in device-only memory cannot be read and its
region is left undecoded, with a log line. Image-to-image copies between two
such images copy the stand-in texels (the extent clipped to the mip level);
copies from one to a buffer would need an encoder and are dropped with a log
line, as are copies between such an image and one of another format. Clears
and blits to compressed images are not allowed by Vulkan.

The `vk.decode_bc` setting (`RELIFT_VK_DECODE_BC`) chooses: `auto` decodes the
formats the host cannot sample (or every BC format on a host without
`textureCompressionBC`), `on` every BC format (to run the path on any GPU),
`off` none. With `stats.every` set, a `stats` line every that many presents
gives the bytes decoded and the CPU time spent, and the host bytes of the
stand-in images against the guest's.

## Attachment load and store ops

The console's driver leaves an attachment's memory alone for
`VK_ATTACHMENT_LOAD_OP_DONT_CARE` and `VK_ATTACHMENT_STORE_OP_DONT_CARE`, and
games draw over contents they told it not to keep. MoltenVK on Apple GPUs
passes DONT_CARE to Metal, which starts each tile of the pass with whatever the
tile memory held last, so the picture comes out as a mosaic of tiles from
earlier passes. `vk_pass.c` passes those ops to the host as `LOAD` and `STORE`
in `vkCreateRenderPass`, `vkCreateRenderPass2` and `vkCmdBeginRendering`
(color, depth and stencil). The `vk.keep_attachments` setting
(`RELIFT_VK_KEEP_ATTACHMENTS`) chooses: `auto` does it for the hosts measured
to need it (Apple, PCI vendor 0x106B), `on` and `off` force it for any device.
The test `vk_dont_care_attachments_keep_contents` measures a new GPU: it logs
how many pixels a DONT_CARE pass kept with the guest's ops untouched, and
checks that `on` and `auto` keep them all.

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

## Layers

The console's loader has one layer built in, `VK_LAYER_NN_vi_swapchain`
("Nintendo VI Swapchain Layer"), and no layer manifests, so it offers no
other. The forwarding layer answers for it with the values read from the
SDK's `vulkan.nso`: `vkEnumerateInstanceLayerProperties` lists it alone, its
instance extension list is `VK_KHR_surface` and its device extension list is
empty, and `vkEnumerateDeviceLayerProperties` lists it when the instance
enabled it. `vkCreateInstance` takes it out of the list it gives the host
(its swapchain is the forwarded one) and fails any other layer with
`VK_ERROR_LAYER_NOT_PRESENT`; `vkCreateDevice` gives the host no layers.
Host layers are never visible to the guest.

## `VK_NN_vi_surface`

The host has no `VK_NN_vi_surface`. The layer advertises it when a surface
provider (`rl_vk_config.surfaces`) is configured and the host has the
provider's instance extensions. When the guest enables it, `vkCreateInstance`
enables the provider's host extensions instead, and `vkCreateViSurfaceNN`
calls the provider with the host instance and the guest address of the
`nn::vi::NativeWindowHandle`. Everything after that is plain `VK_KHR_surface`
and `VK_KHR_swapchain` forwarding, except on an `offscreen` provider.

An `offscreen` provider's surfaces have no host surface. `vkCreateViSurfaceNN`
returns a handle the layer makes (the provider's `create` is not called), and
the layer answers every call on it without the host: surface support (every
queue family), capabilities, formats (also `vkGetPhysicalDeviceSurfaceFormats2KHR`),
present modes, present rectangles, device group present modes and
`vkDestroySurfaceKHR`. A swapchain on it has no host swapchain: each present
blits into two images the layer owns (sized by `extent`), and the layer signals
the guest's acquire semaphore and fence with an empty submit. The host needs
only `VK_KHR_surface`, so it works where the host cannot present at all
(NVIDIA's Linux driver offers no `VK_EXT_headless_surface` without another ICD,
and presents to no headless surface when it has one).

Providers:

- Headless (`rl_vk_headless_surfaces`): tests and offscreen runs; `offscreen`,
  host extensions `VK_KHR_surface` only.
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
  loader; on macOS SDL finds the linked MoltenVK. Window close asks the guest to exit
  through `am` (`rl_am_request_exit`; a second close quits at once), and
  focus changes go to `rl_am_set_focus`. Keyboard and gamepads reach `hid`
  through `rl_input_publish`. SDL3 handles macOS and Windows the same way;
  Linux (Wayland and X11) and macOS (Apple M2, MoltenVK) have been run.

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
the guest never sees `VK_ERROR_OUT_OF_DATE_KHR` from a resize. A lost host
surface (`VK_ERROR_SURFACE_LOST_KHR`: Android destroys the window's surface
when the app goes to the background) drops the host swapchain; presents
until a new one exists drop their frame, and acquire waits, asking the
provider (`rl_vk_surface_provider.recreate`) at most every 100 ms for a new
host surface on the same window, then builds a host swapchain on it. The
guest never sees the loss. While the on-screen controller pauses the game
inside its present, the present shows the paused frame again with the
overlay on top; a suboptimal result there keeps the host swapchain unless the
surface needs a new one, as in a present (on a display turned from its
native orientation every present is suboptimal, and the sheet froze after its
first frame). Guest swapchain
handles are ids from the layer (`0x57` in the top byte). Commands on
swapchains the layer does not implement (`vkGetSwapchainStatusKHR`, shared,
counter and timing queries, `vkWaitForPresentKHR`) look up as NULL.

The console's own swapchain layer (`NvSwapchain` in the `vulkan` module)
offers only `VK_PRESENT_MODE_FIFO_KHR` and sets swap interval 1 on the
window, so a game shows at most one frame per 60 Hz vsync and waits in
dequeue when it renders faster. The layer does the same:
`vkGetPhysicalDeviceSurfacePresentModesKHR` reports FIFO alone, each present
is shown at the next vsync of the vi timeline (`vi_vsync_after`, the one the
display's vsync event fires on) after the frame before it, and acquire hands
an image back only once the frame presented after it is shown. A game that
picks its mode from the list (The Forge prefers immediate when vsync is off)
gets FIFO, as on the console.

`vkGetPhysicalDeviceSurfaceFormatsKHR` reports the console layer's fixed list
too, `R8G8B8A8_SRGB` and `R8G8B8A8_UNORM` in sRGB nonlinear: guest images
are the layer's own and the present blit converts to whatever the host
surface takes. Forwarding the host's list broke start-up: a Wayland surface
on NVIDIA answered 28 formats, then 14, then more than 14 between a game's
count and list calls, and The Forge asserts on that `VK_INCOMPLETE`.

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
swapchain behind the guest's, the same offscreen on a fake host without
`VK_EXT_headless_surface` (no surface call reaches the host), bad handles and unknown structs, and the Switch's
properties on a fake host (the real driver with the Switch's limits, then made
smaller or larger, or with a feature taken away): which values the guest
sees, which calls fail and when `vkCreateDevice` does. The decoded formats
have decoder vectors (BC1, BC3, BC4, BC5, BC7 and R4G4 blocks against known
texels), a BC1 and a BC7 upload with `vk.decode_bc` on whose bytes are written
after recording and read back from the host images (also after an image copy),
the format and image queries with `vk.decode_bc` off and on, and a fake host
with no BC features that still shows the Switch's. With no
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
curl -fsSLo vk.xml "$(sed -n 's/^url = "\(.*\)"$/\1/p' gpus/apigen/vkgen.toml)"
cargo run -p relift-gpu-apigen -- --xml vk.xml --config gpus/apigen/vkgen.toml --out runtime/guests/switch/vk/gen
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
