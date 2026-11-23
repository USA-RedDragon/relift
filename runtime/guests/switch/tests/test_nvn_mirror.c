/**
 * @file test_nvn_mirror.c
 * @brief Memory pool mirrors (nvn_mirror.c), forced on a host that could import: a CPU write reaches a compute
 * program that reads it, and the bytes the program writes reach guest memory when the batch completes.
 */
#include "nvn/nvn_priv.h"
#include "nvn/nvn_vk.h"
#include "tc.h"

#include <relift/settings.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/** @brief Bytes of a page of the written-page bitmap. */
#define PAGE_BYTES 4096u

/** @brief Compute program: dst[i] = src[i] + 1 for i < n, through buffer references in push constants (glslang, SPIR-V 1.5). */
static const uint32_t copy_spv[] = {
    0x07230203, 0x00010500, 0x000d000b, 0x00000034, 0x00000000, 0x00020011,
    0x00000001, 0x00020011, 0x0000000b, 0x00020011, 0x000014e3, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e,
    0x000014e4, 0x00000001, 0x0007000f, 0x00000005, 0x00000004, 0x6e69616d,
    0x00000000, 0x0000000b, 0x00000014, 0x00060010, 0x00000004, 0x00000011,
    0x00000040, 0x00000001, 0x00000001, 0x00040047, 0x0000000b, 0x0000000b,
    0x0000001c, 0x00030047, 0x00000012, 0x00000002, 0x00050048, 0x00000012,
    0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x00000012, 0x00000001,
    0x00000023, 0x00000008, 0x00050048, 0x00000012, 0x00000002, 0x00000023,
    0x00000010, 0x00040047, 0x00000023, 0x00000006, 0x00000004, 0x00030047,
    0x00000024, 0x00000002, 0x00050048, 0x00000024, 0x00000000, 0x00000023,
    0x00000000, 0x00040047, 0x00000033, 0x0000000b, 0x00000019, 0x00020013,
    0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x00000006,
    0x00000020, 0x00000000, 0x00040017, 0x00000009, 0x00000006, 0x00000003,
    0x00040020, 0x0000000a, 0x00000001, 0x00000009, 0x0004003b, 0x0000000a,
    0x0000000b, 0x00000001, 0x0004002b, 0x00000006, 0x0000000c, 0x00000000,
    0x00040020, 0x0000000d, 0x00000001, 0x00000006, 0x00040015, 0x00000011,
    0x00000040, 0x00000000, 0x0005001e, 0x00000012, 0x00000011, 0x00000011,
    0x00000006, 0x00040020, 0x00000013, 0x00000009, 0x00000012, 0x0004003b,
    0x00000013, 0x00000014, 0x00000009, 0x00040015, 0x00000015, 0x00000020,
    0x00000001, 0x0004002b, 0x00000015, 0x00000016, 0x00000002, 0x00040020,
    0x00000017, 0x00000009, 0x00000006, 0x00020014, 0x0000001a, 0x0004002b,
    0x00000015, 0x0000001e, 0x00000001, 0x00040020, 0x0000001f, 0x00000009,
    0x00000011, 0x00030027, 0x00000022, 0x000014e5, 0x0003001d, 0x00000023,
    0x00000006, 0x0003001e, 0x00000024, 0x00000023, 0x00040020, 0x00000022,
    0x000014e5, 0x00000024, 0x0004002b, 0x00000015, 0x00000026, 0x00000000,
    0x00040020, 0x0000002c, 0x000014e5, 0x00000006, 0x0004002b, 0x00000006,
    0x0000002f, 0x00000001, 0x0004002b, 0x00000006, 0x00000032, 0x00000040,
    0x0006002c, 0x00000009, 0x00000033, 0x00000032, 0x0000002f, 0x0000002f,
    0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8,
    0x00000005, 0x00050041, 0x0000000d, 0x0000000e, 0x0000000b, 0x0000000c,
    0x0004003d, 0x00000006, 0x0000000f, 0x0000000e, 0x00050041, 0x00000017,
    0x00000018, 0x00000014, 0x00000016, 0x0004003d, 0x00000006, 0x00000019,
    0x00000018, 0x000500b0, 0x0000001a, 0x0000001b, 0x0000000f, 0x00000019,
    0x000300f7, 0x0000001d, 0x00000000, 0x000400fa, 0x0000001b, 0x0000001c,
    0x0000001d, 0x000200f8, 0x0000001c, 0x00050041, 0x0000001f, 0x00000020,
    0x00000014, 0x0000001e, 0x0004003d, 0x00000011, 0x00000021, 0x00000020,
    0x00040078, 0x00000022, 0x00000025, 0x00000021, 0x00050041, 0x0000001f,
    0x00000028, 0x00000014, 0x00000026, 0x0004003d, 0x00000011, 0x00000029,
    0x00000028, 0x00040078, 0x00000022, 0x0000002a, 0x00000029, 0x00060041,
    0x0000002c, 0x0000002d, 0x0000002a, 0x00000026, 0x0000000f, 0x0006003d,
    0x00000006, 0x0000002e, 0x0000002d, 0x00000002, 0x00000004, 0x00050080,
    0x00000006, 0x00000030, 0x0000002e, 0x0000002f, 0x00060041, 0x0000002c,
    0x00000031, 0x00000025, 0x00000026, 0x0000000f, 0x0005003e, 0x00000031,
    0x00000030, 0x00000002, 0x00000004, 0x000200f9, 0x0000001d, 0x000200f8,
    0x0000001d, 0x000100fd, 0x00010038
};

