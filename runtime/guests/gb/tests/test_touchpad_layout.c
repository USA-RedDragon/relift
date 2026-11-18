/**
 * @file test_touchpad_layout.c
 * @brief The GB and GBC on-screen controller (one layout) over the matrix of
 * view sizes, densities and orientations: nothing overlaps, everything fits.
 */
#include "gb.h"
#include "touchpad_matrix.h"

TEST(the_gb_controller_fits_every_view_without_overlaps)
{
    static const tpm_picture pics[] = { { 160, 144, true }, { 160, 144, false } };
    CHECK_EQ(tpm_run(rl_gb_touchpad(), pics, sizeof pics / sizeof pics[0]), 0);
}
