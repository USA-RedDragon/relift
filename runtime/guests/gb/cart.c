/**
 * @file cart.c
 * @brief Reading the cartridge header.
 */
#include "gb_int.h"

#include <string.h>

enum {
    HDR_LOGO = 0x104,
    HDR_LOGO_LEN = 48,
    HDR_CGB = 0x143,
    HDR_TYPE = 0x147,
    HDR_ROM = 0x148,
    HDR_RAM = 0x149,
    HDR_END = 0x150
};

typedef struct kind {
    uint8_t code;
    uint8_t mbc;
    uint8_t ram;
    uint8_t battery;
    uint8_t rtc;
    uint8_t rumble;
} kind;

static const kind kinds[] = {
    {0x00, RL_GB_MBC_NONE, 0, 0, 0, 0},
    {0x01, RL_GB_MBC1, 0, 0, 0, 0},
    {0x02, RL_GB_MBC1, 1, 0, 0, 0},
    {0x03, RL_GB_MBC1, 1, 1, 0, 0},
    {0x05, RL_GB_MBC2, 0, 0, 0, 0},
    {0x06, RL_GB_MBC2, 0, 1, 0, 0},
    {0x08, RL_GB_MBC_NONE, 1, 0, 0, 0},
    {0x09, RL_GB_MBC_NONE, 1, 1, 0, 0},
    {0x0B, RL_GB_MBC_UNSUPPORTED, 0, 0, 0, 0},
    {0x0C, RL_GB_MBC_UNSUPPORTED, 1, 0, 0, 0},
    {0x0D, RL_GB_MBC_UNSUPPORTED, 1, 1, 0, 0},
    {0x0F, RL_GB_MBC3, 0, 1, 1, 0},
    {0x10, RL_GB_MBC3, 1, 1, 1, 0},
    {0x11, RL_GB_MBC3, 0, 0, 0, 0},
    {0x12, RL_GB_MBC3, 1, 0, 0, 0},
    {0x13, RL_GB_MBC3, 1, 1, 0, 0},
    {0x19, RL_GB_MBC5, 0, 0, 0, 0},
    {0x1A, RL_GB_MBC5, 1, 0, 0, 0},
    {0x1B, RL_GB_MBC5, 1, 1, 0, 0},
    {0x1C, RL_GB_MBC5, 0, 0, 0, 1},
    {0x1D, RL_GB_MBC5, 1, 0, 0, 1},
    {0x1E, RL_GB_MBC5, 1, 1, 0, 1},
    {0x20, RL_GB_MBC_UNSUPPORTED, 1, 1, 0, 0},
    {0x22, RL_GB_MBC_UNSUPPORTED, 1, 1, 0, 1},
    {0xFC, RL_GB_MBC_UNSUPPORTED, 1, 1, 0, 0},
    {0xFD, RL_GB_MBC_UNSUPPORTED, 1, 1, 1, 0},
    {0xFE, RL_GB_MBC_UNSUPPORTED, 1, 1, 1, 0},
    {0xFF, RL_GB_MBC_UNSUPPORTED, 1, 1, 0, 0},
};

static bool ram_size(uint8_t code, uint32_t *bytes)
{
    static const uint32_t sizes[] = {0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000};
    if (code >= sizeof sizes / sizeof sizes[0])
        return false;
    *bytes = sizes[code];
    return true;
}

static bool is_multicart(const uint8_t *rom, size_t len)
{
    if (len != 64 * (size_t)GB_ROM_BANK)
        return false;
    const uint8_t *logo = rom + HDR_LOGO;
    bool blank = true;
    for (unsigned i = 0; i < HDR_LOGO_LEN; i++)
        blank = blank && logo[i] == 0;
    if (blank)
        return false;
    unsigned hits = 0;
    for (unsigned b = 0x10; b <= 0x30; b += 0x10)
        hits += memcmp(rom + (size_t)b * GB_ROM_BANK + HDR_LOGO, logo, HDR_LOGO_LEN) == 0;
    return hits >= 2;
}

int rl_gb_cart_parse(const uint8_t *rom, size_t len, rl_gb_cart *cart)
{
    if (!rom || !cart || len < HDR_END)
        return -1;
    const kind *k = NULL;
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++)
        if (kinds[i].code == rom[HDR_TYPE])
            k = &kinds[i];
    uint32_t ram = 0;
    if (!k || !ram_size(rom[HDR_RAM], &ram) || (rom[HDR_ROM] > 8 && (rom[HDR_ROM] < 0x52 || rom[HDR_ROM] > 0x54)))
        return -1;
    memset(cart, 0, sizeof *cart);
    cart->rom = rom;
    cart->rom_len = len;
    cart->mbc = (rl_gb_mbc)k->mbc;
    cart->ram_len = k->ram ? ram : 0;
    cart->battery = k->battery;
    cart->rtc = k->rtc;
    cart->rumble = k->rumble;
    cart->cgb = (rom[HDR_CGB] & 0x80) != 0;
    cart->multicart = cart->mbc == RL_GB_MBC1 && is_multicart(rom, len);
    return 0;
}