/** @brief Compute program: writes push.value to word 3 of the memory at the address stored at push.table (a pointer it reads, so the layer cannot know the target). */
static const uint32_t ptr_spv[] = {
    0x07230203, 0x00010500, 0x000d000b, 0x00000024, 0x00000000, 0x00020011,
    0x00000001, 0x00020011, 0x0000000b, 0x00020011, 0x000014e3, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e,
    0x000014e4, 0x00000001, 0x0006000f, 0x00000005, 0x00000004, 0x6e69616d,
    0x00000000, 0x0000000a, 0x00060010, 0x00000004, 0x00000011, 0x00000001,
    0x00000001, 0x00000001, 0x00030047, 0x00000008, 0x00000002, 0x00050048,
    0x00000008, 0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x00000008,
    0x00000001, 0x00000023, 0x00000008, 0x00030047, 0x00000011, 0x00000002,
    0x00050048, 0x00000011, 0x00000000, 0x00000023, 0x00000000, 0x00040047,
    0x00000017, 0x00000006, 0x00000004, 0x00030047, 0x00000018, 0x00000002,
    0x00050048, 0x00000018, 0x00000000, 0x00000023, 0x00000000, 0x00040047,
    0x00000023, 0x0000000b, 0x00000019, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000040, 0x00000000,
    0x00040015, 0x00000007, 0x00000020, 0x00000000, 0x0004001e, 0x00000008,
    0x00000006, 0x00000007, 0x00040020, 0x00000009, 0x00000009, 0x00000008,
    0x0004003b, 0x00000009, 0x0000000a, 0x00000009, 0x00040015, 0x0000000b,
    0x00000020, 0x00000001, 0x0004002b, 0x0000000b, 0x0000000c, 0x00000000,
    0x00040020, 0x0000000d, 0x00000009, 0x00000006, 0x00030027, 0x00000010,
    0x000014e5, 0x0003001e, 0x00000011, 0x00000006, 0x00040020, 0x00000010,
    0x000014e5, 0x00000011, 0x00040020, 0x00000013, 0x000014e5, 0x00000006,
    0x00030027, 0x00000016, 0x000014e5, 0x0003001d, 0x00000017, 0x00000007,
    0x0003001e, 0x00000018, 0x00000017, 0x00040020, 0x00000016, 0x000014e5,
    0x00000018, 0x0004002b, 0x0000000b, 0x0000001a, 0x00000003, 0x0004002b,
    0x0000000b, 0x0000001b, 0x00000001, 0x00040020, 0x0000001c, 0x00000009,
    0x00000007, 0x00040020, 0x0000001f, 0x000014e5, 0x00000007, 0x00040017,
    0x00000021, 0x00000007, 0x00000003, 0x0004002b, 0x00000007, 0x00000022,
    0x00000001, 0x0006002c, 0x00000021, 0x00000023, 0x00000022, 0x00000022,
    0x00000022, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003,
    0x000200f8, 0x00000005, 0x00050041, 0x0000000d, 0x0000000e, 0x0000000a,
    0x0000000c, 0x0004003d, 0x00000006, 0x0000000f, 0x0000000e, 0x00040078,
    0x00000010, 0x00000012, 0x0000000f, 0x00050041, 0x00000013, 0x00000014,
    0x00000012, 0x0000000c, 0x0006003d, 0x00000006, 0x00000015, 0x00000014,
    0x00000002, 0x00000010, 0x00040078, 0x00000016, 0x00000019, 0x00000015,
    0x00050041, 0x0000001c, 0x0000001d, 0x0000000a, 0x0000001b, 0x0004003d,
    0x00000007, 0x0000001e, 0x0000001d, 0x00060041, 0x0000001f, 0x00000020,
    0x00000019, 0x0000000c, 0x0000001a, 0x0005003e, 0x00000020, 0x0000001e,
    0x00000002, 0x00000004, 0x000100fd, 0x00010038
};

typedef struct ptr_push {
    uint64_t table;
    uint32_t value;
} ptr_push;

/**
 * @brief Compute program: sets the bit of target's page in the written-page bitmap at pages, as translated stores do,
 * then writes value to the word at target (glslang, SPIR-V 1.5):
 *
 *     layout(push_constant) uniform P { uint64_t pages; uint64_t target; uint value; } pc;
 *     uint page = uint(pc.target >> 12) & 0xfffffu;
 *     atomicOr(Words(pc.pages).v[page >> 5], 1u << (page & 31u));
 *     Words(pc.target).v[0] = pc.value;
 */
