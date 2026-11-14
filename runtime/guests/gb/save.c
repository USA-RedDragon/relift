/**
 * @file save.c
 * @brief Battery saves: the cartridge RAM, and the clock of an MBC3 board,
 * as bytes and as a host file.
 */
#include "gb_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t rl_gb_save_size(const rl_gb *g)
{
    if (!g->cart.battery)
        return 0;
    return g->ram_len + (g->cart.rtc ? RL_GB_SAVE_CLOCK_BYTES : 0);
}

bool rl_gb_save_dirty(const rl_gb *g) { return g->dirty; }

size_t rl_gb_save_bytes(rl_gb *g, uint8_t *out)
{
    size_t n = rl_gb_save_size(g);
    if (n == 0)
        return 0;
    if (g->ram_len)
        memcpy(out, g->ram, g->ram_len);
    if (g->cart.rtc)
        gb_rtc_record(g, out + g->ram_len);
    g->dirty = false;
    return n;
}

int rl_gb_load_bytes(rl_gb *g, const uint8_t *data, size_t len)
{
    if (!g->cart.battery)
        return -1;
    bool clock = g->cart.rtc;
    if (len != g->ram_len && !(clock && len == g->ram_len + RL_GB_SAVE_CLOCK_BYTES))
        return -1;
    if (g->ram_len)
        memcpy(g->ram, data, g->ram_len);
    if (g->cart.mbc == RL_GB_MBC2)
        for (size_t i = 0; i < g->ram_len; i++)
            g->ram[i] &= 0xF;
    if (clock && len > g->ram_len)
        gb_rtc_restore(g, data + g->ram_len);
    g->dirty = false;
    return 0;
}

int rl_gb_save(rl_gb *g, const char *path)
{
    size_t n = rl_gb_save_size(g);
    if (n == 0)
        return 1;
    uint8_t *buf = malloc(n);
    char *tmp = malloc(strlen(path) + 5);
    int rc = -1;
    if (buf && tmp) {
        rl_gb_save_bytes(g, buf);
        strcpy(tmp, path);
        strcat(tmp, ".tmp");
        FILE *f = fopen(tmp, "wb");
        if (f) {
            bool ok = fwrite(buf, 1, n, f) == n;
            ok = fclose(f) == 0 && ok;
#ifdef _WIN32
            if (ok)
                remove(path);
#endif
            if (ok && rename(tmp, path) == 0)
                rc = 0;
            else
                remove(tmp);
        }
    }
    if (rc != 0)
        g->dirty = true;
    free(tmp);
    free(buf);
    return rc;
}

int rl_gb_load(rl_gb *g, const char *path)
{
    if (!g->cart.battery)
        return 1;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 1;
    size_t cap = rl_gb_save_size(g) + 1;
    uint8_t *buf = malloc(cap);
    int rc = -1;
    if (buf) {
        size_t n = fread(buf, 1, cap, f);
        if (!ferror(f))
            rc = rl_gb_load_bytes(g, buf, n);
    }
    fclose(f);
    free(buf);
    return rc;
}
