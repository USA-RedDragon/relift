#include "ppu/ppu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCN_LEN (8 + 1024 + 1024 + 0x18000 + 1024)

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: ppu_scene SCENE OUT [FRAMES [DOT]]\n");
        return 2;
    }
    static uint8_t buf[SCN_LEN];
    FILE *f = fopen(argv[1], "rb");
    if (!f || fread(buf, 1, SCN_LEN, f) != SCN_LEN || memcmp(buf, "GBASCN01", 8)) {
        fprintf(stderr, "bad scene %s\n", argv[1]);
        return 2;
    }
    fclose(f);
    static gba_ppu p;
    gba_ppu_reset(&p, buf + 1032, buf + 2056, buf + 100360);
    for (unsigned a = 0; a < 0x60; a++)
        if (a < 4 || a >= 8)
            gba_ppu_write(&p, 0x04000000u + a, buf[8 + a]);
    int frames = argc > 3 ? atoi(argv[3]) : 4;
    for (int n = 0; n < frames; n++)
        for (unsigned y = 0; y < GBA_PPU_LINES; y++) {
            gba_ppu_line_start(&p, y);
            gba_ppu_render(&p, y);
        }
    f = fopen(argv[2], "wb");
    fwrite(p.pixels, 2, GBA_PPU_W * GBA_PPU_H, f);
    fclose(f);
    return 0;
}