static const uint32_t mark_spv[] = {
    0x07230203, 0x00010500, 0x000d000b, 0x00000034, 0x00000000, 0x00020011,
    0x00000001, 0x00020011, 0x0000000b, 0x00020011, 0x000014e3, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e,
    0x000014e4, 0x00000001, 0x0006000f, 0x00000005, 0x00000004, 0x6e69616d,
    0x00000000, 0x0000000c, 0x00060010, 0x00000004, 0x00000011, 0x00000001,
    0x00000001, 0x00000001, 0x00030047, 0x0000000a, 0x00000002, 0x00050048,
    0x0000000a, 0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x0000000a,
    0x00000001, 0x00000023, 0x00000008, 0x00050048, 0x0000000a, 0x00000002,
    0x00000023, 0x00000010, 0x00040047, 0x0000001b, 0x00000006, 0x00000004,
    0x00030047, 0x0000001c, 0x00000002, 0x00050048, 0x0000001c, 0x00000000,
    0x00000023, 0x00000000, 0x00040047, 0x00000033, 0x0000000b, 0x00000019,
    0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00040015,
    0x00000006, 0x00000020, 0x00000000, 0x00040015, 0x00000009, 0x00000040,
    0x00000000, 0x0005001e, 0x0000000a, 0x00000009, 0x00000009, 0x00000006,
    0x00040020, 0x0000000b, 0x00000009, 0x0000000a, 0x0004003b, 0x0000000b,
    0x0000000c, 0x00000009, 0x00040015, 0x0000000d, 0x00000020, 0x00000001,
    0x0004002b, 0x0000000d, 0x0000000e, 0x00000001, 0x00040020, 0x0000000f,
    0x00000009, 0x00000009, 0x0004002b, 0x0000000d, 0x00000012, 0x0000000c,
    0x0004002b, 0x00000006, 0x00000015, 0x000fffff, 0x0004002b, 0x0000000d,
    0x00000017, 0x00000000, 0x00030027, 0x0000001a, 0x000014e5, 0x0003001d,
    0x0000001b, 0x00000006, 0x0003001e, 0x0000001c, 0x0000001b, 0x00040020,
    0x0000001a, 0x000014e5, 0x0000001c, 0x0004002b, 0x0000000d, 0x0000001f,
    0x00000005, 0x00040020, 0x00000021, 0x000014e5, 0x00000006, 0x0004002b,
    0x00000006, 0x00000023, 0x00000001, 0x0004002b, 0x00000006, 0x00000025,
    0x0000001f, 0x0004002b, 0x00000006, 0x00000028, 0x00000000, 0x0004002b,
    0x0000000d, 0x0000002d, 0x00000002, 0x00040020, 0x0000002e, 0x00000009,
    0x00000006, 0x00040017, 0x00000032, 0x00000006, 0x00000003, 0x0006002c,
    0x00000032, 0x00000033, 0x00000023, 0x00000023, 0x00000023, 0x00050036,
    0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005,
    0x00050041, 0x0000000f, 0x00000010, 0x0000000c, 0x0000000e, 0x0004003d,
    0x00000009, 0x00000011, 0x00000010, 0x000500c2, 0x00000009, 0x00000013,
    0x00000011, 0x00000012, 0x00040071, 0x00000006, 0x00000014, 0x00000013,
    0x000500c7, 0x00000006, 0x00000016, 0x00000014, 0x00000015, 0x00050041,
    0x0000000f, 0x00000018, 0x0000000c, 0x00000017, 0x0004003d, 0x00000009,
    0x00000019, 0x00000018, 0x00040078, 0x0000001a, 0x0000001d, 0x00000019,
    0x000500c2, 0x00000006, 0x00000020, 0x00000016, 0x0000001f, 0x00060041,
    0x00000021, 0x00000022, 0x0000001d, 0x00000017, 0x00000020, 0x000500c7,
    0x00000006, 0x00000026, 0x00000014, 0x00000025, 0x000500c4, 0x00000006,
    0x00000027, 0x00000023, 0x00000026, 0x000700f1, 0x00000006, 0x00000029,
    0x00000022, 0x00000023, 0x00000028, 0x00000027, 0x00040078, 0x0000001a,
    0x0000002c, 0x00000011, 0x00050041, 0x0000002e, 0x0000002f, 0x0000000c,
    0x0000002d, 0x0004003d, 0x00000006, 0x00000030, 0x0000002f, 0x00060041,
    0x00000021, 0x00000031, 0x0000002c, 0x00000017, 0x00000017, 0x0005003e,
    0x00000031, 0x00000030, 0x00000002, 0x00000004, 0x000100fd, 0x00010038
};

typedef struct mark_push {
    uint64_t pages, target;
    uint32_t value;
} mark_push;

typedef struct copy_push {
    uint64_t src, dst;
    uint32_t n;
} copy_push;

/** @brief Whether a host Vulkan device is up; without one the test is skipped, and the device it runs on is printed; the tests fail instead of skipping when RELIFT_VK_REQUIRE is set (CI sets it where lavapipe is installed). */
static bool have_device(void)
{
    bool up = nvn_vk_ready();
    if (up)
        printf("  mirror test on host device %s\n", g.name);
    else
        printf("  skipped: no host Vulkan device\n");
    return up;
}

