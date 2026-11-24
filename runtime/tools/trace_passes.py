#!/usr/bin/env python3
"""Reports how much of its target each pass of an NVN frame trace works on.

  python3 -I trace_passes.py FRAME_DIR [FRAME_DIR ...] [--min 0.9] [--all] [--no-data]
  python3 -I trace_passes.py TRACE_DIR          (a folder holding frame-<n> folders: a timeline)

FRAME_DIR is a frame-<n> folder holding trace.jsonl (see nvn_trace.py). For every draw it compares the render
area (viewport, clipped by the scissor and by the target) with the bound target's full size. For every dispatch
it compares the dispatch extent (groups times the local size from specialization constants 0 to 2) with each
bound image, per axis, and, with the image data of the trace and numpy, the bounding box of the texels the
dispatch changed. Rows under --min (default 90 percent) of their target or volume are flagged with "<<". A
dispatch is flagged only when its extent is under --min of the image on every axis it does not exceed, because
many shaders tile the extent differently (more threads than pixels); the changed-texel box is the evidence
then. Draws with identical sizes in a row are merged. A mip chain shows as a run of shrinking viewports on one
target: judge those by the level, not by this flag.

The last block is the main scene render size per frame: the largest target that receives at least 20 draws, against the last draw's viewport (the present size).
With several frames it is a timeline, so a dynamic-resolution floor shows as a flat line under the present size.
Standard library, plus numpy for the changed-texel box.
"""

import argparse
import json
import math
import os
import sys

try:
    import numpy as np
except ImportError:
    np = None


def load(frame):
    out = []
    with open(os.path.join(frame, "trace.jsonl")) as f:
        for line in f:
            if line.strip():
                d = json.loads(line)
                if d.get("kind") in ("draw", "dispatch"):
                    out.append(d)
    return out


def draw_area(d, t):
    """Fraction of the target a draw's viewport (clipped by scissor and target) covers."""
    x, y, w, h = d["viewport"]
    x0, y0, x1, y1 = max(x, 0), max(y, 0), x + w, y + h
    if d.get("scissor_set"):
        sx, sy, sw, sh = d["scissor"]
        x0, y0, x1, y1 = max(x0, sx), max(y0, sy), min(x1, sx + sw), min(y1, sy + sh)
    x1, y1 = min(x1, t["width"]), min(y1, t["height"])
    cw, ch = max(x1 - x0, 0), max(y1 - y0, 0)
    return cw * ch / float(t["width"] * t["height"]), (cw, ch)


def mip_chain(cmds):
    """Command numbers of draws that are one step of a halving chain on a target (a mip downsample).

    A viewport is a step when the previous or the next draw on the same image is its double or its half.
    A lone half-size viewport is not a step, so a dynamic-resolution or clipping bug stays flagged."""
    by = {}
    for d in cmds:
        if d["kind"] == "draw" and d["targets"]:
            t = d["targets"][0]
            by.setdefault(t["vimg"], []).append((d["i"], draw_area(d, t)[1]))
    half = lambda a, b: abs(a[0] * 2 - b[0]) <= 2 and abs(a[1] * 2 - b[1]) <= 2
    out = set()
    for seq in by.values():
        for k, (i, wh) in enumerate(seq):
            for j in (k - 1, k + 1):
                if 0 <= j < len(seq) and (half(wh, seq[j][1]) or half(seq[j][1], wh)):
                    out.add(i)
    return out


def local_size(d):
    s = {k: v for k, v in d.get("specs", [])}
    return s.get(0, 1), s.get(1, 1), s.get(2, 1)


def changed_box(frame, t):
    """(x0, y0, z0, x1, y1, z1) of the texels whose bytes differ pre to post, or None, or 'same'."""
    if np is None or not t.get("pre") or not t.get("post"):
        return "unknown"
    if t["pre"] == t["post"]:
        return "same"
    try:
        a = np.fromfile(os.path.join(frame, t["pre"]), np.uint8)
        b = np.fromfile(os.path.join(frame, t["post"]), np.uint8)
    except OSError:
        return "unknown"
    w, h, dep = t["width"], t["height"], max(t.get("depth", 1), 1)
    n = w * h * dep
    if len(a) != len(b) or len(a) < n or len(a) % n:
        return "unknown"
    px = len(a) // n
    diff = (a != b).reshape(dep, h, w, px).any(axis=3)
    if not diff.any():
        return "same"
    zs, ys, xs = np.where(diff)
    return (int(xs.min()), int(ys.min()), int(zs.min()), int(xs.max()) + 1, int(ys.max()) + 1, int(zs.max()) + 1)


def fmt_dim(w, h, d=1):
    return "%dx%dx%d" % (w, h, d) if d > 1 else "%dx%d" % (w, h)


