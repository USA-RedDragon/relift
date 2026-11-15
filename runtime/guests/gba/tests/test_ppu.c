/**
 * @file test_ppu.c
 * @brief The picture unit on small hand-built scenes: backgrounds of every
 * kind, sprites, windows, blending, mosaic, register values.
 */
#include "ppu/ppu.h"
#include "runner.h"

#include <stdlib.h>
#include <string.h>

typedef struct scene {
    gba_ppu p;
    uint8_t pram[0x400], vram[0x18000], oam[0x400];
} scene;

static scene *scene_new(void)
{
    scene *s = calloc(1, sizeof *s);
    gba_ppu_reset(&s->p, s->pram, s->vram, s->oam);
    for (unsigned i = 0; i < 128; i++)
        s->oam[i * 8 + 1] = 0x02;
    return s;
}

static void io16(scene *s, uint32_t off, uint16_t v)
{
    gba_ppu_write(&s->p, 0x04000000u + off, (uint8_t)v);
    gba_ppu_write(&s->p, 0x04000000u + off + 1, (uint8_t)(v >> 8));
}

static void pal(scene *s, unsigned i, uint16_t c)
{
    s->pram[i * 2] = (uint8_t)c;
    s->pram[i * 2 + 1] = (uint8_t)(c >> 8);
}

static void v16(scene *s, uint32_t off, uint16_t v)
{
    s->vram[off] = (uint8_t)v;
    s->vram[off + 1] = (uint8_t)(v >> 8);
}

static void frames(scene *s, unsigned n)
{
    for (unsigned f = 0; f < n; f++)
        for (unsigned y = 0; y < GBA_PPU_LINES; y++) {
            gba_ppu_line_start(&s->p, y);
            gba_ppu_render(&s->p, y);
        }
}

static unsigned px(const scene *s, unsigned x, unsigned y) { return s->p.pixels[y * GBA_PPU_W + x]; }

enum { RED = 0x001F, GREEN = 0x03E0, BLUE = 0x7C00, WHITE = 0x7FFF };

static void solid_tile4(scene *s, unsigned base, unsigned tile, unsigned index)
{
    for (unsigned i = 0; i < 32; i++)
        s->vram[base + tile * 32 + i] = (uint8_t)(index | index << 4);
}

TEST(registers_read_back_and_unreadable_ones_read_open_bus)
{
    scene *s = scene_new();
    io16(s, 0x00, 0x1F45);
    CHECK_EQ(gba_ppu_read(&s->p, 0x04000000u), 0x45);
    CHECK_EQ(gba_ppu_read(&s->p, 0x04000001u), 0x1F);
    io16(s, 0x0C, 0xDFFF);
    CHECK_EQ(gba_ppu_read(&s->p, 0x0400000Cu), 0xFF);
    CHECK_EQ(gba_ppu_read(&s->p, 0x0400000Du), 0xDF);
    io16(s, 0x08, 0xFFFF);
    CHECK_EQ(gba_ppu_read(&s->p, 0x04000009u), 0xDF);
    CHECK_EQ(gba_ppu_read(&s->p, 0x04000010u), -1);
    CHECK_EQ(gba_ppu_read(&s->p, 0x04000020u), -1);
    CHECK_EQ(gba_ppu_read(&s->p, 0x0400004Cu), -1);
    CHECK_EQ(gba_ppu_read(&s->p, 0x04000054u), -1);
    io16(s, 0x50, 0x3FFF);
    CHECK_EQ(gba_ppu_read(&s->p, 0x04000050u), 0xFF);
    CHECK_EQ(gba_ppu_read(&s->p, 0x04000051u), 0x3F);
    io16(s, 0x52, 0x1F1F);
    CHECK_EQ(gba_ppu_read(&s->p, 0x04000052u), 0x1F);
    free(s);
}

TEST(forced_blank_draws_white)
{
    scene *s = scene_new();
    io16(s, 0x00, 0x0080);
    frames(s, 1);
    CHECK_EQ(px(s, 0, 0), WHITE);
    CHECK_EQ(px(s, 239, 159), WHITE);
    free(s);
}