/** @brief Runs the program over n words from GPU address src to dst as the layer's replay does around a dispatch. */
static bool run_copy(replay *r, uint64_t src, uint64_t dst, uint32_t n)
{
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                     .codeSize = sizeof copy_spv,
                                     .pCode = copy_spv };
    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = sizeof(copy_push) };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .pushConstantRangeCount = 1,
                                       .pPushConstantRanges = &pcr };
    VkShaderModule sm = 0;
    VkPipelineLayout pl = 0;
    VkPipeline pipe = 0;
    bool ok = VK(vkCreateShaderModule)(g.dev, &smi, NULL, &sm) == VK_SUCCESS &&
              VK(vkCreatePipelineLayout)(g.dev, &pli, NULL, &pl) == VK_SUCCESS;
    if (ok) {
        VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                            .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                       .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                       .module = sm,
                                                       .pName = "main" },
                                            .layout = pl };
        ok = VK(vkCreateComputePipelines)(g.dev, NULL, 1, &cpi, NULL, &pipe) == VK_SUCCESS;
    }
    if (ok)
        ok = nvn_mirror_sync(r, false) && nvn_vk_begin(r);
    if (ok) {
        copy_push p = { src, dst, n };
        VK(vkCmdBindPipeline)(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        VK(vkCmdPushConstants)(g.cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
        VK(vkCmdDispatch)(g.cb, (n + 63) / 64, 1, 1);
        nvn_vk_barrier(g.cb);
        nvn_mirror_written(dst, 4ull * n);
        ok = nvn_vk_flush(r);
    }
    if (pipe)
        VK(vkDestroyPipeline)(g.dev, pipe, NULL);
    if (pl)
        VK(vkDestroyPipelineLayout)(g.dev, pl, NULL);
    if (sm)
        VK(vkDestroyShaderModule)(g.dev, sm, NULL);
    return ok;
}

TEST(nvn_mirror_carries_cpu_writes_to_the_gpu_and_gpu_writes_to_the_cpu)
{
    rl_ctx *c = t_boot(NULL);
    CHECK_EQ(rl_settings_set("nvn.memory", "mirror"), RL_SETTING_OK);
    bool up = have_device();
    CHECK(up || !getenv("RELIFT_VK_REQUIRE"));
    if (!up) {
        rl_settings_set("nvn.memory", "auto");
        return;
    }
    uint64_t heap = t_heap(c, 0x400000);
    uint64_t hp = hos.vm.host_page;
    uint64_t at = (heap + 0x20000 + hp - 1) / hp * hp;
    uint64_t size = 16 * hp, pool = heap + 0x100;
    uint8_t *m = t_mem();
    uint32_t n = 3000;
    for (uint32_t i = 0; i < n; i++)
        rl_st32(m, at + 4ull * i, 100 + i);
    CHECK(nvn_pool_make(pool, at, size, 0));
    uint64_t gpu = nvn_pool_gpu(pool);
    CHECK(gpu != at);
    CHECK_EQ(nvn_gpu_guest(gpu + 0x44), at + 0x44);
    uint64_t dst = gpu + 8 * hp;
    replay r;
    memset(&r, 0, sizeof r);
    r.c = c;
    r.mem = m;
    CHECK(run_copy(&r, gpu, dst, n));
    for (uint32_t i = 0; i < n; i += 97)
        CHECK_EQ(rl_ld32(m, at + 8 * hp + 4ull * i), 101 + i);
    CHECK_EQ(rl_ld32(m, at + 8 * hp + 4ull * (n - 1)), 100 + n);
    for (uint32_t i = 0; i < n; i++)
        rl_st32(m, at + 4ull * i, 7 * i);
    CHECK(run_copy(&r, gpu, dst, n));
    for (uint32_t i = 0; i < n; i += 13)
        CHECK_EQ(rl_ld32(m, at + 8 * hp + 4ull * i), 7 * i + 1);
    rl_st32(m, at + 4 * 5, 555);
    CHECK(run_copy(&r, gpu, dst, n));
    CHECK_EQ(rl_ld32(m, at + 8 * hp + 4 * 5), 556u);
    CHECK_EQ(rl_ld32(m, at + 8 * hp + 4 * 6), 43u);
    rl_settings_set("nvn.memory", "auto");
}

TEST(nvn_mirror_carries_writes_through_a_pointer_a_program_reads_back_to_the_cpu)
{
    rl_ctx *c = t_boot(NULL);
    CHECK_EQ(rl_settings_set("nvn.memory", "mirror"), RL_SETTING_OK);
    bool up = have_device();
    CHECK(up || !getenv("RELIFT_VK_REQUIRE"));
    if (!up) {
        rl_settings_set("nvn.memory", "auto");
        return;
    }
    uint64_t heap = t_heap(c, 0x400000);
    uint64_t hp = hos.vm.host_page;
    uint64_t at = (heap + 0x20000 + hp - 1) / hp * hp;
    uint64_t size = 16 * hp, pool = heap + 0x100;
    uint8_t *m = t_mem();
    CHECK(nvn_pool_make(pool, at, size, 0));
    uint64_t gpu = nvn_pool_gpu(pool);
    uint64_t target = gpu + 9 * hp;
    rl_st64(m, at, target);
    rl_st32(m, at + 9 * hp + 12, 1);
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                     .codeSize = sizeof ptr_spv,
                                     .pCode = ptr_spv };
    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = sizeof(ptr_push) };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .pushConstantRangeCount = 1,
                                       .pPushConstantRanges = &pcr };
    VkShaderModule sm = 0;
    VkPipelineLayout pl = 0;
    VkPipeline pipe = 0;
    CHECK_EQ(VK(vkCreateShaderModule)(g.dev, &smi, NULL, &sm), VK_SUCCESS);
    CHECK_EQ(VK(vkCreatePipelineLayout)(g.dev, &pli, NULL, &pl), VK_SUCCESS);
    VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                   .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                   .module = sm,
                                                   .pName = "main" },
                                        .layout = pl };
    CHECK_EQ(VK(vkCreateComputePipelines)(g.dev, NULL, 1, &cpi, NULL, &pipe), VK_SUCCESS);
    replay r;
    memset(&r, 0, sizeof r);
    r.c = c;
    r.mem = m;
    CHECK(nvn_mirror_sync(&r, false) && nvn_vk_begin(&r));
    ptr_push p = { gpu, 0xabcd };
    VK(vkCmdBindPipeline)(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    VK(vkCmdPushConstants)(g.cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    VK(vkCmdDispatch)(g.cb, 1, 1, 1);
    nvn_vk_barrier(g.cb);
    nvn_mirror_written_all();
    CHECK(nvn_vk_flush(&r));
    CHECK_EQ(rl_ld32(m, at + 9 * hp + 12), 0xabcdu);
    CHECK_EQ(rl_ld32(m, at + 9 * hp + 8), 0u);
    VK(vkDestroyPipeline)(g.dev, pipe, NULL);
    VK(vkDestroyPipelineLayout)(g.dev, pl, NULL);
    VK(vkDestroyShaderModule)(g.dev, sm, NULL);
    rl_settings_set("nvn.memory", "auto");
}

TEST(nvn_mirror_reads_back_only_the_pages_a_program_marks)
{
    rl_ctx *c = t_boot(NULL);
    CHECK_EQ(rl_settings_set("nvn.memory", "mirror"), RL_SETTING_OK);
    bool up = have_device();
    CHECK(up || !getenv("RELIFT_VK_REQUIRE"));
    if (!up) {
        rl_settings_set("nvn.memory", "auto");
        return;
    }
    uint64_t heap = t_heap(c, 0x400000);
    uint64_t hp = hos.vm.host_page;
    uint64_t at = (heap + 0x20000 + hp - 1) / hp * hp;
    uint64_t size = 64 * hp, pool = heap + 0x100;
    uint8_t *m = t_mem();
    CHECK(nvn_pool_make(pool, at, size, 0));
    uint64_t gpu = nvn_pool_gpu(pool);
    uint64_t pages = nvn_mirror_written_pages_address();
    CHECK(pages != 0);
    rl_st32(m, at + 37 * hp + 20, 1);
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                     .codeSize = sizeof mark_spv,
                                     .pCode = mark_spv };
    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = sizeof(mark_push) };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .pushConstantRangeCount = 1,
                                       .pPushConstantRanges = &pcr };
    VkShaderModule sm = 0;
    VkPipelineLayout pl = 0;
    VkPipeline pipe = 0;
    CHECK_EQ(VK(vkCreateShaderModule)(g.dev, &smi, NULL, &sm), VK_SUCCESS);
    CHECK_EQ(VK(vkCreatePipelineLayout)(g.dev, &pli, NULL, &pl), VK_SUCCESS);
    VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                   .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                   .module = sm,
                                                   .pName = "main" },
                                        .layout = pl };
    CHECK_EQ(VK(vkCreateComputePipelines)(g.dev, NULL, 1, &cpi, NULL, &pipe), VK_SUCCESS);
    replay r;
    memset(&r, 0, sizeof r);
    r.c = c;
    r.mem = m;
    uint64_t up0, down0, syncs0, read0, marked0;
    nvn_mirror_stats(&up0, &down0, &syncs0, &read0, &marked0);
    CHECK(nvn_mirror_sync(&r, false) && nvn_vk_begin(&r));
    mark_push p = { pages, gpu + 37 * hp + 20, 0x5eed };
    VK(vkCmdBindPipeline)(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    VK(vkCmdPushConstants)(g.cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    VK(vkCmdDispatch)(g.cb, 1, 1, 1);
    nvn_vk_barrier(g.cb);
    nvn_mirror_written_marked();
    CHECK(nvn_vk_flush(&r));
    CHECK_EQ(rl_ld32(m, at + 37 * hp + 20), 0x5eedu);
    CHECK_EQ(rl_ld32(m, at + 37 * hp + 16), 0u);
    uint64_t up1, down1, syncs1, read1, marked1;
    nvn_mirror_stats(&up1, &down1, &syncs1, &read1, &marked1);
    CHECK_EQ(marked1 - marked0, 1u);
    CHECK_EQ(read1 - read0, (uint64_t)PAGE_BYTES);
    CHECK(!nvn_mirror_readback_pending());
    VK(vkDestroyPipeline)(g.dev, pipe, NULL);
    VK(vkDestroyPipelineLayout)(g.dev, pl, NULL);
    VK(vkDestroyShaderModule)(g.dev, sm, NULL);
    rl_settings_set("nvn.memory", "auto");
}

static double ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/** @brief The time one present's refresh of every synced registered texture takes, over frames presents after one more. */
static double refresh_ms(replay *r, int frames)
{
    g.presents++;
    if (nvn_texset_sync_registered(r) != RL_OK)
        return 1e9;
    double t0 = ms_now();
    for (int f = 0; f < frames; f++) {
        g.presents++;
        if (nvn_texset_sync_registered(r) != RL_OK)
            return 1e9;
    }
    return (ms_now() - t0) / frames;
}

/**
 * @brief A program that reads texture handles out of memory names pool ids the host cannot rewrite, so every id of a
 * texture pool of 33068 descriptors (a size games use) must have a slot of its own, including ids past 16384.
 */
TEST(nvn_every_id_of_a_large_texture_pool_keeps_its_own_slot)
{
    rl_ctx *c = t_boot(NULL);
    bool up = have_device();
    CHECK(up || !getenv("RELIFT_VK_REQUIRE"));
    if (!up)
        return;
    enum { POOL = 33068, SPAN = 0x200 };
    uint64_t heap = t_heap(c, 0x1000000);
    uint64_t hp = hos.vm.host_page;
    uint64_t at = (heap + 0x20000 + hp - 1) / hp * hp, pool = heap + 0x100;
    uint8_t *m = t_mem();
    CHECK(nvn_pool_make(pool, at, 4 * SPAN + hp, 0));
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    replay r;
    memset(&r, 0, sizeof r);
    r.c = c;
    r.mem = m;
    r.st = &st;
    CHECK(nvn_texset());
    CHECK(nvn_texset_pinned() >= POOL);
    static const uint32_t ids[] = { 16383, 16384, 16465, POOL - 1 };
    for (uint32_t i = 0; i < 4; i++) {
        nvn_tex_desc d = { .target = 1, .format = 37, .width = 8, .height = 8, .depth = 1, .levels = 1,
                           .flags = 1u << 4, .stride = 64, .swizzle = { 2, 3, 4, 5 }, .pool = pool,
                           .offset = (uint64_t)i * SPAN };
        CHECK(nvn_tex_layout(&d));
        uint64_t tex = heap + 0x800000 + 0x100 * (uint64_t)i;
        CHECK(nvn_texture_record(tex, &d, NULL));
        c->x[1] = ids[i];
        c->x[2] = tex;
        c->x[3] = 0;
        nvnh_nvnTexturePoolRegisterTexture(c, m);
    }
    CHECK_EQ(nvn_texset_sync_registered(&r), RL_OK);
    CHECK(nvn_vk_flush(&r));
}

/**
 * @brief The child of nvn_an_image_past_the_old_array_size_keeps_its_own_slot: the shader table in
 * $RELIFT_NVN_SHADERS says the programs use one storage image array, so the set gives the device's whole storage
 * image budget to it, and an image registered at id 33067 of a pool of 33068 keeps its id as its slot. Without
 * $RL_IMAGE_CHILD it is a test that passes.
 */
TEST(nvn_image_past_the_old_array_size_child)
{
    if (!getenv("RL_IMAGE_CHILD"))
        return;
    rl_ctx *c = t_boot(NULL);
    bool up = have_device();
    CHECK(up || !getenv("RELIFT_VK_REQUIRE"));
    if (!up)
        return;
    enum { POOL = 33068, ARRAYS = 54 };
    CHECK(nvn_texset());
    VkPhysicalDeviceVulkan12Properties p12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12 };
    VK(vkGetPhysicalDeviceProperties2)(g.phys, &p2);
    uint32_t budget = p12.maxPerStageDescriptorUpdateAfterBindStorageImages;
    if (p12.maxDescriptorSetUpdateAfterBindStorageImages < budget)
        budget = p12.maxDescriptorSetUpdateAfterBindStorageImages;
    CHECK(nvn_texset_declares_image(32 + 8 * 5 + 3));
    CHECK(!nvn_texset_declares_image(32));
    if (budget < POOL || nvn_texset_pinned() < POOL) {
        printf("  skipped: the device holds %u storage images\n", budget);
        return;
    }
    CHECK(budget / ARRAYS < POOL);
    CHECK(nvn_texset_images() >= POOL);
    uint64_t heap = t_heap(c, 0x1000000);
    uint64_t hp = hos.vm.host_page;
    uint64_t at = (heap + 0x20000 + hp - 1) / hp * hp, pool = heap + 0x100;
    uint8_t *m = t_mem();
    CHECK(nvn_pool_make(pool, at, 0x1000 + hp, 0));
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    replay r;
    memset(&r, 0, sizeof r);
    r.c = c;
    r.mem = m;
    r.st = &st;
    nvn_tex_desc d = { .target = 1, .format = 37, .width = 8, .height = 8, .depth = 1, .levels = 1, .flags = 1u << 4,
                       .stride = 64, .swizzle = { 2, 3, 4, 5 }, .pool = pool, .offset = 0 };
    CHECK(nvn_tex_layout(&d));
    uint64_t tex = heap + 0x800000;
    CHECK(nvn_texture_record(tex, &d, NULL));
    c->x[1] = POOL - 1;
    c->x[2] = tex;
    c->x[3] = 0;
    nvnh_nvnTexturePoolRegisterImage(c, m);
    CHECK_EQ(nvn_texset_sync_registered(&r), RL_OK);
    CHECK(nvn_vk_flush(&r));
}

