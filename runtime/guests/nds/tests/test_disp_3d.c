/**
 * @file test_disp_3d.c
 * @brief The 3D layer's alpha in the 2D merge, from GBATEK ("DS 3D Display
 * Control": blending with BG0 as 1st target goes by the 3D alpha values, not
 * BLDALPHA): a pixel of alpha 31 is solid, so a game's black curtain made of
 * one opaque polygon hides the layers behind it whatever EVA and EVB are.
 */
#include "ndst.h"
#include "nds_int.h"

#define POWCNT1 0x04000304u
#define DISPCNT 0x04000000u
#define BLDCNT 0x04000050u
#define BLDALPHA 0x04000052u
#define PAL_A 0x05000000u

typedef struct curtain {
    uint32_t pixel;
} curtain;

static bool curtain_line(void *user, unsigned y, uint32_t *out)
{
    (void)y;
    const curtain *c = user;
    for (unsigned x = 0; x < NDS_W; x++)
        out[x] = c->pixel;
    return true;
}

/** @brief The 18-bit color of engine A's first pixel after two frames, with the 3D layer of the given pixel over a white backdrop and BLDCNT/BLDALPHA blending BG0 with the backdrop. */
static uint32_t shown(uint32_t pixel)
{
    ndst *t = ndst_new();
    curtain c = {pixel};
    nds_disp_set_3d(t->m, curtain_line, &c);
    w9(t, POWCNT1, 2, 0x8003);
    w9(t, DISPCNT, 4, 0x00010108);
    w9(t, BLDCNT, 2, 0x2041);
    w9(t, BLDALPHA, 2, 0x0808);
    w9(t, PAL_A, 2, 0x7FFF);
    uint64_t frames = rl_nds_frames(t->m);
    while (rl_nds_frames(t->m) < frames + 2) {
        uint64_t now = rl_sched_now(t->k[RL_NDS_ARM9]);
        ndst_both(t, now + RL_NDS_LINE_TICKS);
    }
    return rl_nds_frame(t->m)[0] & 0x3FFFFu;
}

TEST(a_solid_3d_pixel_hides_the_layer_behind_it_whatever_bldalpha_says)
{
    CHECK_EQ(shown(0x1F000000u), 0u);
}

TEST(a_translucent_3d_pixel_blends_by_its_own_alpha)
{
    CHECK_EQ(shown(0x10000000u) != 0u, true);
    CHECK_EQ(shown(0x10000000u) != 0x3FFFFu, true);
}