def report_frame(frame, args, rows_out):
    """Prints the pass table of one frame; returns (flag count, main scene size or None)."""
    cmds = load(frame)
    flagged = 0
    merged = []
    mips = mip_chain(cmds)
    for d in cmds:
        if d["kind"] == "draw":
            if not d["targets"]:
                continue
            t = d["targets"][0]
            frac, (cw, ch) = draw_area(d, t)
            key = ("draw", t["width"], t["height"], cw, ch)
            msg = "target %s  area %s  %.0f%%" % (fmt_dim(t["width"], t["height"]), fmt_dim(cw, ch), 100 * frac)
            flag = frac < args.min and d["i"] not in mips
            if d["i"] in mips and frac < args.min:
                msg += "  (mip chain)"
        else:
            g = d["groups"]
            lx, ly, lz = local_size(d)
            ext = (g[0] * lx, g[1] * ly, g[2] * lz)
            imgs = d.get("textures", [])
            if not imgs:
                continue
            if min(ext) == 0:
                continue
            big = min(imgs, key=lambda t: abs(math.log(ext[0] / t["width"])) + abs(math.log(ext[1] / t["height"])))
            dims = (big["width"], big["height"], max(big.get("depth", 1), 1))
            cov = [min(e, m) / float(m) for e, m in zip(ext, dims)]
            over = any(e > m * 1.1 for e, m in zip(ext, dims))
            flag = (not over) and max(cov) < args.min
            box = changed_box(frame, big) if not args.no_data else "unknown"
            if isinstance(box, tuple):
                bs = "wrote %s..%s of %s" % ((box[0], box[1], box[2]), (box[3], box[4], box[5]), fmt_dim(*dims))
                vol = ((box[3] - box[0]) * (box[4] - box[1]) * (box[5] - box[2])) / float(dims[0] * dims[1] * dims[2])
                bs += " (%.0f%% of texels)" % (100 * vol)
            else:
                bs = "unchanged (pre and post identical; traces from before the image-flag fix always show this for compute-written images)" if box == "same" else "changed texels unknown"
            key = ("dispatch", ext, dims)
            msg = "extent %s  image %s  coverage %s  %s" % (
                fmt_dim(*ext), fmt_dim(*dims), "/".join("%.0f%%" % (100 * c) for c in cov), bs)
            if ext[2] == 1 and dims[2] > 1:
                msg += "  (z may loop inside the shader)"
        if args.all or flag:
            if merged and merged[-1][0] == key and merged[-1][3] == flag:
                merged[-1][2].append(d["i"])
            else:
                merged.append([key, msg, [d["i"]], flag, d["kind"]])
    for key, msg, ids, flag, kind in merged:
        span = "%d" % ids[0] if len(ids) == 1 else "%d..%d (%d)" % (ids[0], ids[-1], len(ids))
        print("  %-9s %-14s %s%s" % (kind, span, msg, "   <<" if flag else ""))
        flagged += len(ids) if flag else 0
    counts = {}
    for d in cmds:
        if d["kind"] == "draw" and d["targets"]:
            t = d["targets"][0]
            counts[(t["width"], t["height"])] = counts.get((t["width"], t["height"]), 0) + 1
    scenes = [k for k, v in counts.items() if v >= 20]
    scene = max(scenes, key=lambda k: k[0] * k[1]) if scenes else None
    last = [d for d in cmds if d["kind"] == "draw" and d["targets"]]
    top = tuple(last[-1]["viewport"][2:4]) if last else None
    rows_out.append((os.path.basename(os.path.normpath(frame)), scene, top))
    return flagged


def find_frames(paths):
    out = []
    for p in paths:
        if os.path.exists(os.path.join(p, "trace.jsonl")):
            out.append(p)
        else:
            subs = sorted((d for d in os.listdir(p) if d.startswith("frame-")),
                          key=lambda s: int(s.split("-")[1]) if s.split("-")[1].isdigit() else 0)
            out += [os.path.join(p, d) for d in subs if os.path.exists(os.path.join(p, d, "trace.jsonl"))]
    return out


def main():
    ap = argparse.ArgumentParser(description="Reports the share of its target each pass of a frame trace works on.")
    ap.add_argument("frames", nargs="+")
    ap.add_argument("--min", type=float, default=0.9)
    ap.add_argument("--all", action="store_true", help="list every pass, not only the flagged ones")
    ap.add_argument("--no-data", action="store_true", help="skip the changed-texel box (no image files read)")
    args = ap.parse_args()
    frames = find_frames(args.frames)
    if not frames:
        sys.exit("no frame folder with trace.jsonl")
    rows, total = [], 0
    for f in frames:
        print("%s:" % f)
        total += report_frame(f, args, rows)
    print("\nmain scene render size per frame (largest target with at least 20 draws, against the last draw's viewport):")
    base = max((r[2] for r in rows if r[2]), key=lambda k: k[0] * k[1], default=None)  # last draw's viewport
    for name, scene, top in rows:
        if scene and base:
            per_axis = min(scene[0] / float(base[0]), scene[1] / float(base[1]))
            print("  %-14s scene %s of present %s: %.0f%% of the area, %.0f%% per axis%s" % (
                name, fmt_dim(*scene), fmt_dim(*base), 100.0 * scene[0] * scene[1] / (base[0] * base[1]),
                100 * per_axis, "   <<" if per_axis < args.min else ""))
        else:
            print("  %-14s no scene pass found" % name)
    print("flagged passes: %d (a scene render under --min of the present size per axis is flagged too)" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
