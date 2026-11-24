#!/usr/bin/env python3
"""Reads what the flash trigger saved (RELIFT_NVN_FLASH, runtime/guests/switch/nvn/nvn_flash.c) and names the first
image whose picture jumped in the flashed present.

    nvn_flash.py <dir>/flash-<n> [--min 8] [--all] [--lines 6]

Each thumbnail of present n is matched with the same write in presents n - 1 and n + 1: the same writer, image
storage, format, level and layer, counted in the order the frame wrote them. A write whose thumbnail differs from
n - 1's by at least --min luminance steps while n + 1's is close to n - 1's (at most half that difference) took the
flash. The first one is the image that first held the wrong picture; the trace lines before it (the ring's
trace-<n>.jsonl, up to the tile's "after" count) show what wrote it and what that command read. --all lists every
flashed write, not only the first. Python standard library only.
"""
import argparse
import json
import os
import struct
import sys
import zlib


def read_png(path):
    """Returns (width, height, rows) of an 8-bit RGB or RGBA PNG; rows are lists of (r, g, b)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(path + ": not a PNG")
    pos, idat, w, h, ctype = 8, b"", 0, 0, 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            if depth != 8 or ctype not in (2, 6):
                raise ValueError(path + ": only 8-bit RGB or RGBA")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev, at = [], bytearray(stride), 0
    for _ in range(h):
        ft, line = raw[at], bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 1:
                line[i] = (line[i] + a) & 255
            elif ft == 2:
                line[i] = (line[i] + b) & 255
            elif ft == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, h, [[(r[i], r[i + 1], r[i + 2]) for i in range(0, stride, bpp)] for r in rows]


def luma(p):
    return (299 * p[0] + 587 * p[1] + 114 * p[2] + 500) // 1000


def tiles(folder, n, tw, th, cols):
    """The records of present n with each tile's luminance values attached (None without the files)."""
    jpath = os.path.join(folder, "thumbs-%06d.jsonl" % n)
    ppath = os.path.join(folder, "thumbs-%06d.png" % n)
    if not os.path.exists(jpath) or not os.path.exists(ppath):
        return None
    recs = [json.loads(line) for line in open(jpath) if line.strip()]
    recs = [r for r in recs if "tile" in r]
    _, h, rows = read_png(ppath)
    for r in recs:
        x0, y0 = (r["tile"] % cols) * tw, (r["tile"] // cols) * th
        if y0 + th > h:
            r["px"] = None
            continue
        r["px"] = [luma(rows[y][x]) for y in range(y0, y0 + th) for x in range(x0, x0 + tw)]
    return recs


def key_all(recs):
    """Keys each write by writer, size, format, level, layer and its count among such writes: a frame that cycles
    through a ring of images (a video's decoder images, swapchain-like targets) writes a different storage each
    present, so the storage is not part of the key."""
    seen, out = {}, {}
    for r in recs:
        k = (r["what"], r["width"], r["height"], r["format"], r["level"], r["layer"])
        i = seen.get(k, 0)
        seen[k] = i + 1
        out[k + (i,)] = r
    return out


def dist(a, b):
    if a is None or b is None:
        return None
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("folder")
    ap.add_argument("--min", type=float, default=2.0)
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--lines", type=int, default=6)
    a = ap.parse_args()
    meta = json.load(open(os.path.join(a.folder, "flash.json")))
    n, tw, th, cols = meta["present"], meta["thumb_w"], meta["thumb_h"], meta["thumb_cols"]
    print("present %d: %.1f from the one before, %.1f from the one after, those two %.1f apart" %
          (n, meta["before"], meta["after"], meta["around"]))
    prev, bad, nxt = (tiles(a.folder, k, tw, th, cols) for k in (n - 1, n, n + 1))
    if bad is None or prev is None:
        print("no thumbnails of present %d or %d" % (n, n - 1))
        return 1
    kp, kn = key_all(prev), key_all(nxt or [])
    hits = 0
    for k, r in key_all(bad).items():
        d = dist(r["px"], kp[k]["px"]) if k in kp else None
        dn = dist(kn[k]["px"], kp[k]["px"]) if k in kn and k in kp else None
        if d is None or d < a.min or (dn is not None and dn > d / 2):
            continue
        hits += 1
        print("tile %d (%s after trace line %d): storage %s format %d (vk %d) %dx%d level %d layer %d: %.1f from "
              "present %d, present %d is %s from it" %
              (r["tile"], r["what"], r["after"], r["storage"], r["format"], r["vk_format"], r["width"], r["height"],
               r["level"], r["layer"], d, n - 1, n + 1, "%.1f" % dn if dn is not None else "missing"))
        tpath = os.path.join(a.folder, "trace-%06d.jsonl" % n)
        if hits == 1 and os.path.exists(tpath):
            lines = [line for line in open(tpath) if line.strip()]
            before = [line for line in lines if json.loads(line).get("i", 0) < r["after"]]
            for line in before[-a.lines:]:
                print("    " + line.rstrip()[:600])
        if not a.all:
            break
    missing = [k for k in key_all(bad) if k not in kp]
    if missing:
        print("%d writes of present %d have no match in present %d (first: %s)" % (len(missing), n, n - 1,
                                                                                     missing[0]))
    if not hits:
        print("no write's thumbnail jumped by %.1f or more" % a.min)
    return 0


if __name__ == "__main__":
    sys.exit(main())
