/**
 * @file test_render.c
 * @brief The render log: a toy line renderer fed through rl_render draws the
 * same frames in sync and async mode, with a small log that makes the putter
 * wait and wrap, a feedback read after a drain, and a switch between modes.
 */
#include "runner.h"

#include <relift/render.h>

#include <string.h>

enum { W = 64, H = 48, FRAMES = 40 };
enum { K_POKE = 1, K_FILL = 2, K_LINE = 3, K_FRAME = 4 };

/** @brief The toy's mirror: a tile memory, a scroll register, the frame being drawn and the finished frames' hashes. */
typedef struct toy {
    uint8_t mem[4096];
    uint32_t scroll;
    uint32_t frame[H][W];
    uint64_t hashes[FRAMES];
    unsigned frames;
} toy;

typedef struct poke {
    uint32_t off, len;
} poke;

static void apply(void *user, uint32_t kind, const void *data, uint32_t len)
{
    toy *t = user;
    if (kind == K_POKE) {
        poke p;
        memcpy(&p, data, sizeof p);
        memcpy(t->mem + p.off, (const uint8_t *)data + sizeof p, p.len);
    } else if (kind == K_FILL) {
        memcpy(&t->scroll, data, sizeof t->scroll);
    } else if (kind == K_LINE) {
        uint32_t y;
        memcpy(&y, data, sizeof y);
        for (unsigned x = 0; x < W; x++)
            t->frame[y][x] = t->mem[(y * 37u + x + t->scroll) % sizeof t->mem] * 0x10101u ^ y;
    } else if (kind == K_FRAME) {
        uint64_t h = 0xCBF29CE484222325ull;
        for (unsigned y = 0; y < H; y++)
            for (unsigned x = 0; x < W; x++)
                h = (h ^ t->frame[y][x]) * 0x100000001B3ull;
        t->hashes[t->frames++] = h;
    }
    (void)len;
}

/** @brief The run side: changes the memory and the scroll between lines, as a guest's CPU would. */
static void run(rl_render *r, toy *primary, unsigned frames, bool switch_modes)
{
    uint32_t seed = 12345;
    for (unsigned f = 0; f < frames; f++) {
        if (switch_modes)
            rl_render_set_sync(r, f % 3 == 1);
        for (uint32_t y = 0; y < H; y++) {
            for (unsigned k = 0; k < 3; k++) {
                seed = seed * 1103515245u + 12345u;
                uint32_t off = (seed >> 8) % (sizeof primary->mem - 200), n = 1 + (seed >> 24) % 200;
                for (unsigned i = 0; i < n; i++)
                    primary->mem[off + i] = (uint8_t)(seed + i * 7u);
                uint8_t *p = rl_render_begin(r, K_POKE, (uint32_t)(sizeof(poke) + n));
                memcpy(p, &(poke){off, n}, sizeof(poke));
                memcpy(p + sizeof(poke), primary->mem + off, n);
                rl_render_commit(r);
            }
            if (y % 16 == 0) {
                primary->scroll += y + f;
                rl_render_put(r, K_FILL, &primary->scroll, sizeof primary->scroll);
            }
            rl_render_put(r, K_LINE, &y, sizeof y);
            if (y % 8 == 0)
                rl_render_flush(r);
        }
        rl_render_put(r, K_FRAME, NULL, 0);
    }
}

static toy g_sync, g_async, g_mixed, g_primary;

static void frames_of(toy *t, size_t log_bytes, bool sync, bool switch_modes)
{
    memset(t, 0, sizeof *t);
    memset(&g_primary, 0, sizeof g_primary);
    rl_render *r = rl_render_create(&(rl_render_config){.apply = apply, .user = t, .log_bytes = log_bytes, .sync = sync});
    CHECK(r != NULL);
    run(r, &g_primary, FRAMES, switch_modes);
    rl_render_drain(r);
    CHECK_EQ(t->frames, FRAMES);
    CHECK(memcmp(t->mem, g_primary.mem, sizeof t->mem) == 0);
    rl_render_free(r);
}

TEST(render_async_draws_what_sync_draws)
{
    frames_of(&g_sync, 0, true, false);
    frames_of(&g_async, 4096, false, false);
    frames_of(&g_mixed, 8192, false, true);
    for (unsigned f = 0; f < FRAMES; f++) {
        CHECK_EQ(g_async.hashes[f], g_sync.hashes[f]);
        CHECK_EQ(g_mixed.hashes[f], g_sync.hashes[f]);
    }
}

TEST(render_drain_leaves_nothing_pending)
{
    static toy t;
    memset(&t, 0, sizeof t);
    rl_render *r = rl_render_create(&(rl_render_config){.apply = apply, .user = &t, .log_bytes = 4096});
    CHECK(r != NULL);
    rl_render_set_sync(r, false);
    CHECK(!rl_render_is_sync(r));
    CHECK(rl_render_max_record(4096) < 4096);
    for (uint32_t y = 0; y < H; y++)
        rl_render_put(r, K_LINE, &y, sizeof y);
    rl_render_put(r, K_FRAME, NULL, 0);
    rl_render_drain(r);
    CHECK_EQ(t.frames, 1);
    rl_render_free(r);
}
