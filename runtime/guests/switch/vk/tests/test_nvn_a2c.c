/**
 * @file test_nvn_a2c.c
 * @brief Alpha-to-coverage on the host device: the samples a draw of each alpha covers in each pixel of an 8 by 8
 * target, at each sample count the device has. With RELIFT_A2C_TABLE set to a path, the test writes the masks there
 * (one line per sample count and alpha: the 64 masks in hex), the measurement the console's hardware behavior is
 * read from on an NVIDIA host. On an NVIDIA host every mask must be the RTX 3090's (nvidia_a2c), the pattern the
 * layer keeps by drawing with the host's own alpha to coverage there.
 */
#include "hos.h"
#include "nvn_priv.h"
#include "nvn_module.h"
#include "nvn_vk.h"
#include "runner.h"
#include "test_nvn_a2c_spv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief A full-screen triangle, fill.vert by `glslc --target-env=vulkan1.3 -O`:
 *
 * #version 450
 * void main() { vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }
 */
static const uint32_t fill_vert_spv[] = {
    0x07230203, 0x00010600, 0x000d000b, 0x0000002c, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000c, 0x0000001d, 0x00040047,
    0x0000000c, 0x0000000b, 0x0000002a, 0x00030047, 0x0000001b, 0x00000002, 0x00050048, 0x0000001b,
    0x00000000, 0x0000000b, 0x00000000, 0x00050048, 0x0000001b, 0x00000001, 0x0000000b, 0x00000001,
    0x00050048, 0x0000001b, 0x00000002, 0x0000000b, 0x00000003, 0x00050048, 0x0000001b, 0x00000003,
    0x0000000b, 0x00000004, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016,
    0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000002, 0x00040015, 0x0000000a,
    0x00000020, 0x00000001, 0x00040020, 0x0000000b, 0x00000001, 0x0000000a, 0x0004003b, 0x0000000b,
    0x0000000c, 0x00000001, 0x0004002b, 0x0000000a, 0x0000000e, 0x00000001, 0x0004002b, 0x0000000a,
    0x00000010, 0x00000002, 0x00040017, 0x00000017, 0x00000006, 0x00000004, 0x00040015, 0x00000018,
    0x00000020, 0x00000000, 0x0004002b, 0x00000018, 0x00000019, 0x00000001, 0x0004001c, 0x0000001a,
    0x00000006, 0x00000019, 0x0006001e, 0x0000001b, 0x00000017, 0x00000006, 0x0000001a, 0x0000001a,
    0x00040020, 0x0000001c, 0x00000003, 0x0000001b, 0x0004003b, 0x0000001c, 0x0000001d, 0x00000003,
    0x0004002b, 0x0000000a, 0x0000001e, 0x00000000, 0x0004002b, 0x00000006, 0x00000020, 0x40000000,
    0x0004002b, 0x00000006, 0x00000022, 0x3f800000, 0x0004002b, 0x00000006, 0x00000025, 0x00000000,
    0x00040020, 0x00000029, 0x00000003, 0x00000017, 0x0005002c, 0x00000007, 0x0000002b, 0x00000022,
    0x00000022, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005,
    0x0004003d, 0x0000000a, 0x0000000d, 0x0000000c, 0x000500c4, 0x0000000a, 0x0000000f, 0x0000000d,
    0x0000000e, 0x000500c7, 0x0000000a, 0x00000011, 0x0000000f, 0x00000010, 0x0004006f, 0x00000006,
    0x00000012, 0x00000011, 0x000500c7, 0x0000000a, 0x00000014, 0x0000000d, 0x00000010, 0x0004006f,
    0x00000006, 0x00000015, 0x00000014, 0x00050050, 0x00000007, 0x00000016, 0x00000012, 0x00000015,
    0x0005008e, 0x00000007, 0x00000021, 0x00000016, 0x00000020, 0x00050083, 0x00000007, 0x00000024,
    0x00000021, 0x0000002b, 0x00050051, 0x00000006, 0x00000026, 0x00000024, 0x00000000, 0x00050051,
    0x00000006, 0x00000027, 0x00000024, 0x00000001, 0x00070050, 0x00000017, 0x00000028, 0x00000026,
    0x00000027, 0x00000025, 0x00000022, 0x00050041, 0x00000029, 0x0000002a, 0x0000001d, 0x0000001e,
    0x0003003e, 0x0000002a, 0x00000028, 0x000100fd, 0x00010038,
};

