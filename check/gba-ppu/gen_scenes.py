#!/usr/bin/env python3
"""Generates PPU scenes for compare.py: one family per feature, several
seeds each. The file format is the spike's (GBASCN01: magic, 1024 bytes
of I/O registers, palette, 96 KiB VRAM, OAM).

usage: gen_scenes.py OUTDIR [SEEDS]
"""
import math
import os
import random
import struct
import sys


def w16(b, off, v):
    struct.pack_into("<H", b, off, v & 0xFFFF)


class Scene:
    def __init__(self, rng):
        self.rng = rng
        self.io = bytearray(0x400)
        self.pram = bytearray(rng.getrandbits(8) for _ in range(1024))
        for i in range(1, 1024, 2):
            self.pram[i] &= 0x7F
        self.vram = bytearray(0x18000)
        self.oam = bytearray(1024)
        for i in range(128):
            w16(self.oam, i * 8, 0x0200)

    def reg(self, off, v):
        w16(self.io, off, v)

    def tiles(self, lo, hi, zero=0.3):
        r = self.rng
        for a in range(lo, hi):
            b = r.getrandbits(8)
            if r.random() < zero:
                b &= 0xF0
            if r.random() < zero:
                b &= 0x0F
            self.vram[a] = b

    def text_map(self, sbb, blocks, tiles):
        r = self.rng
        for i in range(blocks * 1024):
            w16(self.vram, sbb * 0x800 + i * 2, r.randrange(tiles) | (r.getrandbits(2) << 10) | (r.randrange(16) << 12))

    def affine_map(self, sbb, size_tiles):
        for i in range(size_tiles * size_tiles):
            self.vram[sbb * 0x800 + i] = self.rng.randrange(256)

    def objs(self, n, tile_lo=0, tile_hi=1024, affine=0.25, mosaic=0.1, modes=(0, 0, 0, 1, 2)):
        r = self.rng
        for g in range(32):
            ang = r.uniform(0, 2 * math.pi)
            sx, sy = r.uniform(0.5, 1.8), r.uniform(0.5, 1.8)
            for k, v in enumerate((math.cos(ang) / sx, -math.sin(ang) / sx, math.sin(ang) / sy, math.cos(ang) / sy)):
                w16(self.oam, g * 32 + 6 + k * 8, int(round(v * 256)))
        for i in range(n):
            y = r.randrange(0, 228)
            x = r.randrange(0, 512)
            shape = r.randrange(3)
            size = r.randrange(4)
            aff = r.random() < affine
            dbl = aff and r.random() < 0.4
            mode = r.choice(modes)
            a0 = y | (int(aff) << 8) | (int(dbl) << 9) | (mode << 10) | (int(r.random() < mosaic) << 12) | (r.getrandbits(1) << 13) | (shape << 14)
            a1 = x | (size << 14) | ((r.randrange(32) << 9) if aff else (r.getrandbits(2) << 12))
            a2 = r.randrange(tile_lo, tile_hi) | (r.randrange(4) << 10) | (r.randrange(16) << 12)
            w16(self.oam, i * 8, a0)
            w16(self.oam, i * 8 + 2, a1)
            w16(self.oam, i * 8 + 4, a2)

    def fx(self, mode=None):
        r = self.rng
        self.reg(0x40, (r.randrange(0, 120) << 8) | r.randrange(100, 256))
        self.reg(0x42, (r.randrange(0, 120) << 8) | r.randrange(100, 256))
        self.reg(0x44, (r.randrange(0, 80) << 8) | r.randrange(80, 160))
        self.reg(0x46, (r.randrange(0, 80) << 8) | r.randrange(80, 228))
        self.reg(0x48, r.getrandbits(14) & 0x3F3F)
        self.reg(0x4A, r.getrandbits(14) & 0x3F3F)
        self.reg(0x4C, r.getrandbits(16) if r.random() < 0.5 else 0)
        m = r.randrange(4) if mode is None else mode
        self.reg(0x50, (r.getrandbits(14) & 0x3F3F) | (m << 6))
        self.reg(0x52, (r.randrange(0, 24) << 8) | r.randrange(0, 24))
        self.reg(0x54, r.randrange(0, 24))

    def bytes(self):
        return b"GBASCN01" + bytes(self.io) + bytes(self.pram) + bytes(self.vram) + bytes(self.oam)


def text_bgs(s, mode, nbg, win=False, objs=0, blend=False):
    r = s.rng
    s.tiles(0, 0x10000)
    s.tiles(0x10000, 0x18000)
    en = 0
    for bg in range(nbg):
        sbb = 16 + bg * 3
        size = r.randrange(4)
        blocks = (1, 2, 2, 4)[size]
        s.text_map(sbb, blocks, 512)
        cbb = r.randrange(2)
        bpp8 = r.getrandbits(1)
        s.reg(0x08 + 2 * bg, r.randrange(4) | (cbb << 2) | (int(r.random() < 0.3) << 6) | (bpp8 << 7) | (sbb << 8) | (size << 14))
        s.reg(0x10 + 4 * bg, r.randrange(512))
        s.reg(0x12 + 4 * bg, r.randrange(512))
        en |= 1 << bg
    dc = mode | (en << 8) | (0x40 if r.random() < 0.7 else 0)
    if objs:
        dc |= 0x1000
        s.objs(objs)
    if win:
        dc |= r.choice([0x2000, 0x4000, 0x6000, 0xA000, 0xE000, 0x8000])
    s.fx()
    if not blend:
        s.reg(0x50, 0)
    s.reg(0x00, dc)


