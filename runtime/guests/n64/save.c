/**
 * @file save.c
 * @brief The cartridge's save memory on disk: the raw images emulators and
 * flash carts use (EEPROM 512 or 2048 bytes as .eep, SRAM 32 KiB as .sra,
 * Flash 128 KiB as .fla) next to each other under one base path, written
 * only for the stores the game changed, atomically, a while after the last
 * change and at exit. A store is not looked up in any table: the machine
 * finds out which chips the game uses from what it does, and a file found
 * at load says which one a saved game used before.
 */
#include "n64_int.h"

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

enum { EEPROM_SMALL = 512, EEPROM_LARGE = 2048, SRAM_LEN = 0x8000, FLASH_LEN = 0x20000 };

static const struct {
    uint8_t store;
    const char *ext;
} stores[] = {{SAVE_EEPROM, ".eep"}, {SAVE_SRAM, ".sra"}, {SAVE_FLASH, ".fla"}};

static uint8_t *data_of(rl_n64 *m, uint8_t store, size_t *len)
{
    switch (store) {
    case SAVE_EEPROM:
        *len = m->cart.eeprom_len;
        return m->cart.eeprom;
    case SAVE_SRAM:
        *len = SRAM_LEN;
        return m->cart.sram;
    default:
        *len = FLASH_LEN;
        return m->cart.flash;
    }
}

bool rl_n64_save_dirty(const rl_n64 *m) { return m->cart.dirty != 0; }

static int load_store(rl_n64 *m, uint8_t store, const uint8_t *data, size_t len)
{
    if (!data)
        return -1;
    if (store == SAVE_EEPROM) {
        if (len != EEPROM_SMALL && len != EEPROM_LARGE)
            return -1;
        memset(m->cart.eeprom, 0xFF, sizeof m->cart.eeprom);
        m->cart.eeprom_len = (uint32_t)len;
    } else if (len != (store == SAVE_SRAM ? SRAM_LEN : FLASH_LEN)) {
        return -1;
    }
    size_t n;
    memcpy(data_of(m, store, &n), data, len);
    if (store == SAVE_FLASH)
        m->cart.flash_armed = true;
    m->cart.dirty &= (uint8_t)~store;
    return 0;
}

int rl_n64_save_load(rl_n64 *m, const char *base)
{
    int loaded = 0;
    bool failed = false;
    size_t bl = strlen(base);
    char *path = malloc(bl + 5);
    uint8_t *buf = malloc(FLASH_LEN + 1);
    if (!path || !buf) {
        free(path);
        free(buf);
        return -1;
    }
    for (unsigned i = 0; i < sizeof stores / sizeof stores[0]; i++) {
        snprintf(path, bl + 5, "%s%s", base, stores[i].ext);
        FILE *f = fopen(path, "rb");
        if (!f)
            continue;
        size_t n = fread(buf, 1, FLASH_LEN + 1, f);
        bool bad = ferror(f) != 0;
        fclose(f);
        if (bad || load_store(m, stores[i].store, buf, n) != 0)
            failed = true;
        else
            loaded++;
    }
    free(path);
    free(buf);
    return failed ? -1 : loaded;
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

static bool write_file(const char *path, const uint8_t *data, size_t len)
{
    char *tmp = malloc(strlen(path) + 5);
    if (!tmp)
        return false;
    strcpy(tmp, path);
    strcat(tmp, ".tmp");
    make_dirs(path);
    bool ok = false;
    FILE *f = fopen(tmp, "wb");
    if (f) {
        ok = fwrite(data, 1, len, f) == len && fflush(f) == 0;
#ifndef _WIN32
        ok = ok && fsync(fileno(f)) == 0;
#endif
        ok = fclose(f) == 0 && ok;
#ifdef _WIN32
        if (ok)
            remove(path);
#endif
        ok = ok && rename(tmp, path) == 0;
        if (!ok)
            remove(tmp);
    }
    free(tmp);
    return ok;
}

int rl_n64_save_write(rl_n64 *m, const char *base)
{
    int written = 0;
    size_t bl = strlen(base);
    char *path = malloc(bl + 5);
    if (!path)
        return -1;
    for (unsigned i = 0; i < sizeof stores / sizeof stores[0]; i++) {
        if (!(m->cart.dirty & stores[i].store))
            continue;
        size_t len;
        const uint8_t *data = data_of(m, stores[i].store, &len);
        snprintf(path, bl + 5, "%s%s", base, stores[i].ext);
        if (!write_file(path, data, len)) {
            free(path);
            return -1;
        }
        m->cart.dirty &= (uint8_t)~stores[i].store;
        written++;
    }
    free(path);
    return written;
}

int rl_n64_save_poll(rl_n64 *m, const char *base, uint64_t quiet)
{
    if (!m->cart.dirty || rl_n64_now(m) < m->cart.changed + quiet)
        return 0;
    int rc = rl_n64_save_write(m, base);
    return rc < 0 ? -1 : rc > 0 ? 1 : 0;
}

bool rl_n64_save_path(char *out, size_t size, const char *rom_path)
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
    int n = snprintf(out, size, "%s/%.*s", dir, (int)stem, base);
    return n > 0 && (size_t)n < size;
}

static const rl_setting_def g_defs[] = {
    {
        .key = "save.dir",
        .type = RL_SETTING_STRING,
        .label = "Save folder",
        .description = "Where the game's save files are kept. Empty: <user folder>/save when the program names an app, "
                       "else next to the ROM",
        .env = "RELIFT_SAVE",
    },
};

void rl_n64_declare_settings(void)
{
    rl_settings_declare_runtime();
    rl_settings_declare(g_defs, sizeof g_defs / sizeof g_defs[0]);
}