TEST(the_backdrop_is_palette_entry_zero)
{
    scene *s = scene_new();
    pal(s, 0, GREEN);
    io16(s, 0x00, 0x0000);
    frames(s, 1);
    CHECK_EQ(px(s, 100, 80), GREEN);
    free(s);
}

TEST(text_background_with_tile_flip_and_scroll)
{
    scene *s = scene_new();
    pal(s, 1, RED);
    pal(s, 2, BLUE);
    for (unsigned r = 0; r < 8; r++) {
        s->vram[32 + r * 4 + 0] = 0x11;
        s->vram[32 + r * 4 + 1] = 0x11;
        s->vram[32 + r * 4 + 2] = 0x22;
        s->vram[32 + r * 4 + 3] = 0x22;
    }
    v16(s, 0x1000, 0x0001);
    v16(s, 0x1002, 0x0401);
    io16(s, 0x08, (2 << 8));
    io16(s, 0x00, 0x0100);
    frames(s, 4);
    CHECK_EQ(px(s, 0, 0), RED);
    CHECK_EQ(px(s, 4, 0), BLUE);
    CHECK_EQ(px(s, 7, 0), BLUE);
    CHECK_EQ(px(s, 8, 0), BLUE);
    CHECK_EQ(px(s, 11, 0), BLUE);
    CHECK_EQ(px(s, 12, 0), RED);
    CHECK_EQ(px(s, 15, 0), RED);
    CHECK_EQ(px(s, 16, 0), 0u);
    io16(s, 0x10, 3);
    frames(s, 1);
    CHECK_EQ(px(s, 0, 0), RED);
    CHECK_EQ(px(s, 1, 0), BLUE);
    free(s);
}

TEST(an_enabled_background_appears_two_lines_late)
{
    scene *s = scene_new();
    pal(s, 1, RED);
    solid_tile4(s, 0, 1, 1);
    for (unsigned i = 0; i < 1024; i++)
        v16(s, 0x800 * 2 + i * 2, 1);
    io16(s, 0x08, (2 << 8));
    frames(s, 1);
    io16(s, 0x00, 0x0100);
    frames(s, 1);
    CHECK_EQ(px(s, 5, 0), 0u);
    CHECK_EQ(px(s, 5, 1), 0u);
    CHECK_EQ(px(s, 5, 2), RED);
    free(s);
}

TEST(bitmap_modes_three_four_and_five)
{
    scene *s = scene_new();
    io16(s, 0x20, 0x100);
    io16(s, 0x26, 0x100);
    v16(s, 0, 0x1234);
    v16(s, 2 * 240 + 2 * 3, 0x0C63);
    io16(s, 0x00, 0x0403);
    frames(s, 2);
    CHECK_EQ(px(s, 0, 0), 0x1234u);
    CHECK_EQ(px(s, 3, 1), 0x0C63u);
    memset(s->vram, 0, 0x18000);
    pal(s, 7, GREEN);
    s->vram[5] = 7;
    s->vram[0xA000 + 9] = 7;
    io16(s, 0x00, 0x0404);
    frames(s, 2);
    CHECK_EQ(px(s, 5, 0), GREEN);
    CHECK_EQ(px(s, 9, 0), 0u);
    io16(s, 0x00, 0x0414);
    frames(s, 2);
    CHECK_EQ(px(s, 9, 0), GREEN);
    CHECK_EQ(px(s, 5, 0), 0u);
    memset(s->vram, 0, 0x18000);
    v16(s, 2 * 159, 0x7000);
    io16(s, 0x00, 0x0405);
    frames(s, 2);
    CHECK_EQ(px(s, 159, 0), 0x7000u);
    CHECK_EQ(px(s, 160, 0), 0u);
    CHECK_EQ(px(s, 0, 128), 0u);
    free(s);
}

TEST(affine_background_scales)
{
    scene *s = scene_new();
    pal(s, 1, RED);
    pal(s, 2, GREEN);
    memset(&s->vram[64], 1, 64);
    memset(&s->vram[128], 2, 64);
    s->vram[0x4000] = 1;
    s->vram[0x4001] = 2;
    io16(s, 0x0C, (8 << 8) | 0x2000);
    io16(s, 0x20, 0x100);
    io16(s, 0x26, 0x100);
    io16(s, 0x00, 0x0401);
    frames(s, 2);
    CHECK_EQ(px(s, 0, 0), RED);
    CHECK_EQ(px(s, 8, 0), GREEN);
    CHECK_EQ(px(s, 16, 0), 0u);
    io16(s, 0x20, 0x80);
    frames(s, 2);
    CHECK_EQ(px(s, 15, 0), RED);
    CHECK_EQ(px(s, 16, 0), GREEN);
    free(s);
}

