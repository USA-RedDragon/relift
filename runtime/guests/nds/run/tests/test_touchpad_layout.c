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
        { 256, 384, true }, { 256, 384, false }, { 512, 192, true }, { 768, 384, true },
        { 512, 576, true }, { 256, 192, true },  { 256, 192, false },
    };
    CHECK_EQ(tpm_run(rl_nds_touchpad(), pics, sizeof pics / sizeof pics[0]), 0);
}
