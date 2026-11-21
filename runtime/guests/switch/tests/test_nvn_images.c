/**
 * @file test_nvn_images.c
 * @brief The lifetime of the layer's host images: an image whose memory pool the game removed leaves the images a
 * texture can find or have written back over it (no host device needed: the images are only listed).
 */
#include "nvn/nvn_priv.h"
#include "nvn/nvn_vk.h"
#include "tc.h"

#include <relift/ports.h>

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
