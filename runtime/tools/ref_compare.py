#!/usr/bin/env python3
"""Matches relift captures to the exact moment of a console reference video and compares them.

  python3 -I ref_compare.py index VIDEO [--fps 10]
  python3 -I ref_compare.py match CAPTURE_OR_DIR --ref VIDEO [--out DIR] [--monotonic] [--min-score 0.30]
                                  [--from PRESENT] [--to PRESENT] [--every N] [--at SECONDS --window S]

`index` decodes the video at a fixed rate (default 10 fps) into small grayscale thumbnails plus a perceptual
hash and stores them next to the video as VIDEO.refidx.npz. `match` finds, for each capture, the video time
whose thumbnail is closest, refines it at 60 fps around that time, and reports the time, the score and the
best other moment (at least 1 s away), so an ambiguous match shows. A capture directory is a sequence whose
file names carry the present (frame-<present>-...): with --monotonic the times may only increase, which fixes
scene cuts and repeated scenery. Per capture it writes <name>.cmp.png (console left, relift right at the same
size, then luminance, saturation and detail difference heatmaps with a 3x3 grid of numbers) and a text summary
per region; match.json collects the numbers.

The descriptor is the locally contrast-normalized gradient field of a blurred thumbnail, compared as a cosine
similarity, so a brightness shift, a gain change, blur and fog change it little. Scores below --min-score
mean no comparison: the capture shows another moment. Judge from the difference panel and by looking at the
matched pair, never from the score alone.

Needs numpy, Pillow and ffmpeg on PATH. Nothing else.
"""

import argparse
import json
import os
import re
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

TW, TH = 96, 54
REFINE_FPS = 60
DEFAULT_MASK = [(0.74, 0.86, 1.0, 1.0)]
REGIONS = [
    "top-left", "top", "top-right",
    "left", "center", "right",
    "bottom-left", "bottom", "bottom-right",
]


# ---------------------------------------------------------------- descriptors


def box_blur(a, r):
    """Mean filter of radius r over the last two axes (edge-replicated)."""
    if r <= 0:
        return a
    k = 2 * r + 1
    p = [(0, 0)] * (a.ndim - 2) + [(r, r), (r, r)]
    x = np.pad(a, p, mode="edge")
    c = np.cumsum(x, axis=-1, dtype=np.float64)
    c = np.concatenate([np.zeros(c.shape[:-1] + (1,)), c], axis=-1)
    x = (c[..., k:] - c[..., :-k]) / k
    c = np.cumsum(x, axis=-2)
    c = np.concatenate([np.zeros(c.shape[:-2] + (1, c.shape[-1])), c], axis=-2)
    return (c[..., k:, :] - c[..., :-k, :]) / k


def mask_array(masks):
    """A (TH, TW) 0/1 array that is 0 inside each fractional box (x0, y0, x1, y1)."""
    m = np.ones((TH, TW), np.float64)
    for x0, y0, x1, y1 in masks:
        m[int(y0 * TH):int(np.ceil(y1 * TH)), int(x0 * TW):int(np.ceil(x1 * TW))] = 0
    return m


def descriptors(thumbs, masks):
    """Unit vectors (N, D) from gray thumbnails (N, TH, TW) as uint8 or float."""
    t = box_blur(np.asarray(thumbs, np.float64), 1)
    gx = np.zeros_like(t)
    gy = np.zeros_like(t)
    gx[:, :, 1:-1] = t[:, :, 2:] - t[:, :, :-2]
    gy[:, 1:-1, :] = t[:, 2:, :] - t[:, :-2, :]
    mag = np.sqrt(gx * gx + gy * gy)
    loc = box_blur(mag, 5) + 0.35 * mag.mean(axis=(1, 2), keepdims=True) + 1e-3
    m = mask_array(masks)
    m[:1, :] = m[-1:, :] = 0
    m[:, :1] = m[:, -1:] = 0
    d = np.concatenate([(gx / loc * m).reshape(len(t), -1), (gy / loc * m).reshape(len(t), -1)], axis=1)
    n = np.linalg.norm(d, axis=1, keepdims=True)
    return d / np.maximum(n, 1e-9)