/**
 * @brief White at the pushed alpha, a2c.frag by `glslc --target-env=vulkan1.3 -O`:
 *
 * #version 450
 * layout(push_constant) uniform P { float alpha; } p;
 * layout(location = 0) out vec4 o;
 * void main() { o = vec4(1.0, 1.0, 1.0, p.alpha); }
 */
static const uint32_t a2c_frag_spv[] = {
    0x07230203, 0x00010600, 0x000d000b, 0x00000014, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x0000000d, 0x00030010,
    0x00000004, 0x00000007, 0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00030047, 0x0000000b,
    0x00000002, 0x00050048, 0x0000000b, 0x00000000, 0x00000023, 0x00000000, 0x00020013, 0x00000002,
    0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007,
    0x00000006, 0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008,
    0x00000009, 0x00000003, 0x0004002b, 0x00000006, 0x0000000a, 0x3f800000, 0x0003001e, 0x0000000b,
    0x00000006, 0x00040020, 0x0000000c, 0x00000009, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d,
    0x00000009, 0x00040015, 0x0000000e, 0x00000020, 0x00000001, 0x0004002b, 0x0000000e, 0x0000000f,
    0x00000000, 0x00040020, 0x00000010, 0x00000009, 0x00000006, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x00050041, 0x00000010, 0x00000011, 0x0000000d,
    0x0000000f, 0x0004003d, 0x00000006, 0x00000012, 0x00000011, 0x00070050, 0x00000007, 0x00000013,
    0x0000000a, 0x0000000a, 0x0000000a, 0x00000012, 0x0003003e, 0x00000009, 0x00000013, 0x000100fd,
    0x00010038,
};

/**
 * @brief Each pixel's mask of samples drawn, read.comp by `glslc --target-env=vulkan1.3 -O`:
 *
 * #version 450
 * #extension GL_EXT_samplerless_texture_functions : require
 * layout(local_size_x = 1) in;
 * layout(binding = 0) uniform texture2DMS ms;
 * layout(binding = 1) buffer Out { uint o[]; };
 * layout(push_constant) uniform P { uint width, samples; } p;
 * void main()
 * {
 *     uvec2 q = gl_WorkGroupID.xy;
 *     uint m = 0u;
 *     for (uint s = 0u; s < p.samples; s++)
 *         m |= (texelFetch(ms, ivec2(q), int(s)).r > 0.5 ? 1u : 0u) << s;
 *     o[q.y * p.width + q.x] = m;
 * }
 */
