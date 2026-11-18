/**
 * @file test_touchpad_layout.c
 * @brief The GBA on-screen controller over the matrix of view sizes,
 * densities and orientations: nothing overlaps, everything fits.
 */
#include "gba.h"
#include "touchpad_matrix.h"

TEST(the_gba_controller_fits_every_view_without_overlaps)
{
    static const tpm_picture pics[] = { { 240, 160, true }, { 240, 160, false } };
    CHECK_EQ(tpm_run(rl_gba_touchpad(), pics, sizeof pics / sizeof pics[0]), 0);
}
