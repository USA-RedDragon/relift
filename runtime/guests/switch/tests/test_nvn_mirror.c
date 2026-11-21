/**
 * @file test_nvn_mirror.c
 * @brief Memory pool mirrors (nvn_mirror.c), forced on a host that could import: a CPU write reaches a compute
 * program that reads it, and the bytes the program writes reach guest memory when the batch completes.
 */
#include "nvn/nvn_priv.h"
#include "nvn/nvn_vk.h"
#include "tc.h"

#include <relift/settings.h>

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

typedef struct copy_push {
    uint64_t src, dst;
    uint32_t n;
} copy_push;

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
    if (!nvn_vk_ready()) {
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
