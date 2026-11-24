#!/usr/bin/env python3
"""Reads heapsample dumps (heapsample.c): the call sites holding the most live bytes, symbolized with addr2line
through each dump's own maps. Given two dumps, the sites ranked by how much they grew from the first to the second.

    report.py DUMP [LATER_DUMP] [--top N] [--frames N]
"""
import argparse
import bisect
import subprocess
import sys


def read(path):
    sites, maps, cur, in_maps = [], [], None, False
    with open(path) as f:
        for line in f:
            if line.startswith("maps"):
                in_maps = True
                continue
            if in_maps:
                parts = line.split()
                if len(parts) >= 6 and "x" in parts[1]:
                    lo, hi = (int(x, 16) for x in parts[0].split("-"))
                    maps.append((lo, hi, int(parts[2], 16), parts[5]))
                continue
            if line.startswith("site "):
                w = line.split()
                cur = {"live": int(w[3]), "total": int(w[5]), "pcs": []}
                sites.append(cur)
            elif line.startswith("  0x") and cur is not None:
                cur["pcs"].append(int(line.strip(), 16))
    maps.sort()
    return sites, maps


def where(pc, maps, starts):
    i = bisect.bisect_right(starts, pc) - 1
    if i >= 0 and maps[i][0] <= pc < maps[i][1]:
        lo, _, off, path = maps[i]
        return path, pc - lo + off
    return None, pc


def symbolize(pcs, maps):
    starts = [m[0] for m in maps]
    by_file, out = {}, {}
    for pc in pcs:
        path, off = where(pc, maps, starts)
        if path:
            by_file.setdefault(path, set()).add(off)
    for path, offs in by_file.items():
        offs = sorted(offs)
        r = subprocess.run(["addr2line", "-f", "-C", "-e", path] + [hex(o - 1) for o in offs],
                           capture_output=True, text=True)
        lines = r.stdout.splitlines()
        for k, o in enumerate(offs):
            fn = lines[2 * k] if 2 * k < len(lines) else "??"
            if fn == "??":
                fn = "%s+0x%x" % (path.rsplit("/", 1)[-1], o)
            out[(path, o)] = fn
    def name(pc):
        path, off = where(pc, maps, starts)
        return out.get((path, off), hex(pc))
    return name


def key(site):
    return tuple(site["pcs"][:8])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("later", nargs="?")
    ap.add_argument("--top", type=int, default=10)
    ap.add_argument("--frames", type=int, default=10)
    a = ap.parse_args()
    s1, maps = read(a.dump)
    rows = [(s["live"], s) for s in s1]
    title = "live"
    if a.later:
        s2, maps = read(a.later)
        before = {key(s): s["live"] for s in s1}
        rows = [(s["live"] - before.get(key(s), 0), s) for s in s2]
        title = "grew"
    rows.sort(key=lambda r: -r[0])
    rows = rows[: a.top]
    name = symbolize([pc for _, s in rows for pc in s["pcs"][: a.frames]], maps)
    for n, s in rows:
        print("%s %.1f MiB (live %.1f MiB)" % (title, n / 1048576, s["live"] / 1048576))
        for pc in s["pcs"][: a.frames]:
            print("   ", name(pc))
    return 0


if __name__ == "__main__":
    sys.exit(main())