TEST(a_regular_sprite_draws_its_tile)
{
    scene *s = scene_new();
    pal(s, 0, BLUE);
    pal(s, 256 + 1, RED);
    for (unsigned i = 0; i < 32; i++)
        s->vram[0x10000 + 32 + i] = 0x11;
    v16(s, 0x10000 + 0, 0);
    s->oam[0] = 10;
    s->oam[1] = 0x00;
    s->oam[2] = 20;
    s->oam[3] = 0x00;
    s->oam[4] = 1;
    s->oam[5] = 0x00;
    io16(s, 0x00, 0x1040);
    frames(s, 2);
    CHECK_EQ(px(s, 20, 10), RED);
    CHECK_EQ(px(s, 27, 17), RED);
    CHECK_EQ(px(s, 28, 10), BLUE);
    CHECK_EQ(px(s, 20, 9), BLUE);
    CHECK_EQ(px(s, 20, 18), BLUE);
    free(s);
}

TEST(a_window_selects_the_layers_inside_it)
{
    scene *s = scene_new();
    pal(s, 0, BLUE);
    pal(s, 1, RED);
    solid_tile4(s, 0, 1, 1);
    for (unsigned i = 0; i < 1024; i++)
        v16(s, 0x800 * 2 + i * 2, 1);
    io16(s, 0x08, (2 << 8));
    io16(s, 0x40, (10 << 8) | 20);
    io16(s, 0x44, (10 << 8) | 20);
    io16(s, 0x48, 0x0000);
    io16(s, 0x4A, 0x0001);
    io16(s, 0x00, 0x2100);
    frames(s, 4);
    CHECK_EQ(px(s, 5, 5), RED);
    CHECK_EQ(px(s, 15, 15), BLUE);
    CHECK_EQ(px(s, 15, 25), RED);
    CHECK_EQ(px(s, 25, 15), RED);
    free(s);
}

TEST(blend_modes_alpha_brighten_darken)
{
    scene *s = scene_new();
    pal(s, 0, 0x0000);
    pal(s, 1, 0x0010);
    solid_tile4(s, 0, 1, 1);
    for (unsigned i = 0; i < 1024; i++)
        v16(s, 0x1000 + i * 2, 1);
    io16(s, 0x08, (2 << 8));
    io16(s, 0x00, 0x0100);
    io16(s, 0x52, 0x0808);
    io16(s, 0x54, 8);
    io16(s, 0x50, 0x2041);
    frames(s, 4);
    CHECK_EQ(px(s, 3, 3), 8u);
    io16(s, 0x50, 0x00C1);
    frames(s, 1);
    CHECK_EQ(px(s, 3, 3), 8u);
    io16(s, 0x50, 0x0081);
    frames(s, 1);
    CHECK_EQ(px(s, 3, 3), 24u | 16u << 5 | 16u << 10);
    free(s);
}

TEST(the_frame_hash_is_fnv_one_a_over_the_halfwords)
{
    scene *s = scene_new();
    uint64_t h = 0xCBF29CE484222325ull;
    for (unsigned i = 0; i < 2 * GBA_PPU_W * GBA_PPU_H; i++)
        h = (h ^ 0) * 0x100000001B3ull;
    CHECK_EQ(gba_ppu_hash(&s->p), h);
    s->p.pixels[0] = 0x1234;
    h = 0xCBF29CE484222325ull;
    h = (h ^ 0x34) * 0x100000001B3ull;
    h = (h ^ 0x12) * 0x100000001B3ull;
    for (unsigned i = 2; i < 2 * GBA_PPU_W * GBA_PPU_H; i++)
        h = (h ^ 0) * 0x100000001B3ull;
    CHECK_EQ(gba_ppu_hash(&s->p), h);
    free(s);
}
