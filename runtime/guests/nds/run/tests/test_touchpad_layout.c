/**
 * @file test_touchpad_layout.c
 * @brief The DS on-screen controller over the matrix of view sizes,
 * densities and orientations, for each shape the pictures take (stacked,
 * side by side, one large and one small, one screen as on a first display
 * with a second beside it): nothing overlaps, everything fits.
 */
#include "play.h"
#include "touchpad_matrix.h"

TEST(the_nds_controller_fits_every_view_without_overlaps)
{
    static const tpm_picture pics[] = {
        { 256, 384, false }, { 256, 384, true }, { 512, 192, true }, { 768, 384, true },
        { 512, 576, true }, { 256, 192, true },  { 256, 192, false },
    };
    CHECK_EQ(tpm_run(rl_nds_touchpad(), pics, sizeof pics / sizeof pics[0]), 0);
}

static void preview(const char *dir, const tpm_view *v, rl_screens_layout layout, const char *tag)
{
    static const uint32_t sizes[RL_SCREENS][2] = { { 256, 192 }, { 256, 192 } };
    tpm_picture pic = { 256, 384, false };
    tpm_setup(rl_nds_touchpad(), v, &pic);
    uint32_t rw = v->w, rh = v->h;
    bool room = v->w > v->h && rl_touchpad_room(&rw, &rh);
    rl_screens_picture plan;
    rl_screens_layout l = rl_screens_resolve(sizes, layout, rw, rh, !room);
    rl_screens_plan(sizes, l, -1, v->h > v->w, &plan);
    rl_touchpad_set_picture(plan.width, plan.height, false);
    rl_touchpad_view(v->w, v->h, v->ppd, v->safe);
    tpm_draw(dir, "nds", v, &pic, tag, &plan);
}

TEST(the_nds_controller_previews_the_layouts_the_screens_take)
{
    const char *dir = getenv("RL_TOUCHPAD_PNG");
    if (!dir)
        return;
    for (size_t i = 0; i < sizeof g_tpm_views / sizeof g_tpm_views[0]; i++) {
        const tpm_view *v = &g_tpm_views[i];
        preview(dir, v, RL_SCREENS_AUTO, "plan-auto");
        preview(dir, v, RL_SCREENS_SIDE_BY_SIDE, "plan-side");
        preview(dir, v, RL_SCREENS_STACKED, "plan-stacked");
    }
    rl_touchpad_reset();
}
