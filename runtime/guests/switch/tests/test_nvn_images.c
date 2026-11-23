/**
 * @file test_nvn_images.c
 * @brief The lifetime of the layer's host images: an image whose memory pool the game removed leaves the images a
 * texture can find or have written back over it (no host device needed: the images are only listed).
 */
#include "nvn/nvn_priv.h"
#include "nvn/nvn_vk.h"
#include "tc.h"

#include <relift/ports.h>

#include <stdlib.h>

#define RET 0x7000000ull

static uint64_t call(rl_ctx *c, rl_fn_t *fn, uint64_t x0)
{
    c->x[0] = x0;
    c->x[30] = RET;
    return fn(c, t_mem());
}

/** @brief An image over the pool at pool, as image_of makes one, added to the device's images. */
static void add_image(vimg *v, uint64_t pool, uint64_t storage)
{
    v->storage = storage;
    v->d.pool = pool;
    v->pool_serial = nvn_obj_serial(pool, NVN_POOL);
    if (g.nimages == g.capimages) {
        uint32_t cap = g.capimages ? 2 * g.capimages : 16;
        vimg **p = realloc(g.images, cap * sizeof *p);
        if (!p)
            return;
        g.images = p;
        g.capimages = cap;
    }
    g.images[g.nimages++] = v;
}

static bool listed(const vimg *v)
{
    for (uint32_t i = 0; i < g.nimages; i++)
        if (g.images[i] == v)
            return true;
    return false;
}

TEST(nvn_images_of_a_finalized_pool_are_no_longer_listed)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000), a = heap + 0x100, b = heap + 0x200;
    CHECK(nvn_pool_make(a, heap + 0x10000, 0x10000, 0));
    CHECK(nvn_pool_make(b, heap + 0x20000, 0x10000, 0));
    vimg in_a = { 0 }, in_b = { 0 };
    add_image(&in_a, a, heap + 0x10000);
    add_image(&in_b, b, heap + 0x20000);
    CHECK(in_a.pool_serial && in_b.pool_serial && in_a.pool_serial != in_b.pool_serial);
    nvn_vk_retire_images();
    CHECK(listed(&in_a) && listed(&in_b));
    CHECK_EQ(call(c, nvnh_nvnMemoryPoolFinalize, a), RET);
    nvn_vk_retire_images();
    CHECK(!listed(&in_a));
    CHECK(listed(&in_b));
}

TEST(nvn_images_of_a_pool_whose_address_holds_a_new_object_are_no_longer_listed)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000), a = heap + 0x100, b = heap + 0x200, keep = heap + 0x300;
    CHECK(nvn_pool_make(a, heap + 0x10000, 0x10000, 0));
    CHECK(nvn_pool_make(b, heap + 0x20000, 0x10000, 0));
    CHECK(nvn_pool_make(keep, heap + 0x30000, 0x10000, 0));
    vimg in_a = { 0 }, in_b = { 0 }, in_keep = { 0 };
    add_image(&in_a, a, heap + 0x10000);
    add_image(&in_b, b, heap + 0x20000);
    add_image(&in_keep, keep, heap + 0x30000);
    nvn_tex_desc d = { .pool = keep, .width = 4, .height = 4 };
    CHECK(nvn_texture_record(a, &d, NULL));
    CHECK(nvn_pool_make(b, heap + 0x20000, 0x10000, 0));
    nvn_vk_retire_images();
    CHECK(!listed(&in_a));
    CHECK(!listed(&in_b));
    CHECK(listed(&in_keep));
}

TEST(nvn_an_image_written_back_at_its_stamp_needs_no_new_write_back)
{
    vimg u = { .storage = 0x10000, .d = { .size = 0x4000 } };
    nvn_vk_written(&u);
    CHECK(!nvn_vk_wb_current(&u));
    nvn_vk_wb_note(&u);
    CHECK(nvn_vk_wb_current(&u));
    nvn_vk_written(&u);
    CHECK(!nvn_vk_wb_current(&u));
}

TEST(nvn_a_later_write_back_of_older_bytes_over_an_image_needs_its_write_back_again)
{
    vimg older = { .storage = 0x20000, .d = { .size = 0x2000 } };
    vimg u = { .storage = 0x21000, .d = { .size = 0x4000 } };
    vimg newer = { .storage = 0x22000, .d = { .size = 0x1000 } };
    vimg apart = { .storage = 0x40000, .d = { .size = 0x1000 } };
    nvn_vk_written(&older);
    nvn_vk_written(&apart);
    nvn_vk_written(&u);
    nvn_vk_written(&newer);
    nvn_vk_wb_note(&u);
    nvn_vk_wb_note(&newer);
    nvn_vk_wb_note(&apart);
    CHECK(nvn_vk_wb_current(&u));
    nvn_vk_wb_note(&older);
    CHECK(!nvn_vk_wb_current(&u));
    CHECK(nvn_vk_wb_current(&newer));
}

TEST(nvn_an_image_written_back_too_many_write_backs_ago_is_written_back_again)
{
    vimg u = { .storage = 0x60000, .d = { .size = 0x1000 } };
    vimg other = { .storage = 0x80000, .d = { .size = 0x1000 } };
    nvn_vk_written(&u);
    nvn_vk_written(&other);
    nvn_vk_wb_note(&u);
    for (int i = 0; i < 1000; i++)
        nvn_vk_wb_note(&other);
    CHECK(!nvn_vk_wb_current(&u));
}
