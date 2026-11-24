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
