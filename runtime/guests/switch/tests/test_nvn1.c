/**
 * @file test_nvn1.c
 * @brief The NVN layer serving an NVN1 (Switch) driver: the version record,
 * the per-version handles, device infos and formats, the Maxwell GOB layout
 * and the NVN1 program format.
 */
#include "nvn/nvn_priv.h"
#include "tc.h"

#define RET 0x7000000ull

static uint64_t call(rl_ctx *c, rl_fn_t *fn, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3)
{
    c->x[0] = x0;
    c->x[1] = x1;
    c->x[2] = x2;
    c->x[3] = x3;
    c->x[30] = RET;
    return fn(c, t_mem());
}

static const char *const impure[] = { "nvnTextureViewCompare", "calls memcmp", 0 };

static const struct rl_nvn_driver nvn1 = {
    .generation = 1,
    .api_major = 55,
    .api_minor = 15,
    .sdk = "16.2.0",
    .layout_error = "no layout",
    .nimpure = 1,
    .impure = impure,
};

TEST(nvn1_handles_carry_bit_32)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    rl_nvn_set_driver(&nvn1);
    CHECK_EQ(nvn_generation(), 1);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetTextureHandle, heap, 5, 3, 0), RET);
    CHECK_EQ(c->x[0], 5 | 3ull << 20 | 1ull << 32);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetTextureHandle, heap, 1, 0x800, 0), RET);
    CHECK_EQ(c->x[0], 0xffffffff80000001ull | 1ull << 32);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetSeparateTextureHandle, heap, 9, 0, 0), RET);
    CHECK_EQ(c->x[0], 9 | 1ull << 32);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetTexelFetchHandle, heap, 9, 0, 0), RET);
    CHECK_EQ(c->x[0], 9 | 1ull << 32);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetSeparateSamplerHandle, heap, 9, 0, 0), RET);
    CHECK_EQ(c->x[0], 9ull << 20);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetImageHandle, heap, 9, 0, 0), RET);
    CHECK_EQ(c->x[0], 9);
    rl_nvn_set_driver(NULL);
    CHECK_EQ(nvn_generation(), 2);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetTextureHandle, heap, 5, 3, 0), RET);
    CHECK_EQ(c->x[0], 5 | 3ull << 20);
}

TEST(nvn1_device_infos_and_formats_follow_the_version)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    static const int32_t ints[] = { 55, 15, 14, 0, 0 };
    static const uint8_t device[] = { 0, 0, 0, 1, 1 };
    static const struct rl_nvn_layout l = { .nints = 5, .ints = ints, .int_device = device, .pool_page_info = RL_NVN_NONE };
    struct rl_nvn_driver d = nvn1;
    d.layout = &l;
    d.layout_error = NULL;
    rl_nvn_set_driver(&d);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetInteger, heap, 0, heap + 0x100, 0), RET);
    CHECK_EQ(rl_ld32(t_mem(), heap + 0x100), 55);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetInteger, heap, 4, heap + 0x100, 0), RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    int32_t v = 0;
    CHECK(nvn_version_measured(43, &v));
    CHECK_EQ(v, 262144);
    CHECK(!nvn_version_measured(3, &v));
    uint32_t bpb, bw, bh;
    CHECK(nvn_tex_format(160, &bpb, &bw, &bh));
    CHECK(!nvn_tex_format(161, &bpb, &bw, &bh));
    CHECK_EQ(nvn_impure("nvnTextureViewCompare") != NULL, 1);
    CHECK(nvn_impure("nvnTextureViewGetFormat") == NULL);
    CHECK(nvn_pool_make(heap + 0x1000, 0, 0x40000, 0x2a1));
    CHECK_EQ(call(c, nvnh_nvnMemoryPoolMapVirtual, heap + 0x1000, 0, 0, 0), RL_PENDING);
    rl_nvn_set_driver(NULL);
    CHECK(nvn_tex_format(161, &bpb, &bw, &bh));
}

