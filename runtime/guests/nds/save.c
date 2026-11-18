/**
 * @file save.c
 * @brief The cartridge backup as a file (the raw image emulators and flash
 * carts use: its length names the chip, see backup.c), atomic writes, the
 * save path and the memory the boot code leaves behind for
 * a cartridge (chip ID, header CRC, the firmware's user settings).
 */
#include "nds_int.h"

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

static const struct {
    const char *name;
    nds_bk_kind kind;
    uint32_t len;
} types[] = {
    {"eeprom512", NDS_BK_EEPROM, 512},     {"eeprom8k", NDS_BK_EEPROM, 8192},    {"eeprom64k", NDS_BK_EEPROM, 65536},
    {"eeprom128k", NDS_BK_EEPROM, 131072}, {"fram8k", NDS_BK_FRAM, 8192},        {"fram32k", NDS_BK_FRAM, 32768},
    {"flash256k", NDS_BK_FLASH, 262144},   {"flash512k", NDS_BK_FLASH, 524288},  {"flash1m", NDS_BK_FLASH, 1048576},
};

int rl_nds_save_set(rl_nds *m, const char *type)
{
    if (!type || !*type || !strcmp(type, "auto"))
        return 0;
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++)
        if (!strcmp(type, types[i].name))
            return nds_bk_set(&m->cart.bk, types[i].kind, types[i].len) ? 0 : -1;
    return -1;
}

size_t rl_nds_save_len(const rl_nds *m) { return m->cart.bk.data ? m->cart.bk.len : 0; }
const uint8_t *rl_nds_save_data(const rl_nds *m) { return m->cart.bk.data; }
bool rl_nds_save_dirty(const rl_nds *m) { return m->cart.bk.dirty; }
void rl_nds_save_clean(rl_nds *m) { m->cart.bk.dirty = false; }

int rl_nds_save_load(rl_nds *m, const uint8_t *data, size_t len)
{
    nds_backup *b = &m->cart.bk;
    if (!data)
        return -1;
    if (b->data) {
        if (len != b->len)
            return -1;
    } else {
        size_t i = 0;
        while (i < sizeof types / sizeof types[0] && types[i].len != len)
            i++;
        if (i == sizeof types / sizeof types[0])
            return -1;
        nds_bk_kind kind = types[i].kind;
        if (len == 8192)
            kind = NDS_BK_EEPROM;
        if (!nds_bk_set(b, kind, (uint32_t)len))
            return -1;
    }
    memcpy(b->data, data, len);
    b->dirty = false;
    return 0;
}

int rl_nds_save_read(rl_nds *m, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 1;
    uint8_t *buf = malloc(NDS_BACKUP_MAX + 1);
    int rc = -1;
    if (buf) {
        size_t n = fread(buf, 1, NDS_BACKUP_MAX + 1, f);
        if (!ferror(f))
            rc = rl_nds_save_load(m, buf, n);
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

int rl_nds_save_write(rl_nds *m, const char *path)
{
    nds_backup *b = &m->cart.bk;
    if (!b->data)
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
        bool ok = fwrite(b->data, 1, b->len, f) == b->len && fflush(f) == 0;
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
        b->dirty = false;
    free(tmp);
    return rc;
}

int rl_nds_save_poll(rl_nds *m, const char *path, uint64_t quiet)
{
    nds_backup *b = &m->cart.bk;
    if (!b->dirty || nds_now(m, RL_NDS_ARM9) < b->changed + quiet)
        return 0;
    int rc = rl_nds_save_write(m, path);
    return rc == 0 ? 1 : rc < 0 ? -1 : 0;
}

bool rl_nds_save_path(char *out, size_t size, const char *rom_path)
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

static void poke(rl_nds *m, uint32_t addr, unsigned size, uint32_t v) { rl_nds_write(m, RL_NDS_ARM9, addr, size, v); }

void rl_nds_user_settings(const rl_nds *m, uint8_t out[0x70])
{
    if (m->spi.fw)
        memcpy(out, m->spi.fw + 0x3FF00, 0x70);
    else
        memset(out, 0, 0x70);
}

void rl_nds_boot_state(rl_nds *m)
{
    const nds_cart *c = &m->cart;
    rl_nds_write(m, RL_NDS_ARM9, 0x04000204u, 2, 0xE880u);
    uint16_t hdr_crc = 0, sec_crc = 0;
    if (c->rom && c->rom_len >= 0x170) {
        hdr_crc = (uint16_t)(c->rom[0x15E] | c->rom[0x15F] << 8);
        sec_crc = (uint16_t)(c->rom[0x6C] | c->rom[0x6D] << 8);
    }
    for (unsigned copy = 0; copy < 2; copy++) {
        uint32_t base = copy ? 0x027FFC00u : 0x027FF800u;
        poke(m, base, 4, c->chip_id);
        poke(m, base + 4, 4, c->chip_id);
        poke(m, base + 8, 2, hdr_crc);
        poke(m, base + 10, 2, sec_crc);
        poke(m, base + 12, 2, 0);
        poke(m, base + 14, 2, 0);
    }
    poke(m, 0x027FF810u, 2, 0xFFFF);
    poke(m, 0x027FF850u, 2, 0x5835);
    poke(m, 0x027FFC10u, 2, 0x5835);
    poke(m, 0x027FF864u, 4, 0);
    poke(m, 0x027FF868u, 4, 0x3FE00);
    poke(m, 0x027FF880u, 4, 7);
    poke(m, 0x027FF884u, 4, 6);
    poke(m, 0x027FF890u, 4, 0xB0002A22u);
    uint8_t user[0x70];
    rl_nds_user_settings(m, user);
    for (unsigned i = 0; i < 0x70; i += 4)
        poke(m, 0x027FFC80u + i, 4, (uint32_t)user[i] | (uint32_t)user[i + 1] << 8 | (uint32_t)user[i + 2] << 16 | (uint32_t)user[i + 3] << 24);
}
