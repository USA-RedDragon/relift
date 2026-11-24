#!/usr/bin/env python3
"""Reads the FSR 2 and FSR 3 debug dumps of relift's upscaler stage (RELIFT_UPSCALER_DUMP, runtime/core/ffx.h).

  python3 -I fsr_dump.py list DIR
  python3 -I fsr_dump.py png DIR [--port fsr2] [--eval N]
  python3 -I fsr_dump.py probe DIR --eval N --at X,Y [--port fsr2] [--radius R]

A dump is one <port>-<evaluation>-<name>.raw per FidelityFX resource and one <port>-manifest.txt line each:
evaluation, name, Vulkan format, width, height, render width and height, whether it is a render-size image, file,
then the frame's constants. `png`
writes <name>.png next to each raw: every channel scaled by its own 0.5 and 99.5 percentiles into gray (one channel),
red-green (two) or the color itself (four, tonemapped when above 1); the lock status shows its lifetime in red and
its luma in green. `probe` prints, for display pixel X, Y (render-size images at the same point of the picture), every
channel of every image of that evaluation, over a square of the given radius when one is asked.

Needs numpy and Pillow. Nothing else.
"""

import argparse
import os
import sys

import numpy as np
from PIL import Image

# Vulkan format: (numpy type, channels, scale to [0, 1] for unorm, reinterpret as float32)
FORMATS = {
    9: (np.uint8, 1, 255.0, False),       # R8_UNORM
    16: (np.uint8, 2, 255.0, False),      # R8G8_UNORM
    37: (np.uint8, 4, 255.0, False),      # R8G8B8A8_UNORM
    76: (np.float16, 1, None, False),     # R16_SFLOAT
    83: (np.float16, 2, None, False),     # R16G16_SFLOAT
    97: (np.float16, 4, None, False),     # R16G16B16A16_SFLOAT
    98: (np.uint32, 1, None, True),       # R32_UINT (FSR stores depth bits)
    100: (np.float32, 1, None, False),    # R32_SFLOAT
    103: (np.float32, 2, None, False),    # R32G32_SFLOAT
    109: (np.float32, 4, None, False),    # R32G32B32A32_SFLOAT
}


def manifest(folder, port):
    rows = []
    path = os.path.join(folder, port + "-manifest.txt")
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) < 9:
                continue
            rows.append({
                "eval": int(p[0]), "name": p[1], "format": int(p[2]), "w": int(p[3]), "h": int(p[4]),
                "rw": int(p[5]), "rh": int(p[6]), "render": p[7] == "render", "file": os.path.join(folder, p[8]),
                "consts": [int(x, 16) for x in p[9:]],
            })
    return rows


def load(row):
    """The image of row as float32 of shape (h, w, channels)."""
    kind, ch, unorm, as_float = FORMATS[row["format"]]
    a = np.fromfile(row["file"], dtype=kind)
    a = a[: row["w"] * row["h"] * ch].reshape(row["h"], row["w"], ch)
    if as_float:
        return a.view(np.float32).astype(np.float32)
    a = a.astype(np.float32)
    return a / unorm if unorm else a


def to_png(row, a, out):
    if row["render"]:
        a = a[: row["rh"], : row["rw"]]
    ch = a.shape[2]
    if "upscaled" in row["name"] or "prepared_input" in row["name"]:
        c = a[..., :3]
        c = c / (1.0 + c)
        img = (np.clip(c, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)
    else:
        planes = []
        for k in range(min(ch, 3)):
            v = a[..., k]
            lo, hi = np.percentile(v, [0.5, 99.5])
            planes.append(np.clip((v - lo) / (hi - lo if hi > lo else 1.0), 0, 1))
        while len(planes) < 3:
            planes.append(planes[0] if ch == 1 else np.zeros_like(planes[0]))
        img = (np.stack(planes, -1) * 255).astype(np.uint8)
    Image.fromarray(img).save(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["list", "png", "probe"])
    ap.add_argument("dir")
    ap.add_argument("--port", default="fsr2")
    ap.add_argument("--eval", type=int)
    ap.add_argument("--at")
    ap.add_argument("--radius", type=int, default=0)
    a = ap.parse_args()
    rows = manifest(a.dir, a.port)
    if a.eval is not None:
        rows = [r for r in rows if r["eval"] == a.eval]
    if a.cmd == "list":
        for r in rows:
            print(r["eval"], r["name"], r["format"], r["w"], r["h"], "render", r["rw"], r["rh"])
        return 0
    if a.cmd == "png":
        for r in rows:
            if r["w"] < 2:
                continue
            out = r["file"][:-4] + ".png"
            to_png(r, load(r), out)
            print(out)
        return 0
    x, y = (int(v) for v in a.at.split(","))
    dw, dh = max(r["w"] for r in rows), max(r["h"] for r in rows)
    for r in rows:
        img = load(r)
        if r["w"] == 1:
            print("%-40s %s" % (r["name"], " ".join("%.4g" % v for v in img[0, 0])))
            continue
        if r["render"]:
            px, py = int((x + 0.5) * r["rw"] / dw), int((y + 0.5) * r["rh"] / dh)
        else:
            px, py = int((x + 0.5) * r["w"] / dw), int((y + 0.5) * r["h"] / dh)
        rad = a.radius
        win = img[max(0, py - rad):py + rad + 1, max(0, px - rad):px + rad + 1]
        cells = " | ".join(",".join("%.3g" % v for v in win[j, i]) for j in range(win.shape[0])
                           for i in range(win.shape[1]))
        print("%-40s @%d,%d %s" % (r["name"], px, py, cells))
    return 0


if __name__ == "__main__":
    sys.exit(main())