TEST(nvn1_texels_land_in_maxwell_gobs)
{
    rl_nvn_set_driver(&nvn1);
    CHECK_EQ(nvn_gob_offset(0, 1), 16);
    CHECK_EQ(nvn_gob_offset(16, 0), 32);
    CHECK_EQ(nvn_gob_offset(0, 2), 64);
    CHECK_EQ(nvn_gob_offset(32, 0), 256);
    CHECK_EQ(nvn_gob_offset(63, 7), 511);
    nvn_tex_desc d = { .target = 1, .format = 0x25, .width = 16, .height = 8, .levels = 1 };
    CHECK(nvn_tex_layout(&d));
    static uint8_t storage[512], in[512], out[512];
    for (unsigned i = 0; i < sizeof in; i++)
        in[i] = (uint8_t)(i * 7 + i / 251);
    nvn_region all = { 0, 0, 0, 16, 8, 1 };
    CHECK(nvn_tex_copy(&d, storage, 0, 0, &all, in, true));
    CHECK_EQ(storage[16], in[64]);
    CHECK_EQ(storage[32], in[16]);
    CHECK_EQ(storage[64], in[128]);
    CHECK(nvn_tex_copy(&d, storage, 0, 0, &all, out, false));
    CHECK(memcmp(in, out, sizeof in) == 0);
    rl_nvn_set_driver(NULL);
    CHECK_EQ(nvn_gob_offset(0, 1), 16);
    CHECK_EQ(nvn_gob_offset(16, 0), 64);
}

/** @brief A Maxwell instruction of opcode bits op (top 12), predicate PT and condition T, with a branch offset. */
static uint64_t maxwell(uint64_t op, int32_t offset)
{
    return op << 52 | ((uint64_t)(uint32_t)offset & 0xffffff) << 20 | 7ull << 16 | 0xf;
}

TEST(nvn1_programs_are_their_sph_and_reachable_code)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t p = t_heap(c, 0x200000);
    uint8_t *m = t_mem();
    memset(m + p, 0, 0x1000);
    rl_st32(m, p, 0x12345678);
    rl_st32(m, p + 0x30, 1 | 3 << 5 | 5 << 10);
    uint64_t code = p + 0x30 + 0x50;
    rl_st64(m, code + 8, maxwell(0xe24, 24));
    rl_st64(m, code + 40, maxwell(0xe30, 0));
    rl_st64(m, code + 48, maxwell(0xe24, -8));
    uint64_t start = 0, len = 0;
    CHECK(nvn_v1_program(m, p, &start, &len));
    CHECK_EQ(start, p + 0x30);
    CHECK_EQ(len, 0x50 + 64);
    int stage = -1;
    CHECK(nvn_v1_sph(m + start, &stage));
    CHECK_EQ(stage, 1);
    rl_st32(m, p + 0x30, 1 | 4 << 5 | 5 << 10);
    CHECK(!nvn_v1_sph(m + start, NULL));
    CHECK(nvn_v1_program(m, p, &start, &len));
    rl_st32(m, p, 0);
    CHECK(!nvn_v1_program(m, p, &start, &len));
}

TEST(nvn1_programs_follow_pushes_and_conditional_ends)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t p = t_heap(c, 0x200000);
    uint8_t *m = t_mem();
    memset(m + p, 0, 0x1000);
    rl_st32(m, p, 0x12345678);
    uint64_t code = p + 0x30;
    rl_st64(m, code + 8, maxwell(0xe29, 88));
    rl_st64(m, code + 16, maxwell(0xe30, 0) & ~0xfull);
    rl_st64(m, code + 24, maxwell(0xe30, 0));
    rl_st64(m, code + 104, maxwell(0xe30, 0));
    uint64_t start = 0, len = 0;
    CHECK(nvn_v1_program(m, p, &start, &len));
    CHECK_EQ(start, p + 0x30);
    CHECK_EQ(len, 128);
    rl_st64(m, code + 8, maxwell(0xe24, 1));
    CHECK(!nvn_v1_program(m, p, &start, &len));
}

TEST(nvn1_hook_serves_both_consoles)
{
    const rl_switch_hook *h = rl_nvn_switch_hook();
    CHECK(h->consoles & 1u << RL_CONSOLE_SWITCH);
    CHECK(h->consoles & 1u << RL_CONSOLE_SWITCH2);
    CHECK(h->wants(h->user, "nvnBootstrapLoader"));
    CHECK(!h->wants(h->user, "vkCreateInstance"));
}