/**
 * @brief A formatted surface access of an RGB9E5 image (NVN format 64) moves its 32-bit texels unchanged through the
 * image's raw view on every host, as the console does (it has no shared-exponent surface encoder, so programs pack
 * the format themselves): its kind constant says so whether or not the host stores E5B9G9R9, and the store is never
 * dropped.
 */
TEST(nvn_an_rgb9e5_image_moves_raw_texels)
{
    rl_ctx *c = t_boot(NULL);
    bool up = have_device();
    CHECK(up || !getenv("RELIFT_VK_REQUIRE"));
    if (!up)
        return;
    CHECK(nvn_texset());
    uint64_t heap = t_heap(c, 0x1000000);
    uint64_t hp = hos.vm.host_page;
    uint64_t at = (heap + 0x20000 + hp - 1) / hp * hp, pool = heap + 0x100;
    uint8_t *m = t_mem();
    CHECK(nvn_pool_make(pool, at, 0x1000 + hp, 0));
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    replay r;
    memset(&r, 0, sizeof r);
    r.c = c;
    r.mem = m;
    r.st = &st;
    nvn_tex_desc d = { .target = 1, .format = 64, .width = 8, .height = 8, .depth = 1, .levels = 1, .flags = 1u << 4,
                       .stride = 32, .swizzle = { 2, 3, 4, 5 }, .pool = pool, .offset = 0 };
    CHECK(nvn_tex_layout(&d));
    uint64_t tex = heap + 0x800000;
    CHECK(nvn_texture_record(tex, &d, NULL));
    c->x[1] = 7;
    c->x[2] = tex;
    c->x[3] = 0;
    nvnh_nvnTexturePoolRegisterImage(c, m);
    uint32_t slot = 0, kind = 9, dim = 0;
    CHECK_EQ(nvn_texset_image(&r, 7, &slot, &kind, &dim), RL_OK);
    CHECK_EQ(kind, 0);
    CHECK_EQ(nvn_texset_surface_kind(slot, kind), NVN_SURFACE_KIND_RGB9E5);
    CHECK(nvn_vk_flush(&r));
}

