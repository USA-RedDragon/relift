/**
 * @file firmware.c
 * @brief The generated firmware image of the SPI flash: the header (with
 * its CRC-16 fields), the Wifi calibration block with its CRC, two user
 * settings areas with the update counters and CRCs of GBATEK's layout. The
 * boot code, GUI and Wifi parts are not present (the BIOS is a high-level
 * stub), and no byte comes from a console.
 *
 * The user settings are an English-language setup with the touch screen
 * calibrated so that the converter's value is sixteen times the pixel: ADC
 * (0, 0) and (0xFE0, 0xBE0) at pixels (1, 1) and (255, 191).
 *
 * Fresh C from GBATEK ("DS Firmware Header", "DS Firmware Wifi Calibration
 * Data", "DS Firmware User Settings").
 */
#include "nds_int.h"

#include <string.h>

enum {
    USER_AREA0 = 0x3FE00,
    USER_AREA1 = 0x3FF00,
    WIFI_LEN = 0x138,
};

uint16_t nds_crc16(uint16_t init, const uint8_t *p, size_t n)
{
    uint16_t crc = init;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (unsigned b = 0; b < 8; b++)
            crc = (uint16_t)((crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1);
    }
    return crc;
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void user_settings(uint8_t *u, unsigned counter)
{
    memset(u, 0, 0x100);
    put16(u + 0x00, 5);
    u[0x02] = 0;
    u[0x03] = 1;
    u[0x04] = 1;
    static const char name[] = "relift";
    for (unsigned i = 0; name[i]; i++)
        put16(u + 0x06 + 2 * i, (uint8_t)name[i]);
    put16(u + 0x1A, 6);
    put16(u + 0x50, 0);
    put16(u + 0x58, 0);
    put16(u + 0x5A, 0);
    u[0x5C] = 1;
    u[0x5D] = 1;
    put16(u + 0x5E, 0xFE0);
    put16(u + 0x60, 0xBE0);
    u[0x62] = 255;
    u[0x63] = 191;
    put16(u + 0x64, 0xEC01);
    u[0x66] = 24;
    memset(u + 0x6C, 0xFF, 4);
    put16(u + 0x70, counter);
    put16(u + 0x72, nds_crc16(0xFFFF, u, 0x70));
    memset(u + 0x74, 0xFF, 0x8C);
}

void nds_firmware_build(uint8_t *fw)
{
    memset(fw, 0xFF, NDS_FIRMWARE_SIZE);
    memset(fw, 0, 0x200);
    put16(fw + 0x00, 0x200 / 8);
    put16(fw + 0x02, 0x200 / 8);
    memcpy(fw + 0x08, "RLFW", 4);
    put16(fw + 0x14, 0x2000 | 0x24);
    put16(fw + 0x16, 0x200 / 8);
    fw[0x1D] = 0xFF;
    put16(fw + 0x1E, 0xFFFF);
    put16(fw + 0x20, USER_AREA0 / 8);
    put16(fw + 0x28, 0xFFFF);
    uint8_t *w = fw + 0x2C;
    put16(fw + 0x2C, WIFI_LEN);
    w[2] = 0;
    w[3] = 0;
    static const uint8_t mac[6] = {0x00, 0x09, 0xBF, 0x52, 0x4C, 0x46};
    memcpy(fw + 0x36, mac, 6);
    put16(fw + 0x3C, 0x3FFE);
    put16(fw + 0x3E, 0xFFFF);
    fw[0x40] = 2;
    fw[0x41] = 0x18;
    fw[0x42] = 0x0C;
    fw[0x43] = 1;
    put16(fw + 0x2A, nds_crc16(0, fw + 0x2C, WIFI_LEN));
    put16(fw + 0x04, nds_crc16(0xFFFF, fw + 0x200, 0));
    put16(fw + 0x06, nds_crc16(0xFFFF, fw + 0x200, 0));
    put16(fw + 0x26, nds_crc16(0xFFFF, fw + 0x200, 0));
    memset(fw + 0x200, 0xFF, 0x100);
    user_settings(fw + USER_AREA0, 0);
    user_settings(fw + USER_AREA1, 1);
}
