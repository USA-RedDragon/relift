/**
 * @file save.c
 * @brief The cartridge backup: SRAM, the choice among the chips of
 * flash.c and eeprom.c, finding the type from the ROM's library strings,
 * the raw .sav image and file (the layout emulators and flash carts use),
 * atomic writes, the save path and the settings.
 */
#include "gba_int.h"

#include <relift/settings.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define make_dir(p) mkdir(p, 0777)
#endif

enum { SRAM_LEN = 0x8000, FLASH64_LEN = 0x10000, FLASH128_LEN = 0x20000, EEPROM_SMALL = 512, EEPROM_LARGE = 8192 };

static const struct {
    const char *text;
    rl_gba_save_kind kind;
    uint16_t id;
} strings[] = {
    {"EEPROM_V", RL_GBA_SAVE_EEPROM, 0},
    {"SRAM_V", RL_GBA_SAVE_SRAM, 0},
    {"SRAM_F_V", RL_GBA_SAVE_SRAM, 0},
    {"FLASH_V", RL_GBA_SAVE_FLASH64, RL_GBA_FLASH_PANASONIC},
    {"FLASH512_V", RL_GBA_SAVE_FLASH64, RL_GBA_FLASH_PANASONIC},
    {"FLASH1M_V", RL_GBA_SAVE_FLASH128, RL_GBA_FLASH_MACRONIX128},
};

rl_gba_save_info rl_gba_save_detect(const uint8_t *rom, size_t len)
{
    rl_gba_save_info none = {RL_GBA_SAVE_NONE, 0};
    for (size_t i = 0; rom && i < len; i++) {
        if (rom[i] != 'E' && rom[i] != 'S' && rom[i] != 'F')
            continue;
        for (unsigned k = 0; k < sizeof strings / sizeof strings[0]; k++) {
            size_t n = strlen(strings[k].text);
            if (len - i > n && memcmp(rom + i, strings[k].text, n) == 0 && rom[i + n] >= '0' && rom[i + n] <= '9')
                return (rl_gba_save_info){strings[k].kind, strings[k].id};
        }
    }
    return none;
}

static size_t kind_len(rl_gba_save_kind k)
{
    switch (k) {
    case RL_GBA_SAVE_SRAM:
        return SRAM_LEN;
    case RL_GBA_SAVE_FLASH64:
        return FLASH64_LEN;
    case RL_GBA_SAVE_FLASH128:
        return FLASH128_LEN;
    case RL_GBA_SAVE_EEPROM:
        return EEPROM_LARGE;
    default:
        return 0;
    }
}

int gba_save_init(rl_gba *g, const rl_gba_cart *cart)
{
    gba_save *s = &g->save;
    memset(s, 0, sizeof *s);
    if (cart->sram) {
        s->kind = RL_GBA_SAVE_SRAM;
        s->data = cart->sram;
        s->len = s->cap = cart->sram_len;
        return 0;
    }
    if ((unsigned)cart->save > RL_GBA_SAVE_EEPROM || cart->save == RL_GBA_SAVE_NONE)
        return cart->save == RL_GBA_SAVE_NONE ? 0 : -1;
    s->kind = cart->save;
    s->cap = kind_len(cart->save);
    s->len = s->kind == RL_GBA_SAVE_EEPROM ? 0 : s->cap;
    if (s->kind == RL_GBA_SAVE_EEPROM && cart->eeprom_len) {
        if (cart->eeprom_len != EEPROM_SMALL && cart->eeprom_len != EEPROM_LARGE)
            return -1;
        s->len = cart->eeprom_len;
        s->ee.sized = true;
    }
    if (s->kind == RL_GBA_SAVE_FLASH64 || s->kind == RL_GBA_SAVE_FLASH128) {
        if (!gba_flash_valid(s->kind, cart->flash_id))
            return -1;
        s->fl.id = cart->flash_id ? cart->flash_id : s->kind == RL_GBA_SAVE_FLASH64 ? RL_GBA_FLASH_PANASONIC : RL_GBA_FLASH_MACRONIX128;
    }
    s->data = malloc(s->cap);
    if (!s->data)
        return -1;
    memset(s->data, 0xFF, s->cap);
    s->owned = true;
    return 0;
}

void gba_save_free(rl_gba *g)
{
    if (g->save.owned)
        free(g->save.data);
    g->save.data = NULL;
}

void gba_save_touch(gba_save *s, uint64_t now)
{
    s->dirty = true;
    s->changed = now;
}

uint8_t gba_save_read(rl_gba *g, uint32_t addr)
{
    gba_save *s = &g->save;
    switch (s->kind) {
    case RL_GBA_SAVE_SRAM:
        return s->data[addr & (s->len - 1)];
    case RL_GBA_SAVE_FLASH64:
    case RL_GBA_SAVE_FLASH128:
        return gba_flash_read(g, addr);
    default:
        return 0xFF;
    }
}

void gba_save_write(rl_gba *g, uint32_t addr, uint8_t v)
{
    gba_save *s = &g->save;
    switch (s->kind) {
    case RL_GBA_SAVE_SRAM: {
        uint8_t *p = &s->data[addr & (s->len - 1)];
        if (*p != v) {
            *p = v;
            gba_save_touch(s, gba_now(g));
        }
        break;
    }
    case RL_GBA_SAVE_FLASH64:
    case RL_GBA_SAVE_FLASH128:
        gba_flash_write(g, addr, v);
        break;
    default:
        break;
    }
}