static const uint32_t read_comp_spv[] = {
    0x07230203, 0x00010600, 0x000d000b, 0x0000004f, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0009000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000c, 0x0000001b, 0x00000026,
    0x0000003d, 0x00060010, 0x00000004, 0x00000011, 0x00000001, 0x00000001, 0x00000001, 0x00040047,
    0x0000000c, 0x0000000b, 0x0000001a, 0x00030047, 0x00000019, 0x00000002, 0x00050048, 0x00000019,
    0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x00000019, 0x00000001, 0x00000023, 0x00000004,
    0x00040047, 0x00000026, 0x00000021, 0x00000000, 0x00040047, 0x00000026, 0x00000022, 0x00000000,
    0x00040047, 0x0000003a, 0x00000006, 0x00000004, 0x00030047, 0x0000003b, 0x00000002, 0x00050048,
    0x0000003b, 0x00000000, 0x00000023, 0x00000000, 0x00040047, 0x0000003d, 0x00000021, 0x00000001,
    0x00040047, 0x0000003d, 0x00000022, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003,
    0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x00040017, 0x00000007, 0x00000006,
    0x00000002, 0x00040017, 0x0000000a, 0x00000006, 0x00000003, 0x00040020, 0x0000000b, 0x00000001,
    0x0000000a, 0x0004003b, 0x0000000b, 0x0000000c, 0x00000001, 0x0004002b, 0x00000006, 0x00000011,
    0x00000000, 0x0004001e, 0x00000019, 0x00000006, 0x00000006, 0x00040020, 0x0000001a, 0x00000009,
    0x00000019, 0x0004003b, 0x0000001a, 0x0000001b, 0x00000009, 0x00040015, 0x0000001c, 0x00000020,
    0x00000001, 0x0004002b, 0x0000001c, 0x0000001d, 0x00000001, 0x00040020, 0x0000001e, 0x00000009,
    0x00000006, 0x00020014, 0x00000021, 0x00030016, 0x00000023, 0x00000020, 0x00090019, 0x00000024,
    0x00000023, 0x00000001, 0x00000000, 0x00000000, 0x00000001, 0x00000001, 0x00000000, 0x00040020,
    0x00000025, 0x00000000, 0x00000024, 0x0004003b, 0x00000025, 0x00000026, 0x00000000, 0x00040017,
    0x00000029, 0x0000001c, 0x00000002, 0x00040017, 0x0000002d, 0x00000023, 0x00000004, 0x0004002b,
    0x00000023, 0x00000030, 0x3f000000, 0x0004002b, 0x00000006, 0x00000032, 0x00000001, 0x0003001d,
    0x0000003a, 0x00000006, 0x0003001e, 0x0000003b, 0x0000003a, 0x00040020, 0x0000003c, 0x0000000c,
    0x0000003b, 0x0004003b, 0x0000003c, 0x0000003d, 0x0000000c, 0x0004002b, 0x0000001c, 0x0000003e,
    0x00000000, 0x00040020, 0x00000048, 0x0000000c, 0x00000006, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x0000000a, 0x0000000d, 0x0000000c,
    0x0007004f, 0x00000007, 0x0000000e, 0x0000000d, 0x0000000d, 0x00000000, 0x00000001, 0x000200f9,
    0x00000013, 0x000200f8, 0x00000013, 0x000700f5, 0x00000006, 0x0000004e, 0x00000011, 0x00000005,
    0x00000037, 0x00000014, 0x000700f5, 0x00000006, 0x0000004d, 0x00000011, 0x00000005, 0x00000039,
    0x00000014, 0x00050041, 0x0000001e, 0x0000001f, 0x0000001b, 0x0000001d, 0x0004003d, 0x00000006,
    0x00000020, 0x0000001f, 0x000500b0, 0x00000021, 0x00000022, 0x0000004d, 0x00000020, 0x000400f6,
    0x00000015, 0x00000014, 0x00000000, 0x000400fa, 0x00000022, 0x00000014, 0x00000015, 0x000200f8,
    0x00000014, 0x0004003d, 0x00000024, 0x00000027, 0x00000026, 0x0004007c, 0x00000029, 0x0000002a,
    0x0000000e, 0x0004007c, 0x0000001c, 0x0000002c, 0x0000004d, 0x0007005f, 0x0000002d, 0x0000002e,
    0x00000027, 0x0000002a, 0x00000040, 0x0000002c, 0x00050051, 0x00000023, 0x0000002f, 0x0000002e,
    0x00000000, 0x000500ba, 0x00000021, 0x00000031, 0x0000002f, 0x00000030, 0x000600a9, 0x00000006,
    0x00000033, 0x00000031, 0x00000032, 0x00000011, 0x000500c4, 0x00000006, 0x00000035, 0x00000033,
    0x0000004d, 0x000500c5, 0x00000006, 0x00000037, 0x0000004e, 0x00000035, 0x00050080, 0x00000006,
    0x00000039, 0x0000004d, 0x0000001d, 0x000200f9, 0x00000013, 0x000200f8, 0x00000015, 0x00050051,
    0x00000006, 0x00000040, 0x0000000d, 0x00000001, 0x00050041, 0x0000001e, 0x00000041, 0x0000001b,
    0x0000003e, 0x0004003d, 0x00000006, 0x00000042, 0x00000041, 0x00050084, 0x00000006, 0x00000043,
    0x00000040, 0x00000042, 0x00050051, 0x00000006, 0x00000045, 0x0000000d, 0x00000000, 0x00050080,
    0x00000006, 0x00000046, 0x00000043, 0x00000045, 0x00060041, 0x00000048, 0x00000049, 0x0000003d,
    0x0000003e, 0x00000046, 0x0003003e, 0x00000049, 0x0000004e, 0x000100fd, 0x00010038,
};

#define SIDE 8u
#define STEPS 256u

/**
 * @brief The RTX 3090's alpha to coverage (driver 615.78.08, measured by this test): from alpha step a of 255 on, until the
 * next row of the same sample count, pixel (x, y) is covered by mask[(y & 1) * 2 + (x & 1)]. 4n levels at (2i + 1) /
 * (8n) spread over a 2x2 footprint, each pixel filling its samples in an order of its own: the console GPU's
 * dithered pattern (PIXELS_2X2), which an NVIDIA host must keep.
 */
