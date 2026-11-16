/**
 * @file psg_script.c
 * @brief Runs a PSG script on the device and prints each sample, for the
 * comparison with ares (check/oracle/ares/psg-compare.py).
 *
 * The script has one event per line: "<clock> w <byte>" (data port),
 * "<clock> s <byte>" (Game Gear stereo port) or "<clock> end". Clocks do
 * not decrease. Usage: psg_script [--gg] < script
 * Each output line is "<att0> <att1> <att2> <att3> <left> <right>", the two
 * outputs as C hexadecimal floats.
 */
#include "psg.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static sms_psg dev;

static void flush(void)
{
    sms_psg_sample s[256];
    size_t n;
    while ((n = sms_psg_drain(&dev, s, 256)) != 0)
        for (size_t i = 0; i < n; i++)
            printf("%u %u %u %u %a %a\n", s[i].att[0], s[i].att[1], s[i].att[2], s[i].att[3], s[i].left, s[i].right);
}

int main(int argc, char **argv)
{
    sms_psg_reset(&dev, argc > 1 && strcmp(argv[1], "--gg") == 0);
    char line[128];
    while (fgets(line, sizeof line, stdin)) {
        uint64_t t;
        char kind[8];
        unsigned v = 0;
        if (sscanf(line, "%" SCNu64 " %7s %u", &t, kind, &v) < 2)
            continue;
        if (kind[0] == 'w')
            sms_psg_write(&dev, t, (uint8_t)v);
        else if (kind[0] == 's')
            sms_psg_stereo(&dev, t, (uint8_t)v);
        else
            sms_psg_catch_up(&dev, t);
        flush();
    }
    return 0;
}