def affine_bg(s, bg, size):
    r = s.rng
    sbb = 24 + bg * 2
    s.affine_map(sbb, 16 << size)
    s.reg(0x08 + 2 * bg, r.randrange(4) | (r.randrange(2) << 2) | (int(r.random() < 0.3) << 6) | (sbb << 8) | (r.getrandbits(1) << 13) | (size << 14))
    ang = r.uniform(0, 2 * math.pi)
    sc = r.uniform(0.5, 2.0)
    base = 0x20 if bg == 2 else 0x30
    for k, v in enumerate((math.cos(ang) / sc, -math.sin(ang) / sc, math.sin(ang) / sc, math.cos(ang) / sc)):
        s.reg(base + 2 * k, int(round(v * 256)))
    struct.pack_into("<I", s.io, base + 8, r.randrange(0, 1 << 20) | ((r.getrandbits(1) * 0xF) << 24))
    struct.pack_into("<I", s.io, base + 12, r.randrange(0, 1 << 20) | ((r.getrandbits(1) * 0xF) << 24))


def make(family, rng):
    s = Scene(rng)
    r = rng
    if family == "text_plain":
        text_bgs(s, 0, r.randrange(1, 5))
    elif family == "text_obj":
        text_bgs(s, 0, r.randrange(1, 5), objs=r.randrange(1, 40))
    elif family == "text_win":
        text_bgs(s, 0, r.randrange(1, 5), win=True, objs=r.randrange(0, 24))
    elif family == "text_blend":
        text_bgs(s, 0, r.randrange(2, 5), win=r.random() < 0.5, objs=r.randrange(0, 24), blend=True)
    elif family == "mode1":
        text_bgs(s, 1, 2, objs=r.randrange(0, 24), win=r.random() < 0.5, blend=True)
        affine_bg(s, 2, r.randrange(4))
        s.reg(0x00, (s.io[0] | s.io[1] << 8) | 0x0400)
    elif family == "mode2":
        s.tiles(0, 0x10000)
        s.tiles(0x10000, 0x18000)
        affine_bg(s, 2, r.randrange(4))
        affine_bg(s, 3, r.randrange(4))
        dc = 2 | 0x0C00 | 0x40
        if r.random() < 0.6:
            dc |= 0x1000
            s.objs(r.randrange(1, 24))
        if r.random() < 0.5:
            dc |= r.choice([0x2000, 0x4000, 0xA000, 0xE000])
        s.fx()
        s.reg(0x00, dc)
    elif family in ("mode3", "mode4", "mode5"):
        mode = int(family[-1])
        for i in range(0, 0x14000, 2):
            w16(s.vram, i, r.getrandbits(16) if mode != 4 else (r.getrandbits(16) & (0xFFFF if r.random() < 0.8 else 0xFF00)))
        s.tiles(0x14000, 0x18000)
        for k, v in enumerate((0x100, 0, 0, 0x100) if r.random() < 0.6 else (0x80, 0x20, -0x20, 0x90)):
            s.reg(0x20 + 2 * k, v)
        dc = mode | 0x0400 | 0x40 | (0x10 if mode > 3 and r.getrandbits(1) else 0)
        if r.random() < 0.7:
            dc |= 0x1000
            s.objs(r.randrange(1, 20), tile_lo=512)
        if r.random() < 0.4:
            dc |= r.choice([0x2000, 0x4000, 0xA000])
        s.fx()
        s.reg(0x0C, r.randrange(4) | (int(r.random() < 0.3) << 6))
        s.reg(0x00, dc)
    elif family == "obj_only":
        s.tiles(0x10000, 0x18000)
        s.objs(r.randrange(1, 128), affine=r.random(), mosaic=r.random() * 0.3)
        s.fx()
        s.reg(0x4C, r.getrandbits(16))
        s.reg(0x00, 0x1000 | (0x40 if r.random() < 0.5 else 0) | r.choice([0, 0x2000, 0x8000, 0xA000]))
    elif family == "obj_2d":
        s.tiles(0x10000, 0x18000)
        s.objs(r.randrange(1, 40))
        s.reg(0x00, 0x1000)
    elif family == "blank":
        text_bgs(s, 0, 2)
        s.reg(0x00, (s.io[0] | s.io[1] << 8) | 0x80)
    elif family == "mode_invalid":
        s.tiles(0, 0x18000)
        s.text_map(16, 1, 512)
        s.reg(0x08, 16 << 8)
        s.reg(0x00, r.choice([6, 7]) | 0x1F00)
    else:
        raise SystemExit("family " + family)
    return s


FAMILIES = ["text_plain", "text_obj", "text_win", "text_blend", "mode1", "mode2", "mode3", "mode4", "mode5", "obj_only", "obj_2d", "blank", "mode_invalid"]


def main():
    out = sys.argv[1]
    seeds = int(sys.argv[2]) if len(sys.argv) > 2 else 4
    os.makedirs(out, exist_ok=True)
    for f in FAMILIES:
        for k in range(seeds):
            rng = random.Random("%s-%d" % (f, k))
            s = make(f, rng)
            with open(os.path.join(out, "%s_%d.scn" % (f, k)), "wb") as fh:
                fh.write(s.bytes())
    print(len(FAMILIES) * seeds, "scenes in", out)


if __name__ == "__main__":
    main()
