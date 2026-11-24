/**
 * @file test_nvn_clear.c
 * @brief Render target clears replayed one command at a time: the float, signed and unsigned color clears hand their
 * 16 bytes to the host clear unchanged. The device calls go to recording stand-ins.
 */
#include "nvn_priv.h"
#include "nvn_vk.h"
#include "runner.h"

#include <stdlib.h>
#include <string.h>

static struct {
    unsigned clears;
    VkClearAttachment att;
    VkClearRect rect;
} calls;

static void fake_begin(VkCommandBuffer cb, const VkRenderingInfo *ri) { (void)cb, (void)ri; }

static void fake_end(VkCommandBuffer cb) { (void)cb; }

static void fake_barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags dep,
                         uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni,
                         const VkImageMemoryBarrier *i)
{
    (void)cb, (void)src, (void)dst, (void)dep, (void)nm, (void)m, (void)nb, (void)b, (void)ni, (void)i;
}

static void fake_clear(VkCommandBuffer cb, uint32_t na, const VkClearAttachment *a, uint32_t nr, const VkClearRect *r)
{
    (void)cb;
    calls.clears++;
    if (na)
        calls.att = a[0];
    if (nr)
        calls.rect = r[0];
}

typedef struct fixture {
    vkf_fns saved;
    nvn_gfx st;
    replay r;
    vimg a;
    nvn_cmdset *s;
} fixture;

/** @brief A replay over one 64x32 color target whose command set holds color (16 bytes) at offset 0. */
static void setup(fixture *f, const void *color)
{
    memset(f, 0, sizeof *f);
    memset(&calls, 0, sizeof calls);
    f->saved = g.fns;
    g.fns.f[VKG_vkCmdBeginRendering] = (void (*)(void))fake_begin;
    g.fns.f[VKG_vkCmdEndRendering] = (void (*)(void))fake_end;
    g.fns.f[VKG_vkCmdClearAttachments] = (void (*)(void))fake_clear;
    g.fns.f[VKG_vkCmdPipelineBarrier] = (void (*)(void))fake_barrier;
    f->a.d.width = 64;
    f->a.d.height = 32;
    f->a.d.target = 1;
    f->a.d.levels = 1;
    f->st.ncolor = 1;
    f->st.color[0] = (rt){ .v = &f->a, .view = (VkImageView)(uintptr_t)0x10 };
    f->s = calloc(1, sizeof *f->s);
    f->s->bytes = malloc(16);
    f->s->nbytes = 16;
    memcpy(f->s->bytes, color, 16);
    f->r.st = &f->st;
    f->r.s = f->s;
    nvn_vk_assume_recording(true);
}

static void teardown(fixture *f)
{
    nvn_vk_end_rendering();
    nvn_vk_assume_recording(false);
    g.fns = f->saved;
    free(f->s->bytes);
    free(f->s);
}

TEST(nvn_integer_color_clears_reach_the_host_clear_unchanged)
{
    static const uint32_t ops[] = { NVN_CMD_CLEAR_COLOR, NVN_CMD_CLEAR_COLORI, NVN_CMD_CLEAR_COLORUI };
    const int32_t sint[4] = { -7, 0x7fffffff, (int32_t)0x80000000, 3 };
    for (unsigned i = 0; i < 3; i++) {
        static fixture f;
        setup(&f, sint);
        nvn_cmd e = { .op = ops[i], .x = { 0, 0, NVN_SNAP | 0, 0xf } };
        CHECK_EQ(nvn_vk_cmd(&f.r, &e), RL_OK);
        CHECK_EQ(calls.clears, 1u);
        CHECK_EQ(calls.att.aspectMask, VK_IMAGE_ASPECT_COLOR_BIT);
        CHECK_EQ(calls.att.colorAttachment, 0u);
        CHECK_EQ(calls.att.clearValue.color.int32[0], -7);
        CHECK_EQ(calls.att.clearValue.color.int32[1], 0x7fffffff);
        CHECK_EQ(calls.att.clearValue.color.uint32[2], 0x80000000u);
        CHECK_EQ(calls.att.clearValue.color.uint32[3], 3u);
        CHECK_EQ(calls.rect.rect.extent.width, 64u);
        CHECK_EQ(calls.rect.rect.extent.height, 32u);
        teardown(&f);
    }
}

TEST(nvn_integer_color_clear_with_no_channels_does_nothing)
{
    static fixture f;
    const uint32_t v[4] = { 1, 2, 3, 4 };
    setup(&f, v);
    nvn_cmd e = { .op = NVN_CMD_CLEAR_COLORUI, .x = { 0, 0, NVN_SNAP | 0, 0 } };
    CHECK_EQ(nvn_vk_cmd(&f.r, &e), RL_OK);
    CHECK_EQ(calls.clears, 0u);
    teardown(&f);
}