rl_gba_save_kind rl_gba_save_kind_of(const rl_gba *g) { return g->save.kind; }
size_t rl_gba_save_len(const rl_gba *g) { return g->save.len; }
const uint8_t *rl_gba_save_data(const rl_gba *g) { return g->save.data; }
bool rl_gba_save_dirty(const rl_gba *g) { return g->save.dirty; }
void rl_gba_save_clean(rl_gba *g) { g->save.dirty = false; }

int rl_gba_save_load(rl_gba *g, const uint8_t *data, size_t len)
{
    gba_save *s = &g->save;
    if (s->kind == RL_GBA_SAVE_NONE || !data)
        return -1;
    if (s->kind == RL_GBA_SAVE_EEPROM) {
        bool fixed = s->ee.sized && g->cart.eeprom_len;
        if ((len != EEPROM_SMALL && len != EEPROM_LARGE) || (fixed && len != s->len))
            return -1;
        memset(s->data, 0xFF, s->cap);
        gba_eeprom_size(g, len);
    } else if (len != s->len) {
        return -1;
    }
    memcpy(s->data, data, len);
    s->dirty = false;
    return 0;
}

int rl_gba_save_read(rl_gba *g, const char *path)
{
    gba_save *s = &g->save;
    if (s->kind == RL_GBA_SAVE_NONE)
        return 1;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 1;
    uint8_t *buf = malloc(s->cap + 1);
    int rc = -1;
    if (buf) {
        size_t n = fread(buf, 1, s->cap + 1, f);
        if (!ferror(f))
            rc = rl_gba_save_load(g, buf, n);
    }
    fclose(f);
    free(buf);
    return rc;
}

static void make_dirs(const char *path)
{
    char *p = strdup(path);
    if (!p)
        return;
    for (char *c = p + 1; *c; c++)
        if (*c == '/' || *c == '\\') {
            char sep = *c;
            *c = 0;
            make_dir(p);
            *c = sep;
        }
    free(p);
}

int rl_gba_save_write(rl_gba *g, const char *path)
{
    gba_save *s = &g->save;
    if (s->kind == RL_GBA_SAVE_NONE || (s->kind == RL_GBA_SAVE_EEPROM && !s->ee.sized))
        return 1;
    char *tmp = malloc(strlen(path) + 5);
    if (!tmp)
        return -1;
    strcpy(tmp, path);
    strcat(tmp, ".tmp");
    make_dirs(path);
    int rc = -1;
    FILE *f = fopen(tmp, "wb");
    if (f) {
        bool ok = fwrite(s->data, 1, s->len, f) == s->len && fflush(f) == 0;
#ifndef _WIN32
        ok = ok && fsync(fileno(f)) == 0;
#endif
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
    if (rc == 0)
        s->dirty = false;
    free(tmp);
    return rc;
}

int rl_gba_save_poll(rl_gba *g, const char *path, uint64_t quiet)
{
    gba_save *s = &g->save;
    if (!s->dirty || gba_now(g) < s->changed + quiet)
        return 0;
    int rc = rl_gba_save_write(g, path);
    return rc == 0 ? 1 : rc < 0 ? -1 : 0;
}

bool rl_gba_save_path(char *out, size_t size, const char *rom_path)
{
    const char *base = rom_path;
    for (const char *c = rom_path; *c; c++)
        if (*c == '/' || *c == '\\')
            base = c + 1;
    size_t stem = strlen(base);
    const char *dot = strrchr(base, '.');
    if (dot && dot != base)
        stem = (size_t)(dot - base);
    char dir[4096];
    const char *save = rl_setting_str("save.dir");
    if (*save) {
        snprintf(dir, sizeof dir, "%s", save);
    } else if (rl_settings_app() && rl_settings_user_dir(dir, sizeof dir - 8, rl_settings_app())) {
        strcat(dir, "/save");
    } else {
        size_t n = (size_t)(base - rom_path);
        if (n == 0)
            snprintf(dir, sizeof dir, ".");
        else
            snprintf(dir, sizeof dir, "%.*s", (int)(n - 1), rom_path);
    }
    size_t dl = strlen(dir);
    while (dl > 1 && (dir[dl - 1] == '/' || dir[dl - 1] == '\\'))
        dir[--dl] = 0;
    int n = snprintf(out, size, "%s/%.*s.sav", dir, (int)stem, base);
    return n > 0 && (size_t)n < size;
}

static const rl_setting_def g_defs[] = {
    {
        .key = "save.dir",
        .type = RL_SETTING_STRING,
        .label = "Save folder",
        .description = "Where the game's save file is kept. Empty: <user folder>/save when the program names an app, "
                       "else next to the ROM",
        .env = "RELIFT_SAVE",
    },
};

void rl_gba_declare_settings(void)
{
    rl_settings_declare_runtime();
    rl_settings_declare(g_defs, sizeof g_defs / sizeof g_defs[0]);
}
