/**
 * @file test_ppu_dev.c
 * @brief The picture unit on the machine: registers through the bus,
 * lazy drawing before writes to video memory, the frame event and hook,
 * the frame hash.
 */
#include "mach.h"
#include "ppu/ppu.h"

typedef struct frames {
    unsigned n;
    uint64_t when[4];
    uint16_t first;
} frames;

static void on_frame(void *user, const uint16_t *pixels, uint64_t when)
{
    frames *f = user;
    if (f->n < 4)
        f->when[f->n] = when;
    f->first = pixels[0];
    f->n++;
}

static void bg_scene(mach *m)
{
    rl_gba_write(m->g, 0x05000000u, 2, 0x0000);
    rl_gba_write(m->g, 0x05000002u, 2, 0x001F);
    for (unsigned i = 0; i < 8; i++)
        rl_gba_write(m->g, 0x06000020u + i * 4, 4, 0x11111111u);
    for (unsigned i = 0; i < 1024; i++)
        rl_gba_write(m->g, 0x06001000u + i * 2, 2, 1);
    rl_gba_write(m->g, 0x04000008u, 2, 2u << 8);
    rl_gba_write(m->g, 0x04000000u, 2, 0x0100);
}

TEST(frames_end_at_the_start_of_vblank_and_reach_the_hook)
{
    mach m = mach_new();
    frames f = {0};
    rl_gba_set_frame_hook(m.g, on_frame, &f);
    CHECK_EQ(rl_gba_frame(m.g) == NULL, 1u);
    mach_to(&m, 160u * RL_GBA_LINE_CYCLES - 1);
    CHECK_EQ(f.n, 0u);
    mach_to(&m, 160u * RL_GBA_LINE_CYCLES);
    CHECK_EQ(f.n, 1u);
    CHECK_EQ(f.when[0], 160u * RL_GBA_LINE_CYCLES);
    mach_to(&m, 3u * RL_GBA_FRAME_CYCLES);
    CHECK_EQ(f.n, 3u);
    CHECK_EQ(f.when[2], 2u * RL_GBA_FRAME_CYCLES + 160u * RL_GBA_LINE_CYCLES);
    CHECK_EQ(rl_gba_frames(m.g), 3u);
    mach_free(&m);
}

TEST(a_background_drawn_through_the_bus_shows_in_the_frame)
{
    mach m = mach_new();
    bg_scene(&m);
    mach_to(&m, 3u * RL_GBA_FRAME_CYCLES);
    const uint16_t *px = rl_gba_frame(m.g);
    CHECK(px != NULL);
    CHECK_EQ(rl_gba_frames(m.g), 3u);
    CHECK_EQ(px[10 * 240 + 10], 0x001Fu);
    CHECK_EQ(px[100 * 240 + 200], 0x001Fu);
    uint64_t h = rl_gba_frame_hash(m.g);
    mach_to(&m, 4u * RL_GBA_FRAME_CYCLES);
    CHECK_EQ(rl_gba_frame_hash(m.g), h);
    mach_free(&m);
}

TEST(a_palette_write_after_the_render_dot_shows_in_the_next_line_only)
{
    mach m = mach_new();
    bg_scene(&m);
    mach_to(&m, 2u * RL_GBA_FRAME_CYCLES);
    mach_to(&m, 2u * RL_GBA_FRAME_CYCLES + 20u * RL_GBA_LINE_CYCLES + 100u);
    rl_gba_write(m.g, 0x05000002u, 2, 0x03E0);
    mach_to(&m, 3u * RL_GBA_FRAME_CYCLES);
    const uint16_t *px = rl_gba_frame(m.g);
    CHECK_EQ(px[19 * 240 + 50], 0x001Fu);
    CHECK_EQ(px[20 * 240 + 50], 0x001Fu);
    CHECK_EQ(px[21 * 240 + 50], 0x03E0u);
    mach_free(&m);
}