/** @brief Writes a shader table interface file whose one program uses storage image array binding. */
static bool write_image_iface(const char *dir, uint32_t binding)
{
    uint8_t b[12 + 40 + 16] = { 0 };
    memcpy(b, "RLSPI002", 8);
    b[8] = 1;
    b[48] = 1;
    b[52] = 12;
    memcpy(b + 56, &binding, 4);
    char path[PATH_MAX + 16];
    snprintf(path, sizeof path, "%s/table.rlspi", dir);
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(b, 1, sizeof b, f) == sizeof b;
    return f && fclose(f) == 0 && ok;
}

/**
 * @brief A texture pool of 33068 descriptors whose images reach past the 2^20 / 54 storage images an array held when
 * the layout declared all 54 arrays: with the arrays the programs use declared, an image at id 33067 keeps its slot.
 */
TEST(nvn_an_image_past_the_old_array_size_keeps_its_own_slot)
{
    char dir[PATH_MAX], exe[PATH_MAX];
    const char *base = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/relift-img-XXXXXX", base && *base ? base : "/tmp");
    CHECK(mkdtemp(dir) != NULL);
    CHECK(write_image_iface(dir, 32 + 8 * 5 + 3));
    CHECK(rl_exe_path(exe, sizeof exe));
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        setenv("RELIFT_NVN_SHADERS", dir, 1);
        unsetenv("RELIFT_NVN_TABLES");
        setenv("RELIFT_TEST_ONE", "nvn_image_past_the_old_array_size_child", 1);
        setenv("RL_IMAGE_CHILD", "1", 1);
        char *argv[] = { exe, NULL };
        execv(exe, argv);
        _exit(126);
    }
    int st = 0;
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFEXITED(st));
    CHECK_EQ(WEXITSTATUS(st), 0);
    char path[PATH_MAX + 16];
    snprintf(path, sizeof path, "%s/table.rlspi", dir);
    remove(path);
    rmdir(dir);
}

