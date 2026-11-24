# msaa_probe

Measures where a Vulkan device rasterizes each sample of 2, 4, 8 and 16
sample render targets (1/16 pixel). The output is one JSON object, the
`ReliftSamplePositions` member of a device's `device.json`. `measuredStandard`
says whether every measured position is Vulkan's standard one, which is
also NVN's default (see `nvn_vkdev.standard_samples`).

Build on Linux (from `runtime/`):

    clang -std=c11 -O2 -Iguests/switch/vk/gen guests/switch/devtests/msaa_probe/msaa_probe.c -ldl -o msaa_probe

Build for Android (NDK r27):

    $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android33-clang -std=c11 -O2 \
        -Iguests/switch/vk/gen guests/switch/devtests/msaa_probe/msaa_probe.c -ldl -o msaa_probe

Run `msaa_probe` for the system driver, or `msaa_probe <driver.so>` to load a
driver package directly (Turnip). On a shared device, hold the device's lock
for the whole session.

## msaa_cells

Prints where an NVIDIA GPU stores each sample of a 2, 4, 8 or 16 sample
target: each gw by gh block of the output is one pixel's grid, with each
cell holding its sample's index + 1. Pass `s8` for a stencil target (the RTX
3090's only 16-sample target). On the RTX 3090 the 2, 4 and 8 sample grids
are Mesa NVK's cells. Its 16-sample grid is the one in
`nvn_msaa.c` grid_of.

    clang -std=c11 -O2 -Iguests/switch/vk/gen guests/switch/devtests/msaa_probe/msaa_cells.c -ldl -o msaa_cells
    ./msaa_cells 16 s8
