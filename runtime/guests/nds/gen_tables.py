"""usage: python3 -I gen_tables.py OUT.c  -- writes runtime/guests/nds/bios_tables.c (sine, pitch, volume tables from their formulas)"""
import math, sys

def sine():
    return [round(32767 * math.sin(i * math.pi / 128)) for i in range(64)]

def pitch():
    return [round(65536 * (2 ** (i / 768) - 1)) for i in range(768)]

SEGS = ((1, 482, 0), (483, 602, 2), (603, 662, 3), (663, 723, 4))
def volume():
    out = [0] * 724
    for lo, hi, k in SEGS:
        for i in range(lo, hi + 1):
            v = math.floor(127.742 * 10 ** ((i - 482) / 200) / 2 ** k + 0.5)
            out[i] = 126 if v == 127 else min(v, 127)
    return out

def rows(name, vals, width, per):
    s = "const uint16_t %s[%d] = {\n" % (name, len(vals))
    for i in range(0, len(vals), per):
        s += "    " + ", ".join(("0x%0" + str(width) + "X") % v for v in vals[i:i + per]) + ",\n"
    return s + "};\n"

HEAD = '''/**
 * @file bios_tables.c
 * @brief The tables of GetSineTable, GetPitchTable and GetVolumeTable, and
 * GetBootProcs.
 *
 * The tables are generated from the formulas below by
 * runtime/guests/nds/gen_tables.py (no BIOS image is read); a unit test
 * recomputes every entry with the C library and compares. The endpoints
 * match GBATEK's ranges ("SIN(0 .. 88.6 degrees)*8000h" ending 0x7FF5,
 * pitch ending 0xFF8A, volume 0x00 to 0x7F).
 *
 *   sine[i]   = round(32767 * sin(i * pi / 128))
 *   pitch[i]  = round(65536 * (2^(i / 768) - 1))
 *   volume[i] = 0 at 0; else, with k = 0, 2, 3, 4 on 1..482, 483..602,
 *               603..662, 663..723: v = round(127.742 * 10^((i - 482) / 200)
 *               / 2^k), then 127 -> 126 and anything above 127 -> 127.
 *               (One tenth of a decibel per index; the constant and the
 *               segment steps are fitted to the values a BIOS-free
 *               reimplementation returns, not given by GBATEK.)
 */
#include "bios_int.h"

'''

TAIL = '''
uint32_t rl_nds_bios_sine(unsigned index) { return index < 64 ? bios_sine[index] : 0; }

uint32_t rl_nds_bios_pitch(unsigned index) { return index < 768 ? bios_pitch[index] : 0; }

uint32_t rl_nds_bios_volume(unsigned index) { return index < 724 ? bios_volume[index] : 0; }

void nds_bios_boot_procs(const nds_bios_env *e, uint32_t r[4])
{
    (void)e;
    for (unsigned i = 0; i < 4; i++)
        r[i] = 0;
}

rl_nds_swi_status nds_bios_custom_halt(const nds_bios_env *e, uint32_t value)
{
    nds_bios_st(e, 0x04000301u, 1, value & 0xFFu, false);
    return rl_nds_halt_lost(e->m, e->cpu) ? RL_NDS_SWI_STOP : RL_NDS_SWI_OK;
}
'''
body = HEAD + "static " + rows("bios_sine", sine(), 4, 8).replace("const uint16_t", "const uint16_t") + "\nstatic " + rows("bios_pitch", pitch(), 4, 8) + "\nstatic " + rows("bios_volume", volume(), 2, 16) + TAIL
open(sys.argv[1], "w").write(body)