TEST(nvn_registered_textures_cost_nothing_per_frame_until_their_bytes_change)
{
    rl_ctx *c = t_boot(NULL);
    bool up = have_device();
    CHECK(up || !getenv("RELIFT_VK_REQUIRE"));
    if (!up)
        return;
    enum { FEW = 50, MANY = 5000, SPAN = 0x200, FIRST = 9000 };
    uint64_t heap = t_heap(c, 0x1000000);
    uint64_t hp = hos.vm.host_page;
    uint64_t at = (heap + 0x20000 + hp - 1) / hp * hp, pool = heap + 0x100;
    uint8_t *m = t_mem();
    CHECK(nvn_pool_make(pool, at, (uint64_t)MANY * SPAN + hp, 0));
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    replay r;
    memset(&r, 0, sizeof r);
    r.c = c;
    r.mem = m;
    r.st = &st;
    CHECK(nvn_texset());
    CHECK(nvn_texset_pinned() > FIRST + MANY);
    double few = 0, many = 0;
    for (uint32_t i = 0; i < MANY; i++) {
        nvn_tex_desc d = { .target = 1, .format = 37, .width = 8, .height = 8, .depth = 1, .levels = 1,
                           .flags = 1u << 4, .stride = 64, .swizzle = { 2, 3, 4, 5 }, .pool = pool,
                           .offset = (uint64_t)i * SPAN };
        CHECK(nvn_tex_layout(&d));
        uint64_t tex = heap + 0x800000 + 0x100 * (uint64_t)i;
        CHECK(nvn_texture_record(tex, &d, NULL));
        for (uint32_t k = 0; k < SPAN; k += 4)
            rl_st32(m, at + (uint64_t)i * SPAN + k, 0xa55a0000u | i);
        c->x[1] = FIRST + i;
        c->x[2] = tex;
        c->x[3] = 0;
        nvnh_nvnTexturePoolRegisterTexture(c, m);
        if (i + 1 == FEW) {
            CHECK_EQ(nvn_texset_sync_registered(&r), RL_OK);
            few = refresh_ms(&r, 20);
        }
    }
    CHECK_EQ(nvn_texset_sync_registered(&r), RL_OK);
    CHECK(nvn_vk_flush(&r));
    many = refresh_ms(&r, 20);
    printf("  an unchanged frame: %.4f ms with %u registered textures, %.4f ms with %u\n", few, FEW, many, MANY);
    CHECK(many < few + 0.05);
    vimg *v = nvn_vk_image(&r, heap + 0x800000 + 0x100 * 4711ull);
    CHECK(v != NULL);
    uint64_t before = v ? v->hash : 0;
    CHECK(!t_write(at + 4711ull * SPAN + 8, 0x77).faulted);
    g.presents++;
    CHECK_EQ(nvn_texset_sync_registered(&r), RL_OK);
    CHECK(v && v->hash != before);
    vimg *next = nvn_vk_image(&r, heap + 0x800000 + 0x100 * 4710ull);
    CHECK(next != NULL);
    uint64_t kept = next ? next->hash : 0, kept_edges = next ? next->edge_hash : 0;
    CHECK(!t_write(at + 4711ull * SPAN + 12, 0x66).faulted);
    g.presents++;
    CHECK_EQ(nvn_texset_sync_registered(&r), RL_OK);
    CHECK(kept != 0);
    CHECK(next && next->hash == kept && next->edge_hash == kept_edges);
    CHECK(v && v->hash != before);
    CHECK(nvn_vk_flush(&r));
}