def phash(thumbs):
    """64-bit difference hash per thumbnail (as uint64), robust to brightness."""
    t = np.asarray(thumbs, np.float64)
    out = []
    for f in t:
        s = np.asarray(Image.fromarray(f.astype(np.uint8)).resize((9, 8), Image.BOX), np.float64)
        bits = (s[:, 1:] > s[:, :-1]).flatten()
        out.append(int("".join("1" if b else "0" for b in bits), 2))
    return np.array(out, np.uint64)


def gray_thumb(img):
    """Gray TH x TW uint8 thumbnail of a PIL image, like ffmpeg's area scale of the luma."""
    return np.asarray(img.convert("L").resize((TW, TH), Image.BOX), np.uint8)


# ---------------------------------------------------------------- ffmpeg


def ffmpeg_thumbs(video, fps, start=None, dur=None):
    cmd = ["ffmpeg", "-v", "error", "-nostdin"]
    if start is not None:
        cmd += ["-ss", "%.3f" % max(start, 0.0)]
    cmd += ["-i", video]
    if dur is not None:
        cmd += ["-t", "%.3f" % dur]
    cmd += ["-vf", "fps=%g,scale=%d:%d:flags=area,format=gray" % (fps, TW, TH), "-f", "rawvideo", "-"]
    raw = subprocess.run(cmd, check=True, stdout=subprocess.PIPE).stdout
    n = len(raw) // (TW * TH)
    return np.frombuffer(raw[: n * TW * TH], np.uint8).reshape(n, TH, TW)


def ffmpeg_frame(video, t, size):
    cmd = ["ffmpeg", "-v", "error", "-nostdin", "-ss", "%.3f" % max(t, 0.0), "-i", video, "-frames:v", "1",
           "-vf", "scale=%d:%d:flags=lanczos" % size, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]
    raw = subprocess.run(cmd, check=True, stdout=subprocess.PIPE).stdout
    return Image.frombuffer("RGB", size, raw, "raw", "RGB", 0, 1)


def video_duration(video):
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", video],
                         check=True, stdout=subprocess.PIPE, text=True).stdout
    return float(out.strip())


# ---------------------------------------------------------------- index


def index_path(video):
    return video + ".refidx.npz"


def build_index(video, fps=10.0):
    th = ffmpeg_thumbs(video, fps)
    np.savez_compressed(index_path(video), thumbs=th, phash=phash(th), fps=np.float64(fps))
    return index_path(video)


def load_index(video):
    p = index_path(video)
    if not os.path.exists(p):
        build_index(video)
    z = np.load(p)
    return z["thumbs"], z["phash"], float(z["fps"])


# ---------------------------------------------------------------- matching


def peaks(scores, k, sep):
    """Up to k (index, score) peaks of a score series, at least sep samples apart, best first."""
    order = np.argsort(-scores)
    out = []
    for i in order:
        if all(abs(int(i) - j) >= sep for j, _ in out):
            out.append((int(i), float(scores[i])))
            if len(out) == k:
                break
    return out


def hamming(a, b):
    return bin(int(a) ^ int(b)).count("1")


def match_scores(idx_desc, cap_desc):
    return idx_desc @ cap_desc


def refine(video, cap_desc, t0, masks, half=0.6):
    """Best time and score near t0 from a 60 fps decode of [t0 - half, t0 + half]."""
    start = max(t0 - half, 0.0)
    th = ffmpeg_thumbs(video, REFINE_FPS, start, 2 * half)
    if len(th) == 0:
        return t0, -1.0
    s = descriptors(th, masks) @ cap_desc
    k = int(np.argmax(s))
    return start + k / REFINE_FPS, float(s[k])


