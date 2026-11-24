#!/usr/bin/env python3
"""xfblayout.py: the stream-out methods the NVN2 driver writes for a program's transform feedback block.

  xfblayout.py check <sdk.nso> [n]     run the driver's own code (hw sdk.nso 0x619a84-0x619df8) on n random blocks
                                       through a64run and compare its words with emulate(); prints mismatches
  xfblayout.py table <sdk.nso>         the driver's attribute table (u16 per GLSLC attr, from -2), as C

The block is the driver's per-stage xfb state (0x614 bytes, filled by 0x5e5bc0 from the GLSLC header; glslc.py):
  +0x000 + 0x80*i  s8 attr[count]      +0x200 + 0x80*i u8 first[count]     +0x400 + 0x80*i u8 ncomp[count]
  +0x600 u16 stride[4]   +0x608 u8 stream[4]   +0x60c u8 count[4]   +0x610 u8 buffers   +0x612 u8 raw
"""
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import a64run

TABLE_PTR = 0xDE1900
"""The pointer the layout loop loads its attribute table through, in the SDK 22 driver sdk.nso of build id
820cd7d096aac448693f504400cc2375416d1d3b; the loop (0x619a84-0x619df8), memcpy (0x790fb0) and memset (0x791190) are
from the same build."""


def table_of(nso):
    """The table address, from the pointer the driver loads, and a reader of its byte at attr * 2."""
    m = a64run.Mem(nso)
    base, _ = m.read(TABLE_PTR, 8)
    return base, lambda attr: m.read((base + 2 * attr) & 0xFFFFFFFFFFFFFFFF, 1)[0]


def emulate(block, tab):
    """The method words for block (bytes) with tab(attr) the driver table's byte, as the driver builds them."""
    out = []
    if not block[0x610]:
        return out
    raw = block[0x612]
    for i in range(4):
        stride = struct.unpack_from("<H", block, 0x600 + 2 * i)[0]
        out += [0x20030000 | ((0x700 + 16 * i) >> 2), block[0x608 + i], stride >> 2, stride]
        if not stride:
            continue
        count = block[0x60C + i]
        bs = bytearray()
        if raw:
            bs += block[0x80 * i: 0x80 * i + count]
        else:
            for j in range(count):
                a = block[0x80 * i + j]
                a = a - 256 if a > 127 else a
                c = block[0x200 + 0x80 * i + j]
                end = (c + block[0x400 + 0x80 * i + j]) & 0xFF
                while c < end:
                    bs.append(0xFF if ((a + 6) & 0xFFFFFFFF) < 4 else (tab(a) + c) & 0xFF)
                    c = (c + 1) & 0xFF
        if len(bs) & 3:
            bs += bytes(4 - (len(bs) & 3))
        nwords = len(bs) // 4
        out.append(0x20000000 | (nwords << 16) | ((0x2800 + 0x80 * i) >> 2))
        if len(bs) >= 4:
            out += list(struct.unpack_from("<%dI" % nwords, bs))
    return out


def driver(nso, block):
    """The words the driver's own loop writes for block."""
    mem = a64run.Mem(nso)
    cpu = a64run.Cpu(nso, mem)
    cpu.natives = {0x790FB0: a64run.memcpy, 0x791190: a64run.memset}
    blk, buf, stack = 0x10_0000_0000, 0x20_0000_0000, 0x30_0000_8000
    for k, b in enumerate(block):
        mem.b[blk + k] = b
    cpu.x[0] = buf
    cpu.x[20] = blk
    cpu.x[29] = stack
    cpu.x[31] = stack - 0x400
    cpu.run(0x619A84, lr_stop=0x619DF8)
    end = max((a for a in mem.b if buf <= a < buf + 0x10000), default=buf - 1) + 1
    n = (end - buf + 3) // 4
    return [mem.read(buf + 4 * k, 4)[0] for k in range(n)]


def random_block(r):
    b = bytearray(0x614)
    b[0x610] = r.randint(1, 4)
    b[0x612] = r.random() < 0.2
    for i in range(4):
        struct.pack_into("<H", b, 0x600 + 2 * i, r.choice([0, 4, 12, 16, 32, 48, 0x7C, r.randrange(0, 0x800, 4)]))
        b[0x608 + i] = r.randrange(4)
        c = r.randint(0, 12)
        b[0x60C + i] = c
        for j in range(c):
            b[0x80 * i + j] = r.choice([0, 1, 6, 7, 32, 33, 40, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF, r.randrange(128)])
            b[0x200 + 0x80 * i + j] = r.randrange(4)
            b[0x400 + 0x80 * i + j] = r.randint(1, 4)
    return bytes(b)


def main():
    nso = a64run.Nso(sys.argv[2])
    _, tab = table_of(nso)
    if sys.argv[1] == "check":
        r = random.Random(7)
        bad = 0
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 200
        for t in range(n):
            blk = random_block(r)
            want, got = driver(nso, blk), emulate(blk, tab)
            if want != got[: len(want)] or len(got) != len(want):
                bad += 1
                if bad <= 3:
                    print("case %d differs:\n driver %s\n emulate %s" % (t, [hex(x) for x in want],
                                                                       [hex(x) for x in got]))
        print("%d cases, %d differ" % (n, bad))
    elif sys.argv[1] == "table":
        vals = [tab(a) for a in range(-2, 128)]
        print("/* attr -2 .. 127 */")
        for k in range(0, len(vals), 16):
            print("    " + ", ".join("0x%02x" % v for v in vals[k: k + 16]) + ",")


if __name__ == "__main__":
    main()