TEST(nvn_a_pass_with_an_unbound_color_slot_below_a_bound_one_begins)
{
    rl_ctx *c = t_boot(NULL);
    bool up = have_device();
    CHECK(up || !getenv("RELIFT_VK_REQUIRE"));
    if (!up)
        return;
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t hp = hos.vm.host_page;
    uint64_t at = (heap + 0x20000 + hp - 1) / hp * hp, pool = heap + 0x100, tex = heap + 0x800;
    CHECK(nvn_pool_make(pool, at, 0x10000, 0));
    nvn_tex_desc d = { .target = 1, .format = 37, .width = 8, .height = 8, .depth = 1, .levels = 1,
                       .flags = 1u << 4, .stride = 64, .swizzle = { 2, 3, 4, 5 }, .pool = pool };
    CHECK(nvn_tex_layout(&d));
    CHECK(nvn_texture_record(tex, &d, NULL));
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    replay r;
    memset(&r, 0, sizeof r);
    r.c = c;
    r.mem = t_mem();
    r.st = &st;
    vimg *v = nvn_vk_image(&r, tex);
    CHECK(v != NULL);
    if (!v)
        return;
    st.ncolor = 2;
    st.color[1] = (rt){ .v = v, .layers = 1, .view = v->view, .format = v->format };
    VkRect2D area;
    CHECK(nvn_vk_begin_rendering(&r, &area));
    CHECK_EQ(area.extent.width, 8);
    CHECK(nvn_vk_begin_rendering_layered(&r, &area, true));
    nvn_vk_end_rendering();
    CHECK(nvn_vk_flush(&r));
}