def viterbi(cands, tol=0.15):
    """Picks one candidate (time, score) per step so times never decrease, maximizing the summed score."""
    n = len(cands)
    best = [[c[1] for c in cands[0]]]
    back = [[-1] * len(cands[0])]
    for i in range(1, n):
        row, brow = [], []
        for t, s in cands[i]:
            bj, bv = -1, -1e9
            for j, (tp, _) in enumerate(cands[i - 1]):
                if t >= tp - tol and best[i - 1][j] > bv:
                    bj, bv = j, best[i - 1][j]
            if bj < 0:
                bv = max(best[i - 1]) - 1.0
                bj = int(np.argmax(best[i - 1]))
            row.append(bv + s)
            brow.append(bj)
        best.append(row)
        back.append(brow)
    j = int(np.argmax(best[-1]))
    path = [0] * n
    for i in range(n - 1, -1, -1):
        path[i] = j
        j = back[i][j]
    return path


def match_images(video, images, masks, monotonic=False, window=None, fps_hint=None):
    """Matches PIL images to video times; returns one dict per image."""
    thumbs, _, fps = load_index(video)
    desc = descriptors(thumbs, masks)
    times = np.arange(len(thumbs)) / fps
    allowed = np.ones(len(times), bool)
    if window is not None:
        allowed = (times >= window[0]) & (times <= window[1])
    res, cands = [], []
    for im in images:
        c = descriptors(gray_thumb(im)[None], masks)[0]
        sc = match_scores(desc, c)
        sc_w = np.where(allowed, sc, -1.0)
        pk = peaks(sc_w, 8, max(int(0.5 * fps), 1))
        res.append((sc_w, pk))
        cands.append([(i / fps, s) for i, s in pk])
    if monotonic and len(images) > 1:
        path = viterbi(cands)
    else:
        path = [0] * len(images)
    out = []
    for im, (sc_w, pk), pi, c in zip(images, res, path, cands):
        c0 = descriptors(gray_thumb(im)[None], masks)[0]
        t, s = refine(video, c0, c[pi][0], masks)
        far = [(tt, ss) for tt, ss in c if abs(tt - c[pi][0]) >= 1.0]
        t2, s2 = (far[0] if far else (None, None))
        out.append({"time": round(t, 3), "score": round(s, 4), "coarse_score": round(c[pi][1], 4),
                    "second_time": None if t2 is None else round(t2, 3),
                    "second_score": None if s2 is None else round(s2, 4),
                    "margin": None if s2 is None else round(s - s2, 4)})
    return out


# ---------------------------------------------------------------- difference panel


def luma(a):
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]


def saturation(a):
    mx, mn = a.max(axis=-1), a.min(axis=-1)
    return np.where(mx > 8, (mx - mn) / np.maximum(mx, 1), 0.0) * 100.0


def detail(y):
    """Local contrast: mean absolute difference from a 7x7 mean, per pixel."""
    return np.abs(y - box_blur(y, 3))


