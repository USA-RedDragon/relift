/**
 * @file ym2413_script.c
 * @brief Runs a YM2413 script on the device and prints each sample, for the
 * comparison with ares (check/oracle/ares/ym2413-compare.py).
 *
 * The script has one event per line: "<clock> a <byte>" (address port),
 * "<clock> w <byte>" (data port) or "<clock> end". Clocks do not decrease.
 * Each output line is a sample, the chip's mix as an integer.
 */
#include "ym2413.h"

#include <inttypes.h>
#include <stdio.h>

static sms_ym2413 dev;

static void flush(void)
{
    int32_t s[256];
    size_t n;
    while ((n = sms_ym2413_drain(&dev, s, 256)) != 0)
        for (size_t i = 0; i < n; i++)
            printf("%" PRId32 "\n", s[i]);
}

int main(void)
{
    sms_ym2413_reset(&dev);
    char line[128];
    while (fgets(line, sizeof line, stdin)) {
        uint64_t t;
        char kind[8];
        unsigned v = 0;
        if (sscanf(line, "%" SCNu64 " %7s %u", &t, kind, &v) < 2)
            continue;
        if (kind[0] == 'a')
            sms_ym2413_address(&dev, t, (uint8_t)v);
        else if (kind[0] == 'w')
            sms_ym2413_write(&dev, t, (uint8_t)v);
        else
            sms_ym2413_catch_up(&dev, t);
        flush();
    }
    return 0;
}
