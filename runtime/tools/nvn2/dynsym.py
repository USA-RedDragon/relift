#!/usr/bin/env python3
"""dynsym.py <nso> [regex]: defined dynamic symbols of an uncompressed NSO (through MOD0 and its dynamic table)."""
import re
import struct
import sys

d = open(sys.argv[1], "rb").read()
pat = re.compile(sys.argv[2] if len(sys.argv) > 2 else ".")
segs = [struct.unpack_from("<III", d, o) for o in (0x10, 0x20, 0x30)]


def rd(a, n):
    for f, m, s in segs:
        if m <= a < m + s:
            return d[f + a - m: f + a - m + n]
    return b"\0" * n


mod0 = struct.unpack_from("<I", rd(4, 4))[0]
dyn = mod0 + struct.unpack_from("<i", rd(mod0 + 4, 4))[0]
tags = {}
a = dyn
while True:
    t, v = struct.unpack("<QQ", rd(a, 16))
    if t == 0:
        break
    tags.setdefault(t, v)
    a += 16
symtab, strtab, syment = tags[6], tags[5], tags.get(11, 24)
n = struct.unpack("<I", rd(tags[4] + 4, 4))[0] if 4 in tags else (strtab - symtab) // syment
for i in range(n):
    name_off, info, other, shndx, value, size = struct.unpack("<IBBHQQ", rd(symtab + i * syment, 24))
    if not value:
        continue
    e = rd(strtab + name_off, 256)
    if b"\0" not in e:
        continue
    name = e[: e.index(b"\0")].decode(errors="replace")
    if pat.search(name):
        print("%08x  %s" % (value, name))


def reloc_target(addr):
    """The symbol a JUMP_SLOT/GLOB_DAT relocation at addr names (DT_JMPREL, DT_RELA), and its value if defined."""
    for tab, sz in ((23, 2), (7, 8)):
        if tab not in tags:
            continue
        base = tags[tab]
        total = tags.get(sz, 0)
        for o in range(0, total, 24):
            off, info, add = struct.unpack("<QQq", rd(base + o, 24))
            if off == addr:
                si = info >> 32
                name_off, _, _, _, value, _ = struct.unpack("<IBBHQQ", rd(symtab + si * syment, 24))
                e = rd(strtab + name_off, 256)
                return e[: e.index(b"\0")].decode(errors="replace"), value
    return None