static const struct {
    uint32_t samples, from;
    uint8_t mask[4];
} nvidia_a2c[] = {
    { 2, 0, { 0x00, 0x00, 0x00, 0x00 } },
    { 2, 16, { 0x01, 0x00, 0x00, 0x00 } },
    { 2, 48, { 0x01, 0x00, 0x00, 0x01 } },
    { 2, 80, { 0x01, 0x02, 0x00, 0x01 } },
    { 2, 112, { 0x01, 0x02, 0x02, 0x01 } },
    { 2, 144, { 0x03, 0x02, 0x02, 0x01 } },
    { 2, 176, { 0x03, 0x02, 0x02, 0x03 } },
    { 2, 208, { 0x03, 0x03, 0x02, 0x03 } },
    { 2, 240, { 0x03, 0x03, 0x03, 0x03 } },
    { 4, 0, { 0x00, 0x00, 0x00, 0x00 } },
    { 4, 8, { 0x08, 0x00, 0x00, 0x00 } },
    { 4, 24, { 0x08, 0x00, 0x00, 0x08 } },
    { 4, 40, { 0x08, 0x08, 0x00, 0x08 } },
    { 4, 56, { 0x08, 0x08, 0x08, 0x08 } },
    { 4, 72, { 0x09, 0x08, 0x08, 0x08 } },
    { 4, 88, { 0x09, 0x08, 0x08, 0x09 } },
    { 4, 104, { 0x09, 0x09, 0x08, 0x09 } },
    { 4, 120, { 0x09, 0x09, 0x09, 0x09 } },
    { 4, 136, { 0x0b, 0x09, 0x09, 0x09 } },
    { 4, 152, { 0x0b, 0x09, 0x09, 0x0b } },
    { 4, 168, { 0x0b, 0x0b, 0x09, 0x0b } },
    { 4, 184, { 0x0b, 0x0b, 0x0b, 0x0b } },
    { 4, 200, { 0x0f, 0x0b, 0x0b, 0x0b } },
    { 4, 216, { 0x0f, 0x0b, 0x0b, 0x0f } },
    { 4, 232, { 0x0f, 0x0f, 0x0b, 0x0f } },
    { 4, 248, { 0x0f, 0x0f, 0x0f, 0x0f } },
    { 8, 0, { 0x00, 0x00, 0x00, 0x00 } },
    { 8, 4, { 0x40, 0x00, 0x00, 0x00 } },
    { 8, 12, { 0x40, 0x00, 0x00, 0x08 } },
    { 8, 20, { 0x40, 0x80, 0x00, 0x08 } },
    { 8, 28, { 0x40, 0x80, 0x10, 0x08 } },
    { 8, 36, { 0x50, 0x80, 0x10, 0x08 } },
    { 8, 44, { 0x50, 0x80, 0x10, 0x88 } },
    { 8, 52, { 0x50, 0x88, 0x10, 0x88 } },
    { 8, 60, { 0x50, 0x88, 0x50, 0x88 } },
    { 8, 68, { 0xd0, 0x88, 0x50, 0x88 } },
    { 8, 76, { 0xd0, 0x88, 0x50, 0x98 } },
    { 8, 84, { 0xd0, 0xc8, 0x50, 0x98 } },
    { 8, 92, { 0xd0, 0xc8, 0x58, 0x98 } },
    { 8, 100, { 0xd8, 0xc8, 0x58, 0x98 } },
    { 8, 108, { 0xd8, 0xc8, 0x58, 0xd8 } },
    { 8, 116, { 0xd8, 0xd8, 0x58, 0xd8 } },
    { 8, 124, { 0xd8, 0xd8, 0xd8, 0xd8 } },
    { 8, 132, { 0xda, 0xd8, 0xd8, 0xd8 } },
    { 8, 140, { 0xda, 0xd8, 0xd8, 0xd9 } },
    { 8, 148, { 0xda, 0xd9, 0xd8, 0xd9 } },
    { 8, 156, { 0xda, 0xd9, 0xda, 0xd9 } },
    { 8, 164, { 0xde, 0xd9, 0xda, 0xd9 } },
    { 8, 172, { 0xde, 0xd9, 0xda, 0xf9 } },
    { 8, 180, { 0xde, 0xf9, 0xda, 0xf9 } },
    { 8, 188, { 0xde, 0xf9, 0xde, 0xf9 } },
    { 8, 196, { 0xdf, 0xf9, 0xde, 0xf9 } },
    { 8, 204, { 0xdf, 0xf9, 0xde, 0xfb } },
    { 8, 212, { 0xdf, 0xfd, 0xde, 0xfb } },
    { 8, 220, { 0xdf, 0xfd, 0xfe, 0xfb } },
    { 8, 228, { 0xff, 0xfd, 0xfe, 0xfb } },
    { 8, 236, { 0xff, 0xfd, 0xfe, 0xff } },
    { 8, 244, { 0xff, 0xff, 0xfe, 0xff } },
    { 8, 252, { 0xff, 0xff, 0xff, 0xff } },
};

