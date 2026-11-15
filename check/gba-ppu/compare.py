#!/usr/bin/env python3
"""Draws PPU scenes (gen_scenes.py format) with relift's picture unit and
with NanoBoyAdvance (nba-oracle) and reports the differing pixels.

usage: compare.py ORACLE SCENE.scn... [--out DIR] [--frames N]
"""
import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.join(HERE, "..", "..", "runtime", "guests", "gba")


def build(work):
    exe = os.path.join(work, "ppu_scene")
    srcs = [os.path.join(HERE, "ppu_scene.c")] + [
        os.path.join(RUNTIME, "ppu", f) for f in sorted(os.listdir(os.path.join(RUNTIME, "ppu"))) if f.endswith(".c") and f not in ("ppu_dev.c", "ppu_present.c")
    ]
    subprocess.run(["clang", "-std=c11", "-O2", "-D_GNU_SOURCE", "-I" + RUNTIME, *srcs, "-o", exe], check=True)
    return exe


class Oracle:
    def __init__(self, path, rom):
        self.p = subprocess.Popen([path, "--rom", rom, "--skip-bios"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        assert self.p.stdout.readline().startswith("READY")

    def cmd(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        r = self.p.stdout.readline().strip()
        if r.startswith("ERR"):
            raise RuntimeError(line[:40] + ": " + r)
        return r

    def close(self):
        self.cmd("QUIT")


def nba_frame(path, rom, scn, frames):
    d = open(scn, "rb").read()
    o = Oracle(path, rom)
    o.cmd("RUN frames 1")
    o.cmd("POKE 05000000 " + d[1032:2056].hex())
    for off in range(0, 0x18000, 0x4000):
        o.cmd("POKE %08X %s" % (0x06000000 + off, d[2056 + off : 2056 + off + 0x4000].hex()))
    o.cmd("POKE 07000000 " + d[100360:101384].hex())
    for a in range(0, 0x60, 2):
        if 4 <= a < 8:
            continue
        o.cmd("IOPOKE %X %s" % (a, d[8 + a : 10 + a].hex()))
    o.cmd("RUN frames %d" % frames)
    px = bytes.fromhex(o.cmd("PIXELS").split()[1])
    o.close()
    return px


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("oracle")
    ap.add_argument("scenes", nargs="+")
    ap.add_argument("--out")
    ap.add_argument("--frames", type=int, default=4)
    a = ap.parse_args()
    with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as work:
        exe = build(work)
        rom = os.path.join(work, "idle.gba")
        open(rom, "wb").write(bytes.fromhex("feffffea") * 64)
        for scn in a.scenes:
            name = os.path.splitext(os.path.basename(scn))[0]
            ours = os.path.join(work, name + ".rgb555")
            subprocess.run([exe, scn, ours, str(a.frames + 2)], check=True)
            mine = open(ours, "rb").read()
            theirs = nba_frame(a.oracle, rom, scn, a.frames)
            diff = [i // 2 for i in range(0, len(mine), 2) if mine[i : i + 2] != theirs[i : i + 2]]
            print("%-24s %6d of %d pixels differ" % (name, len(diff), len(mine) // 2))
            if a.out:
                os.makedirs(a.out, exist_ok=True)
                open(os.path.join(a.out, name + ".relift.rgb555"), "wb").write(mine)
                open(os.path.join(a.out, name + ".nba.rgb555"), "wb").write(theirs)
                with open(os.path.join(a.out, name + ".diff.txt"), "w") as f:
                    for i in diff:
                        x, y = i % 240, i // 240
                        m = mine[2 * i] | mine[2 * i + 1] << 8
                        t = theirs[2 * i] | theirs[2 * i + 1] << 8
                        f.write("%d %d relift=%04x nba=%04x\n" % (x, y, m, t))


if __name__ == "__main__":
    main()