def region_stats(ref, cap, masks):
    """Per region luminance, saturation and detail of both images, as a list of dicts."""
    ry, cy = luma(ref), luma(cap)
    rs, cs = saturation(ref), saturation(cap)
    rd, cd = detail(ry), detail(cy)
    h, w = ry.shape
    m = np.ones((h, w), bool)
    for x0, y0, x1, y1 in masks:
        m[int(y0 * h):int(np.ceil(y1 * h)), int(x0 * w):int(np.ceil(x1 * w))] = False
    out = []
    for r in range(3):
        for c in range(3):
            sl = (slice(r * h // 3, (r + 1) * h // 3), slice(c * w // 3, (c + 1) * w // 3))
            mm = m[sl]
            f = lambda a: float(a[sl][mm].mean()) if mm.any() else 0.0
            d = {"region": REGIONS[r * 3 + c],
                 "luma_ref": f(ry), "luma_cap": f(cy),
                 "sat_ref": f(rs), "sat_cap": f(cs),
                 "detail_ref": f(rd), "detail_cap": f(cd)}
            d["luma_diff"] = d["luma_cap"] - d["luma_ref"]
            d["sat_diff"] = d["sat_cap"] - d["sat_ref"]
            d["detail_ratio"] = d["detail_cap"] / max(d["detail_ref"], 0.25)
            out.append(d)
    return out


def describe(d):
    """Plain words for one region's numbers; empty parts when the difference is small."""
    words = []
    dl = d["luma_diff"]
    rel = abs(dl) / max(d["luma_ref"], 8.0)
    if abs(dl) >= 3 and rel > 0.15:
        words.append("%s by %.0f (%.0f%%)" % ("brighter" if dl > 0 else "darker", abs(dl), 100 * rel))
    ds = d["sat_diff"]
    if abs(ds) >= 6:
        words.append("%s saturated by %.0f points" % ("more" if ds > 0 else "less", abs(ds)))
    dr = d["detail_ratio"]
    if dr > 1.4:
        words.append("crisper (detail x%.1f)" % dr)
    elif dr < 0.7:
        words.append("softer (detail x%.1f)" % dr)
    if dr > 1.4 and ds > 5:
        words.append("possible missing haze or mist (console flatter and less saturated)")
    return words


def heat(a, scale):
    """Diverging color map: negative blue, zero dark gray, positive red. a/scale in -1..1."""
    v = np.clip(a / scale, -1, 1)
    r = np.where(v > 0, 40 + 215 * v, 40 * (1 + v))
    b = np.where(v < 0, 40 - 215 * v, 40 * (1 - v))
    g = 40 * (1 - np.abs(v)) + 20 * (1 - np.abs(v))
    return np.stack([r, g, b], axis=-1).astype(np.uint8)


def font(size):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        return ImageFont.load_default()


def label(img, text, xy=(8, 6), size=22):
    d = ImageDraw.Draw(img)
    f = font(size)
    box = d.textbbox(xy, text, font=f)
    d.rectangle((box[0] - 4, box[1] - 3, box[2] + 4, box[3] + 3), fill=(0, 0, 0))
    d.text(xy, text, fill=(255, 255, 255), font=f)


def panel(ref_img, cap_img, ref_label, cap_label, masks, size=(960, 540)):
    """One image: side by side (console left, relift right) over three heatmaps with region numbers.

    Returns (image, region stats)."""
    ref = np.asarray(ref_img.resize(size, Image.LANCZOS), np.float64)
    cap = np.asarray(cap_img.resize(size, Image.LANCZOS), np.float64)
    stats = region_stats(ref, cap, masks)
    w, h = size
    ry, cy = luma(ref), luma(cap)
    maps = [
        ("luminance (relift - console)", box_blur(cy - ry, 6), 40.0, "luma_diff", "%+.0f"),
        ("saturation (relift - console)", box_blur(saturation(cap) - saturation(ref), 6), 25.0, "sat_diff", "%+.0f"),
        ("detail (relift - console)", box_blur(detail(cy) - detail(ry), 6), 8.0, "detail_ratio", "x%.2f"),
    ]
    out = Image.new("RGB", (2 * w, h + h * 2 // 3 + 4))
    a = Image.fromarray(ref.astype(np.uint8))
    b = Image.fromarray(cap.astype(np.uint8))
    label(a, "console " + ref_label)
    label(b, "relift " + cap_label)
    out.paste(a, (0, 0))
    out.paste(b, (w, 0))
    tw, th = 2 * w // 3, h * 2 // 3
    for k, (title, arr, scale, key, fmt) in enumerate(maps):
        t = Image.fromarray(heat(arr, scale)).resize((tw, th), Image.BILINEAR)
        dr = ImageDraw.Draw(t)
        for i in range(1, 3):
            dr.line((i * tw // 3, 0, i * tw // 3, th), fill=(90, 90, 90))
            dr.line((0, i * th // 3, tw, i * th // 3), fill=(90, 90, 90))
        f = font(20)
        for i, s in enumerate(stats):
            x, y = (i % 3) * tw // 3 + 8, (i // 3) * th // 3 + th // 6 - 10
            dr.text((x, y), fmt % s[key], fill=(255, 255, 255), font=f)
        label(t, title, size=18)
        out.paste(t, (k * tw, h + 4))
    return out, stats


# ---------------------------------------------------------------- command line


def capture_files(path, lo, hi, every):
    if os.path.isdir(path):
        names = sorted(n for n in os.listdir(path) if n.lower().endswith(".png"))
        files = []
        for n in names:
            m = re.search(r"frame-(\d+)", n)
            files.append((int(m.group(1)) if m else None, os.path.join(path, n)))
    else:
        m = re.search(r"frame-(\d+)", os.path.basename(path))
        files = [(int(m.group(1)) if m else None, path)]
    if lo is not None:
        files = [f for f in files if f[0] is not None and f[0] >= lo]
    if hi is not None:
        files = [f for f in files if f[0] is not None and f[0] <= hi]
    return files[:: max(every, 1)]


def parse_masks(vals):
    if not vals:
        return DEFAULT_MASK
    return [tuple(float(x) for x in v.split(",")) for v in vals if v != "none"]


def cmd_match(a):
    masks = parse_masks(a.mask)
    files = capture_files(a.capture, a.start, a.to, a.every)
    if not files:
        sys.exit("no captures")
    out_dir = a.out or (a.capture if os.path.isdir(a.capture) else os.path.dirname(os.path.abspath(a.capture)))
    out_dir = os.path.join(out_dir, "refcmp")
    os.makedirs(out_dir, exist_ok=True)
    imgs = [Image.open(p).convert("RGB") for _, p in files]
    window = (a.at - a.window, a.at + a.window) if a.at is not None else None
    results = match_images(a.ref, imgs, masks, a.monotonic, window)
    report = []
    for (present, p), im, r in zip(files, imgs, results):
        ok = r["score"] >= a.min_score
        name = os.path.splitext(os.path.basename(p))[0]
        entry = dict(r, capture=p, present=present, comparable=ok)
        lines = ["%s: console %.2f s, score %.3f (second best %s s, score %s, margin %s)%s" % (
            name, r["time"], r["score"], r["second_time"], r["second_score"], r["margin"],
            "" if ok else "  NO COMPARISON: score below %.2f, the capture shows another moment" % a.min_score)]
        if ok:
            if r["margin"] is not None and r["margin"] < 0.03:
                lines.append("  AMBIGUOUS: the second best moment scores within 0.03; use --monotonic or --at")
            ref_img = ffmpeg_frame(a.ref, r["time"], (1280, 720))
            img, stats = panel(ref_img, im, "%.2f s (score %.2f)" % (r["time"], r["score"]),
                               "present %s" % present, masks)
            img.save(os.path.join(out_dir, name + ".cmp.png"))
            entry["regions"] = stats
            for s in stats:
                w = describe(s)
                lines.append("  %-13s luma %3.0f vs %3.0f, sat %2.0f vs %2.0f, detail %4.1f vs %4.1f: %s" % (
                    s["region"], s["luma_cap"], s["luma_ref"], s["sat_cap"], s["sat_ref"],
                    s["detail_cap"], s["detail_ref"], "; ".join(w) if w else "close"))
            entry["panel"] = os.path.join(out_dir, name + ".cmp.png")
        print("\n".join(lines))
        entry["summary"] = lines
        report.append(entry)
    with open(os.path.join(out_dir, "match.json"), "w") as f:
        json.dump(report, f, indent=1)
    print("wrote", out_dir)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("index")
    p.add_argument("video")
    p.add_argument("--fps", type=float, default=10.0)
    p = sub.add_parser("match")
    p.add_argument("capture")
    p.add_argument("--ref", required=True)
    p.add_argument("--out")
    p.add_argument("--monotonic", action="store_true")
    p.add_argument("--min-score", type=float, default=0.30)
    p.add_argument("--from", dest="start", type=int)
    p.add_argument("--to", type=int)
    p.add_argument("--every", type=int, default=1)
    p.add_argument("--at", type=float, help="restrict the search to AT +- WINDOW seconds")
    p.add_argument("--window", type=float, default=10.0)
    p.add_argument("--mask", action="append", help="x0,y0,x1,y1 fractions to ignore (watermark); 'none' for no mask")
    a = ap.parse_args()
    if a.cmd == "index":
        print(build_index(a.video, a.fps))
    else:
        cmd_match(a)


if __name__ == "__main__":
    main()