/** @brief The 3090's mask of pixel i of the target at sample count n and alpha step a. */
static uint32_t nvidia_mask(uint32_t n, uint32_t a, uint32_t i)
{
    uint32_t m = 0;
    for (uint32_t k = 0; k < sizeof nvidia_a2c / sizeof nvidia_a2c[0]; k++)
        if (nvidia_a2c[k].samples == n && nvidia_a2c[k].from <= a)
            m = nvidia_a2c[k].mask[(i / SIDE & 1u) * 2 + (i % SIDE & 1u)];
    return m;
}

static VkShaderModule module_of(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code };
    VkShaderModule m = NULL;
    VK(vkCreateShaderModule)(g.dev, &ci, NULL, &m);
    return m;
}

/** @brief The device objects one sample count's measurement uses. */
typedef struct rig {
    VkImage img;
    VkDeviceMemory img_mem;
    VkImageView view;
    VkBuffer buf;
    VkDeviceMemory buf_mem;
    uint32_t *map;
    VkPipelineLayout gpl, cpl;
    VkPipeline gpipe, cpipe;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    /** @brief The translated fragment program computes the mask (A2C_SPEC), not the host's alpha to coverage. */
    bool translated;
} rig;

static bool rig_up(rig *k, VkSampleCountFlagBits s, bool translated)
{
    memset(k, 0, sizeof *k);
    k->translated = translated;
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                              .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { SIDE, SIDE, 1 }, .mipLevels = 1,
                              .arrayLayers = 1, .samples = s, .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT };
    if (VK(vkCreateImage)(g.dev, &ici, NULL, &k->img) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    VK(vkGetImageMemoryRequirements)(g.dev, k->img, &req);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                 .memoryTypeIndex = (uint32_t)nvn_vk_mem_type(req.memoryTypeBits,
                                                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    if (VK(vkAllocateMemory)(g.dev, &mai, NULL, &k->img_mem) != VK_SUCCESS ||
        VK(vkBindImageMemory)(g.dev, k->img, k->img_mem, 0) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = k->img,
                                  .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                  .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    if (VK(vkCreateImageView)(g.dev, &vci, NULL, &k->view) != VK_SUCCESS)
        return false;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = STEPS * SIDE * SIDE * 4 + 256,
                               .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
    if (VK(vkCreateBuffer)(g.dev, &bci, NULL, &k->buf) != VK_SUCCESS)
        return false;
    VK(vkGetBufferMemoryRequirements)(g.dev, k->buf, &req);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)nvn_vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (VK(vkAllocateMemory)(g.dev, &mai, NULL, &k->buf_mem) != VK_SUCCESS ||
        VK(vkBindBufferMemory)(g.dev, k->buf, k->buf_mem, 0) != VK_SUCCESS ||
        VK(vkMapMemory)(g.dev, k->buf_mem, 0, bci.size, 0, (void **)&k->map) != VK_SUCCESS)
        return false;
    VkPushConstantRange gpr = { translated ? VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                                           : VK_SHADER_STAGE_FRAGMENT_BIT,
                                0, translated ? 20 : 4 };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .pushConstantRangeCount = 1, .pPushConstantRanges = &gpr };
    if (VK(vkCreatePipelineLayout)(g.dev, &pli, NULL, &k->gpl) != VK_SUCCESS)
        return false;
    VkShaderModule vs = translated ? module_of(flat_vert_spv, sizeof flat_vert_spv)
                                   : module_of(fill_vert_spv, sizeof fill_vert_spv);
    VkShaderModule fs = translated ? module_of(fragment_a2c_spv, sizeof fragment_a2c_spv)
                                   : module_of(a2c_frag_spv, sizeof a2c_frag_spv);
    uint32_t spec_n = (uint32_t)s;
    VkSpecializationMapEntry me = { A2C_SPEC, 0, 4 };
    VkSpecializationInfo si = { 1, &me, 4, &spec_n };
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = fs, .pName = "main", .pSpecializationInfo = translated ? &si : NULL },
    };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, SIDE, SIDE, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { SIDE, SIDE } };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                              .viewportCount = 1, .pViewports = &vp, .scissorCount = 1,
                                              .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                  .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1 };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                .rasterizationSamples = s, .alphaToCoverageEnable = !translated };
    VkPipelineColorBlendAttachmentState ba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                .attachmentCount = 1, .pAttachments = &ba };
    VkFormat f = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                         .colorAttachmentCount = 1, .pColorAttachmentFormats = &f };
    VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                         .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
                                         .pInputAssemblyState = &ia, .pViewportState = &vps,
                                         .pRasterizationState = &rs, .pMultisampleState = &ms,
                                         .pColorBlendState = &cbs, .layout = k->gpl };
    bool ok = VK(vkCreateGraphicsPipelines)(g.dev, NULL, 1, &gpi, NULL, &k->gpipe) == VK_SUCCESS;
    VK(vkDestroyShaderModule)(g.dev, vs, NULL);
    VK(vkDestroyShaderModule)(g.dev, fs, NULL);
    if (!ok)
        return false;
    VkDescriptorSetLayoutBinding b[2] = { { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
                                          { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = 2, .pBindings = b };
    if (VK(vkCreateDescriptorSetLayout)(g.dev, &li, NULL, &k->dsl) != VK_SUCCESS)
        return false;
    VkPushConstantRange cpr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 };
    VkPipelineLayoutCreateInfo cli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                       .pSetLayouts = &k->dsl, .pushConstantRangeCount = 1,
                                       .pPushConstantRanges = &cpr };
    if (VK(vkCreatePipelineLayout)(g.dev, &cli, NULL, &k->cpl) != VK_SUCCESS)
        return false;
    VkShaderModule cs = module_of(read_comp_spv, sizeof read_comp_spv);
    VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = cs,
                                                   .pName = "main" },
                                        .layout = k->cpl };
    ok = VK(vkCreateComputePipelines)(g.dev, NULL, 1, &cpi, NULL, &k->cpipe) == VK_SUCCESS;
    VK(vkDestroyShaderModule)(g.dev, cs, NULL);
    if (!ok)
        return false;
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
                                       .poolSizeCount = 2, .pPoolSizes = ps };
    if (VK(vkCreateDescriptorPool)(g.dev, &dpi, NULL, &k->pool) != VK_SUCCESS)
        return false;
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = k->pool, .descriptorSetCount = 1, .pSetLayouts = &k->dsl };
    if (VK(vkAllocateDescriptorSets)(g.dev, &dai, &k->set) != VK_SUCCESS)
        return false;
    VkDescriptorImageInfo ii = { NULL, k->view, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorBufferInfo bi = { k->buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = k->set, .dstBinding = 0, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = k->set, .dstBinding = 1, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi },
    };
    VK(vkUpdateDescriptorSets)(g.dev, 2, w, 0, NULL);
    return true;
}