TEST(a_palette_write_before_the_render_dot_shows_in_the_same_line)
{
    mach m = mach_new();
    bg_scene(&m);
    mach_to(&m, 2u * RL_GBA_FRAME_CYCLES);
    mach_to(&m, 2u * RL_GBA_FRAME_CYCLES + 20u * RL_GBA_LINE_CYCLES + 10u);
    rl_gba_write(m.g, 0x05000002u, 2, 0x03E0);
    mach_to(&m, 3u * RL_GBA_FRAME_CYCLES);
    const uint16_t *px = rl_gba_frame(m.g);
    CHECK_EQ(px[19 * 240 + 50], 0x001Fu);
    CHECK_EQ(px[20 * 240 + 50], 0x03E0u);
    mach_free(&m);
}

TEST(the_render_dot_can_move_to_the_start_of_hblank)
{
    mach m = mach_new();
    rl_gba_set_render_dot(m.g, 1006);
    bg_scene(&m);
    mach_to(&m, 2u * RL_GBA_FRAME_CYCLES);
    mach_to(&m, 2u * RL_GBA_FRAME_CYCLES + 20u * RL_GBA_LINE_CYCLES + 500u);
    rl_gba_write(m.g, 0x05000002u, 2, 0x03E0);
    mach_to(&m, 3u * RL_GBA_FRAME_CYCLES);
    const uint16_t *px = rl_gba_frame(m.g);
    CHECK_EQ(px[19 * 240 + 50], 0x001Fu);
    CHECK_EQ(px[20 * 240 + 50], 0x03E0u);
    mach_free(&m);
}

TEST(ppu_registers_read_through_the_io_map)
{
    mach m = mach_new();
    rl_gba_write(m.g, 0x04000000u, 2, 0x1F45);
    CHECK_EQ(rl_gba_read(m.g, 0x04000000u, 2), 0x1F45u);
    rl_gba_write(m.g, 0x04000050u, 2, 0x3FFF);
    CHECK_EQ(rl_gba_read(m.g, 0x04000050u, 2), 0x3FFFu);
    rl_gba_write(m.g, 0x04000010u, 2, 0x1FF);
    rl_gba_set_open_bus(m.g, 0xAABBCCDDu);
    CHECK_EQ(rl_gba_read(m.g, 0x04000010u, 1), 0xDDu);
    mach_free(&m);
}

TEST(moving_the_display_moves_the_frame_event)
{
    mach m = mach_new();
    frames f = {0};
    rl_gba_set_frame_hook(m.g, on_frame, &f);
    mach_to(&m, 1000);
    rl_gba_lcd_set(m.g, 100, 0);
    mach_to(&m, 1000 + 60u * RL_GBA_LINE_CYCLES - 1);
    CHECK_EQ(f.n, 0u);
    mach_to(&m, 1000 + 60u * RL_GBA_LINE_CYCLES);
    CHECK_EQ(f.n, 1u);
    mach_free(&m);
}

TEST(frames_expand_to_rgb_by_repeating_the_top_bits)
{
    uint16_t px[GBA_PPU_W * GBA_PPU_H] = {0x7FFF, 0x001F, 0x03E0, 0x7C00, 0x0421};
    static uint8_t rgb[GBA_PPU_W * GBA_PPU_H * 3];
    rl_gba_frame_rgb(px, rgb);
    CHECK_EQ(rgb[0] | rgb[1] << 8 | rgb[2] << 16, 0xFFFFFFu);
    CHECK_EQ(rgb[3] | rgb[4] << 8 | rgb[5] << 16, 0x0000FFu);
    CHECK_EQ(rgb[6] | rgb[7] << 8 | rgb[8] << 16, 0x00FF00u);
    CHECK_EQ(rgb[9] | rgb[10] << 8 | rgb[11] << 16, 0xFF0000u);
    CHECK_EQ(rgb[12] | rgb[13] << 8 | rgb[14] << 16, 0x080808u);
}

TEST(presenting_without_a_backend_reports_false)
{
    uint16_t px[GBA_PPU_W * GBA_PPU_H] = {0};
    CHECK_EQ(rl_gba_present(NULL, px), 0u);
}
