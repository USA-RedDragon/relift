#!/usr/bin/env python3
"""clsnames.py: method names of a 3D class header (such as Mesa's clc797.h), by method index (byte offset / 4).

An array's length is not in the header: it is taken as the room up to the first define at or past one stride from its
base, so an array never names the methods of the next group.
"""
import os
import re

#: The class header to read: $RELIFT_CLASS_HEADER, Mesa's src/nouveau/headers/nvidia/classes/clc797.h (MIT).
HDR = os.environ.get("RELIFT_CLASS_HEADER", "")


def load(path=HDR, prefix="NVC797_"):
    """Method index to name; empty when no header is given."""
    if not path or not os.path.isfile(path):
        return {}
    one = re.compile(r"#define %s([A-Z0-9_]+)\s+(0x[0-9a-fA-F]{4})\s*$" % prefix)
    arr = re.compile(r"#define %s([A-Z0-9_]+)\(([a-z, ]+)\)\s+\((0x[0-9a-fA-F]+)((?:\+\([a-z]\)\*\d+)+)\)" % prefix)
    scalars, arrays = {}, []
    for line in open(path):
        m = arr.match(line)
        if m:
            terms = [int(s) for _, s in re.findall(r"\(([a-z])\)\*(\d+)", m.group(4))]
            arrays.append((m.group(1), int(m.group(3), 16), terms))
            continue
        m = one.match(line)
        if m:
            scalars.setdefault(int(m.group(2), 16), m.group(1))
    bases = sorted(set(scalars) | {b for _, b, _ in arrays})
    names = {o // 4: n for o, n in scalars.items() if o % 4 == 0}
    for name, base, terms in arrays:
        outer = max(terms)
        end = next((b for b in bases if b >= base + outer), base + outer * 64)
        count = [max(1, (end - base) // s) if s == outer else max(1, outer // s) for s in terms]

        def rec(k, off, idx):
            if k == len(terms):
                if off % 4 == 0 and off < end:
                    names.setdefault(off // 4, "%s(%s)" % (name, ",".join(map(str, idx))))
                return
            for i in range(count[k]):
                rec(k + 1, off + i * terms[k], idx + [i])

        rec(0, base, [])
    return names