static void rig_down(rig *k)
{
    VK(vkDestroyDescriptorPool)(g.dev, k->pool, NULL);
    VK(vkDestroyPipeline)(g.dev, k->cpipe, NULL);
    VK(vkDestroyPipeline)(g.dev, k->gpipe, NULL);
    VK(vkDestroyPipelineLayout)(g.dev, k->cpl, NULL);
    VK(vkDestroyPipelineLayout)(g.dev, k->gpl, NULL);
    VK(vkDestroyDescriptorSetLayout)(g.dev, k->dsl, NULL);
    VK(vkDestroyImageView)(g.dev, k->view, NULL);
    VK(vkDestroyImage)(g.dev, k->img, NULL);
    VK(vkFreeMemory)(g.dev, k->img_mem, NULL);
    VK(vkDestroyBuffer)(g.dev, k->buf, NULL);
    VK(vkFreeMemory)(g.dev, k->buf_mem, NULL);
}

/** @brief Draws alpha over the cleared target and reads each pixel's mask into k->map. */
static void measure_at(rig *k, uint32_t samples, float alpha)
{
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = g.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    VkCommandBuffer cb;
    CHECK_EQ(VK(vkAllocateCommandBuffers)(g.dev, &cai, &cb), VK_SUCCESS);
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK(vkBeginCommandBuffer)(cb, &cbi);
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
                                .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = k->img,
                                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VK(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                             NULL, 1, &ib);
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = k->view,
                                     .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                     .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { SIDE, SIDE } },
                           .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
    VK(vkCmdBeginRendering)(cb, &ri);
    VK(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, k->gpipe);
    if (k->translated) {
        float push[5] = { 0, 0, 0, 0, alpha };
        VK(vkCmdPushConstants)(cb, k->gpl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push,
                               push);
    } else {
        VK(vkCmdPushConstants)(cb, k->gpl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &alpha);
    }
    VK(vkCmdDraw)(cb, 3, 1, 0, 0);
    VK(vkCmdEndRendering)(cb);
    nvn_vk_barrier(cb);
    VK(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, k->cpipe);
    VK(vkCmdBindDescriptorSets)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, k->cpl, 0, 1, &k->set, 0, NULL);
    uint32_t pc[2] = { SIDE, samples };
    VK(vkCmdPushConstants)(cb, k->cpl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
    VK(vkCmdDispatch)(cb, SIDE, SIDE, 1);
    nvn_vk_barrier(cb);
    VK(vkEndCommandBuffer)(cb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    VK(vkResetFences)(g.dev, 1, &g.fence);
    CHECK_EQ(VK(vkQueueSubmit)(g.queue, 1, &si, g.fence), VK_SUCCESS);
    CHECK_EQ(VK(vkWaitForFences)(g.dev, 1, &g.fence, VK_TRUE, 10000000000ull), VK_SUCCESS);
    VK(vkFreeCommandBuffers)(g.dev, g.pool, 1, &cb);
}

/** @brief measure_at alpha step a / (STEPS - 1). */
static void measure(rig *k, uint32_t samples, uint32_t a) { measure_at(k, samples, (float)a / (float)(STEPS - 1)); }

/** @brief The samples covered over the whole target at alpha. */
static uint32_t covered(rig *k, uint32_t samples, float alpha)
{
    measure_at(k, samples, alpha);
    uint32_t total = 0;
    for (uint32_t i = 0; i < SIDE * SIDE; i++)
        total += (uint32_t)__builtin_popcount(k->map[i] & ((1u << samples) - 1u));
    return total;
}

static float f32_of(uint32_t b)
{
    float f;
    memcpy(&f, &b, sizeof f);
    return f;
}

/**
 * @brief The least alpha (as its bits, a non-negative float not above 1) at which at least want samples of the
 * target are covered, by bisection over the float bits (covered grows with alpha).
 */
static uint32_t threshold(rig *k, uint32_t samples, uint32_t want)
{
    uint32_t lo = 0, hi = 0x3f800000u;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (covered(k, samples, f32_of(mid)) >= want)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

TEST(nvn_alpha_to_coverage_covers_more_samples_as_alpha_grows)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes))
        return;
    const char *path = getenv("RELIFT_A2C_TABLE");
    FILE *out = path ? fopen(path, "w") : NULL;
    if (out)
        fprintf(out, "# %s: samples, alpha step of %u, then the mask of each pixel of an %ux%u target, rows first\n",
                g.name, STEPS - 1, SIDE, SIDE);
    static const VkSampleCountFlagBits counts[4] = { VK_SAMPLE_COUNT_2_BIT, VK_SAMPLE_COUNT_4_BIT,
                                                     VK_SAMPLE_COUNT_8_BIT, VK_SAMPLE_COUNT_16_BIT };
    VkImageFormatProperties ifp;
    CHECK_EQ(VK(vkGetPhysicalDeviceImageFormatProperties)(g.phys, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
                                                          VK_IMAGE_TILING_OPTIMAL,
                                                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                              VK_IMAGE_USAGE_SAMPLED_BIT,
                                                          0, &ifp),
             VK_SUCCESS);
    for (uint32_t c = 0; c < 4; c++) {
        if (!(ifp.sampleCounts & counts[c]))
            continue;
        uint32_t n = (uint32_t)counts[c], full = (1u << n) - 1u;
        static rig k;
        CHECK(rig_up(&k, counts[c], false));
        uint32_t last = 0;
        for (uint32_t a = 0; a < STEPS; a++) {
            measure(&k, n, a);
            uint32_t total = 0;
            for (uint32_t i = 0; i < SIDE * SIDE; i++)
                total += (uint32_t)__builtin_popcount(k.map[i]);
            if (a == 0)
                CHECK_EQ(total, 0u);
            if (a == STEPS - 1)
                CHECK_EQ(total, n * SIDE * SIDE);
            CHECK(total >= last);
            last = total;
            uint32_t off = 0;
            for (uint32_t i = 0; g.nvidia && n <= 8 && i < SIDE * SIDE; i++)
                off += (k.map[i] & full) != nvidia_mask(n, a, i);
            CHECK_EQ(off, 0u);
            if (out)
                fprintf(out, "%u %u\t", n, a);
            for (uint32_t i = 0; out && i < SIDE * SIDE; i++)
                fprintf(out, "%s%x", i ? " " : "", k.map[i] & full);
            if (out)
                fprintf(out, "\n");
        }
        /* Each level covers one more sample in each 2x2 footprint: SIDE * SIDE / 4 more over the target. */
        for (uint32_t lvl = 1; out && lvl <= 4 * n; lvl++)
            fprintf(out, "threshold %u %u\t%08x\n", n, lvl, threshold(&k, n, lvl * SIDE * SIDE / 4));
        static const uint32_t odd[] = { 0x80000000u, 0xbf800000u, 0x3f800001u, 0x40000000u, 0x7f800000u, 0xff800000u,
                                        0x7fc00000u, 0xffc00000u, 0x00000001u };
        for (uint32_t i = 0; out && i < sizeof odd / sizeof odd[0]; i++)
            fprintf(out, "special %u %08x\t%u\n", n, odd[i], covered(&k, n, f32_of(odd[i])));
        rig_down(&k);
    }
    if (out)
        fclose(out);
}

/** @brief The console's thresholds (alpha at or above t / 4096 covers one more sample a footprint), as the 3090's. */
static const uint16_t thresholds[3][32] = {
    { 249, 763, 1277, 1791, 2306, 2820, 3334, 3848 },
    { 121, 378, 635, 892, 1149, 1406, 1663, 1920, 2177, 2434, 2691, 2948, 3205, 3462, 3719, 3976 },
    { 57, 185, 314, 442, 571, 699, 828, 956, 1085, 1213, 1342, 1470, 1599, 1727, 1856, 1984,
      2113, 2241, 2370, 2498, 2627, 2755, 2884, 3012, 3141, 3269, 3398, 3526, 3655, 3783, 3912, 4040 },
};

static uint32_t f32_bits(float f)
{
    uint32_t b;
    memcpy(&b, &f, sizeof b);
    return b;
}

TEST(nvn_translated_alpha_to_coverage_gives_the_3090s_masks)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes))
        return;
    static const VkSampleCountFlagBits counts[3] = { VK_SAMPLE_COUNT_2_BIT, VK_SAMPLE_COUNT_4_BIT,
                                                     VK_SAMPLE_COUNT_8_BIT };
    VkImageFormatProperties ifp;
    CHECK_EQ(VK(vkGetPhysicalDeviceImageFormatProperties)(g.phys, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
                                                          VK_IMAGE_TILING_OPTIMAL,
                                                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                              VK_IMAGE_USAGE_SAMPLED_BIT,
                                                          0, &ifp),
             VK_SUCCESS);
    for (uint32_t c = 0; c < 3; c++) {
        if (!(ifp.sampleCounts & counts[c]))
            continue;
        uint32_t n = (uint32_t)counts[c], full = (1u << n) - 1u;
        static rig k;
        CHECK(rig_up(&k, counts[c], true));
        uint32_t off = 0;
        for (uint32_t a = 0; a < STEPS; a++) {
            measure(&k, n, a);
            for (uint32_t i = 0; i < SIDE * SIDE; i++)
                off += (k.map[i] & full) != nvidia_mask(n, a, i);
        }
        CHECK_EQ(off, 0u);
        for (uint32_t l = 0; l < 4 * n; l++) {
            float t = (float)thresholds[c][l] / 4096.0f;
            CHECK_EQ(covered(&k, n, t), (l + 1) * SIDE * SIDE / 4);
            CHECK_EQ(covered(&k, n, f32_of(f32_bits(t) - 1)), l * SIDE * SIDE / 4);
        }
        static const uint32_t none[] = { 0x80000000u, 0xbf800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u, 1u };
        for (uint32_t i = 0; i < sizeof none / sizeof none[0]; i++)
            CHECK_EQ(covered(&k, n, f32_of(none[i])), 0u);
        static const uint32_t all[] = { 0x3f800001u, 0x40000000u, 0x7f800000u };
        for (uint32_t i = 0; i < sizeof all / sizeof all[0]; i++)
            CHECK_EQ(covered(&k, n, f32_of(all[i])), n * SIDE * SIDE);
        rig_down(&k);
    }
}
