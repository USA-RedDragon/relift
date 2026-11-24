#!/usr/bin/env python3
"""Differential replay of a frame the differential capture saved (RELIFT_NVN_DIFF): names the first dispatch whose
output differs, and why.

  python3 -I nvn_diff.py CAPTURE_DIR --programs DIR --device DEVICE_JSON --out DIR
                         [--krun PATH] [--xlate PATH] [--wgrun PATH] [--gpu NAME] [--repeat N]
                         [--target HEX] [--all] [--chain] [--spvi-groups N] [--steps N]

CAPTURE_DIR is a diff-<present> folder (runtime/guests/switch/nvn/nvn_diff.c). Each captured dispatch is run again
from the exact bytes it read in the game (the frame's first-touch state with the outputs of the game's earlier
dispatches laid over it; with --chain, the outputs of the replay's own earlier dispatches instead):

- on the GPU with krun (gpus/sm86/kat/compute/krun.c), --repeat times (default 3), each in a fresh device;
- on the CPU with the SPIR-V interpreter (gpus/sm86 example wgrun, gpus/spvi), for programs it can run (no image
  instructions), on the workgroups whose words differ (at most --spvi-groups, default 4).

Every written buffer range and storage image is compared word for word: the game's output, each GPU replay, and the
interpreter's. Which dispatches run: by default the writer chain of the target, the first image a dispatch filled
with new zero bands (or --target, a guest address of an image or buffer), walked back through the capture's ranges
to every earlier dispatch that wrote what it read; --all runs every dispatch.

Verdicts per dispatch:
  match            the game and every replay agree
  sync             the game's output differs from replays that agree with each other (and with the interpreter
                   where it ran): the inputs were right but the game's run was not; an ordering or race between
                   commands, or a race inside the program the replays do not hit
  race             the GPU replays of the same inputs differ from each other: a race inside the program
  translation      the GPU replay differs from the interpreter on the same inputs: a translation or driver problem
  incomplete       outputs were dropped (arena full) or the replay could not run

report.json holds everything; report.txt the summary: the first divergent dispatch, its program, the differing
words, their image region (pixel box and 128-pixel tile columns) or buffer offsets, and for the interpreter the
workgroups and the SPIR-V block each stopped in. Standard library only.
"""

import argparse
import json
import os
import struct
import subprocess
import sys

# Bytes per texel of the VkFormat numbers (as nvn_replay.py).
BPP = {37: 4, 43: 4, 44: 4, 50: 4, 64: 4, 76: 2, 83: 2, 91: 8, 97: 8, 98: 4, 99: 4, 100: 4, 103: 8, 106: 8, 107: 12,
       109: 16, 110: 16, 122: 4, 9: 1, 16: 2, 70: 2, 81: 2, 124: 2, 126: 4, 129: 4, 130: 5, 58: 4, 59: 4,
       13: 1, 14: 1, 15: 1, 20: 2, 21: 2, 22: 2, 74: 2, 75: 2, 77: 2, 82: 2, 84: 2, 88: 2, 95: 8, 96: 8,
       101: 4, 102: 8, 104: 8, 105: 8, 108: 12, 111: 16, 112: 16, 113: 16}
TYPES = {0: "1d", 1: "2d", 2: "3d", 3: "cube", 4: "1darr", 5: "2darr", 6: "cubearr"}
CHUNK = 1 << 16


def h(s):
    return int(s, 16) if isinstance(s, str) else s


class Capture:
    """A saved frame: its dispatches, spans and arena."""

    def __init__(self, d):
        self.dir = d
        with open(os.path.join(d, "capture.json")) as f:
            self.j = json.load(f)
        self.disp = self.j["dispatches"]
        self.spans = self.j["spans"]
        self.arena = open(os.path.join(d, "arena.bin"), "rb")
        for s in self.spans:
            s["guest"] = h(s["guest"])
            s["gpu"] = h(s["gpu"])
        for e in self.disp:
            for r in e["ranges"]:
                r["guest"] = h(r["guest"])
                r["gpu"] = h(r["gpu"])
            for t in e["textures"]:
                t["storage"] = h(t["storage"])

    def bytes_of(self, s):
        self.arena.seek(s["at"])
        return self.arena.read(s["size"])


class Mem:
    """Guest memory as the replay sees it: segments laid over one another, the latest on top."""

    def __init__(self):
        self.segs = []  # (lo, bytearray), later ones win

    def put(self, lo, b):
        self.segs.append((lo, bytearray(b)))

    def under(self, lo, b):
        """Adds b at lo only where nothing is yet (a first-touch span)."""
        self.segs.insert(0, (lo, bytearray(b)))

    def read(self, lo, n):
        out = bytearray(n)
        have = bytearray(n)  # unused marker; zeros where nothing captured
        for slo, sb in self.segs:
            a, z = max(lo, slo), min(lo + n, slo + len(sb))
            if a < z:
                out[a - lo:z - lo] = sb[a - slo:z - slo]
        del have
        return out

    def covered(self, lo, n):
        """Bytes of [lo, lo + n) some segment holds."""
        iv = sorted((max(lo, s), min(lo + n, s + len(b))) for s, b in self.segs if s < lo + n and s + len(b) > lo)
        tot, end = 0, lo
        for a, z in iv:
            a = max(a, end)
            if z > a:
                tot += z - a
                end = z
        return tot


def word_diff(a, b):
    """Offsets of the differing 4-byte words of two equal-length byte strings (chunked, fast when equal)."""
    out = []
    n = min(len(a), len(b))
    for c in range(0, n, CHUNK):
        x, y = a[c:c + CHUNK], b[c:c + CHUNK]
        if x == y:
            continue
        for i in range(0, len(x) - len(x) % 4, 4):
            if x[i:i + 4] != y[i:i + 4]:
                out.append(c + i)
    return out


def image_region(offs, t, bpp):
    """The pixel box and the 128-wide tile columns of differing words of image t (level 0 rows)."""
    w = t["width"] >> (t["level"] if t["image"] else 0) or 1
    hh = t["height"] >> (t["level"] if t["image"] else 0) or 1
    row = w * bpp
    pts = [((o % row) // bpp, (o // row) % hh) for o in offs if o < row * hh * max(1, t["layers"])]
    if not pts:
        return None
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    cols = sorted({x // 128 for x in xs})
    rows = sorted({y // 8 * 8 for y in ys})
    return {"x": [min(xs), max(xs) + 1], "y": [min(ys), max(ys) + 1], "pixels": len(set(pts)),
            "tile_cols_128": cols[:64], "rows_mod8": len(rows)}


def zero_bands(b, t, bpp, prev):
    """Columns of exact-zero texels at least 1/16 of the height tall where prev had non-zero texels: [(x0, x1, y0, y1)]."""
    w, hh = t["width"], t["height"]
    row = w * bpp
    if len(b) < row * hh or len(prev) < row * hh:
        return []
    z = bytes(bpp)
    runs = []
    minrows = max(8, hh // 16)
    for x in range(w):
        y = 0
        while y < hh:
            o = y * row + x * bpp
            if b[o:o + bpp] != z:
                y += 1
                continue
            y0, had = y, 0
            while y < hh and b[y * row + x * bpp:y * row + x * bpp + bpp] == z:
                had += prev[y * row + x * bpp:y * row + x * bpp + bpp] != z
                y += 1
            if y - y0 >= minrows and had * 10 >= (y - y0) * 9:
                runs.append((x, y0, y))
    bands = []
    for x, y0, y1 in runs:
        for bd in bands:
            if bd[1] == x and min(bd[3], y1) - max(bd[2], y0) > 0:
                bd[1] = x + 1
                bd[2] = min(bd[2], y0)
                bd[3] = max(bd[3], y1)
                break
        else:
            bands.append([x, x + 1, y0, y1])
    return [tuple(bd) for bd in bands if bd[1] - bd[0] <= w // 8]


def dims_bytes(e):
    """Bytes of the texture dimension table both replays get: a whole number of 64 KiB (wgrun's least uniform block)
    holding every bound id, so a translation that clamps ids to the table's length clamps none of them."""
    top = max([t["id"] + 1 for t in e["textures"]] + [1])
    return (4 * top + 0xFFFF) & ~0xFFFF


class Replayer:
    def __init__(self, cap, a, out):
        self.cap, self.a, self.out = cap, a, out
        self.spv = {}

    def binary(self, key):
        """The program file of key, which may be a prefix of its name in --programs."""
        p = os.path.join(self.a.programs, key + ".bin")
        if os.path.exists(p):
            return p
        for name in sorted(os.listdir(self.a.programs)):
            if name.startswith(key) and name.endswith(".bin"):
                return os.path.join(self.a.programs, name)
        return p

    def program(self, key):
        if key in self.spv:
            return self.spv[key]
        spv = os.path.join(self.out, "prog-%s.spv" % key[:16])
        r = subprocess.run([self.a.xlate, self.binary(key), spv, self.a.device],
                           capture_output=True, text=True)
        ok = r.returncode == 0 and r.stdout.startswith("ok")
        self.spv[key] = spv if ok else None
        if not ok:
            print("xlate %s: %s %s" % (key[:8], r.stdout[-400:], r.stderr[-400:]), file=sys.stderr)
        return self.spv[key]

    @staticmethod
    def regions(e):
        """The dispatch's mapped ranges merged into regions: [guest_lo, guest_hi, gpu_lo]."""
        rs = sorted((r["guest"], r["guest"] + r["size"], r["gpu"]) for r in e["ranges"] if not r["unmapped"])
        out = []
        for lo, hi, gpu in rs:
            if out and lo <= out[-1][1] and gpu - lo == out[-1][2] - out[-1][0]:
                out[-1][1] = max(out[-1][1], hi)
            else:
                out.append([lo, hi, gpu])
        return out

    def run_gpu(self, e, mem, imgs, tag, spv=None, extra=()):
        """Runs dispatch e on the GPU from mem (buffers) and imgs (storage -> bytes); returns (buffers, images)."""
        d = os.path.join(self.out, "d%03d" % e["i"], tag)
        os.makedirs(d, exist_ok=True)
        spv = spv or self.program(e["key"])
        if not spv:
            return None
        specs = dict((k, v) for k, v in e["specs"])
        spec = ["plainspecs", "spv " + spv, "groups %d %d %d" % tuple(e["groups"]),
                "local %d %d %d" % (specs.get(0, 1), specs.get(1, 1), specs.get(2, 1))]
        for k, v in sorted(specs.items()):
            if k > 2:
                spec.append("spec %d %d" % (k, v))
        regs = self.regions(e)
        c0 = bytearray(open(os.path.join(self.cap.dir, e["c0"]), "rb").read())
        puts = []
        for k, (lo, hi, gpu) in enumerate(regs):
            init = os.path.join(d, "in%d.bin" % k)
            with open(init, "wb") as f:
                f.write(mem.read(lo, hi - lo))
            spec.append("outf %s %d %s" % (os.path.join(d, "out%d.bin" % k), hi - lo, init))
            for off in range(0x10, len(c0) - 7, 8):
                v = struct.unpack_from("<Q", c0, off)[0]
                if gpu <= v < gpu + (hi - lo):
                    puts.append("put 0 %d addr %d %d" % (off, k, v - gpu))
        spec.append("bank 0 " + os.path.join(self.cap.dir, e["c0"]))
        if "c1" in e:
            spec.append("bank 1 " + os.path.join(self.cap.dir, e["c1"]))
        for r in e["ranges"]:
            if r["kind"] == "uniform" and r["slot"] < 15 and not r["unmapped"]:
                p = os.path.join(d, "ubo%d.bin" % r["slot"])
                with open(p, "wb") as f:
                    f.write(mem.read(r["guest"], min(r["size"], 65536)))
                spec.append("bank %d %s" % (3 + r["slot"], p))
        spec.extend(puts)
        top = 4
        outs = []
        for ti, t in enumerate(e["textures"]):
            bpp = BPP.get(t["vk_format"])
            if not bpp or not t["bytes"] or t["storage"] not in imgs:
                continue
            p = os.path.join(d, "tex%d.bin" % ti)
            with open(p, "wb") as f:
                f.write(imgs[t["storage"]])
            lw = t["width"] >> (t["level"] if t["image"] else 0) or 1
            lh = t["height"] >> (t["level"] if t["image"] else 0) or 1
            spec.append("imagefile %s %d %d %d %d %d %d %s" % (
                TYPES.get(t["dim"] if not t["image"] else 1, "2d"), lw, lh,
                t["depth"] if t["target"] == 2 else t["layers"], t["copy_levels"], t["vk_format"], bpp, p))
            if t["image"]:
                spec.append("view %d %d" % (32 + 8 * t["kind"] + t["dim"], t["id"]))
                o = os.path.join(d, "img%d.bin" % ti)
                spec.append("imageout " + o)
                outs.append((ti, o))
            else:
                spec.append("view %d %d" % (1 + 10 * t["kind"] + t["dim"], t["id"]))
            top = max(top, t["id"] + 1)
        if top > 4:
            spec.insert(1, "texcount %d" % top)
            spec.append("sampler 0 1 1 0")
        dims = bytearray(dims_bytes(e))
        for t in e["textures"]:
            if not t["image"]:
                struct.pack_into("<I", dims, 4 * t["id"], t["dim"] | t["kind"] << 8)
        dp = os.path.join(d, "texdims.bin")
        with open(dp, "wb") as f:
            f.write(dims)
        spec.append("texdims " + dp)
        spec.extend(extra)
        sp = os.path.join(d, "spec")
        with open(sp, "w") as f:
            f.write("\n".join(spec) + "\n")
        r = subprocess.run([self.a.krun, sp, self.a.gpu], capture_output=True, text=True)
        self.addrs = {}
        for line in r.stdout.splitlines():
            w = line.split()
            if len(w) == 3 and w[0] == "buf":
                self.addrs[int(w[1])] = int(w[2], 16)
        if r.returncode:
            print("krun d%d: %s %s" % (e["i"], r.stdout[-400:], r.stderr[-400:]), file=sys.stderr)
            return None
        bufs = {}
        for k, (lo, hi, gpu) in enumerate(regs):
            bufs[lo] = open(os.path.join(d, "out%d.bin" % k), "rb").read()
        im = {}
        for ti, o in outs:
            im[e["textures"][ti]["storage"]] = open(o, "rb").read()
        return bufs, im

    def ref_program(self, key):
        """The interpreter's translation: HMMA by the translator's exact emulation instead of cooperative matrices."""
        k = "ref:" + key
        if k in self.spv:
            return self.spv[k]
        spv = os.path.join(self.out, "prog-%s-ref.spv" % key[:16])
        r = subprocess.run([self.a.xlate, self.binary(key), spv, self.a.device,
                            "--no-coop-hmma"], capture_output=True, text=True)
        self.spv[k] = spv if r.returncode == 0 and r.stdout.startswith("ok") else None
        return self.spv[k]

    def run_spvi(self, e, mem, imgs, groups, tag="spvi", spv=None, extra=(), steps=None, reloc=None):
        """Runs workgroups of dispatch e on the interpreter from mem and imgs.

        Returns (buffers {guest_lo: bytes}, images {storage: bytes}, stores [(guest, bytes, invocation, group,
        block)], notes) or (None, None, None, why)."""
        if not self.a.wgrun:
            return None, None, None, "no wgrun"
        spv = spv or self.ref_program(e["key"])
        if not spv:
            return None, None, None, "no translation"
        d = os.path.join(self.out, "d%03d" % e["i"], tag)
        os.makedirs(d, exist_ok=True)
        specs = dict((k, v) for k, v in e["specs"])
        args = [self.a.wgrun, spv, "--size", "%d,%d,%d" % (specs.get(0, 1), specs.get(1, 1), specs.get(2, 1)),
                "--steps", str(steps or self.a.steps), "--log-stores", os.path.join(d, "stores.txt")]
        for k, v in sorted(specs.items()):
            if k > 2:
                args += ["--spec", "%d=%d" % (k, v)]
        regs = self.regions(e)
        c0 = os.path.join(self.cap.dir, e["c0"])
        if reloc:
            # The GPU run's buffer addresses: its c[0] words and regions, so addresses compare equal.
            b0 = bytearray(open(c0, "rb").read())
            for off in range(0x10, len(b0) - 7, 8):
                v = struct.unpack_from("<Q", b0, off)[0]
                for k, (lo, hi, gpu) in enumerate(regs):
                    if gpu <= v < gpu + (hi - lo) and k in reloc:
                        struct.pack_into("<Q", b0, off, reloc[k] + v - gpu)
            c0 = os.path.join(d, "c0.bin")
            with open(c0, "wb") as f:
                f.write(b0)
            regs = [(lo, hi, reloc.get(k, gpu)) for k, (lo, hi, gpu) in enumerate(regs)]
        args += ["--uniform", "1,0=" + c0]
        if "c1" in e:
            args += ["--uniform", "1,1=" + os.path.join(self.cap.dir, e["c1"])]
        gpu_of = {}
        for k, (lo, hi, gpu) in enumerate(regs):
            p = os.path.join(d, "in%d.bin" % k)
            with open(p, "wb") as f:
                f.write(mem.read(lo, hi - lo))
            args += ["--region", "%x=%s" % (gpu, p), "--dump", "%x,%d=%s" % (gpu, hi - lo, os.path.join(d, "out%d.bin" % k))]
            gpu_of[lo] = gpu
        for r in e["ranges"]:
            if r["kind"] == "base" and not r["unmapped"]:
                start = r["gpu"]
                for (lo, hi, gpu), (lo0, hi0, gpu0) in zip(regs, self.regions(e)):
                    if gpu0 <= r["gpu"] < gpu0 + (hi0 - lo0):
                        start = gpu + r["gpu"] - gpu0
                args += ["--base", "%x=%x" % (r["slot"], start)]
            elif r["kind"] == "uniform" and r["slot"] < 15 and not r["unmapped"]:
                p = os.path.join(d, "ubo%d.bin" % r["slot"])
                with open(p, "wb") as f:
                    f.write(mem.read(r["guest"], min(r["size"], 65536)))
                args += ["--uniform", "0,%d=%s" % (r["slot"], p)]
        dims = bytearray(dims_bytes(e))
        outs = []
        for ti, t in enumerate(e["textures"]):
            if t["storage"] not in imgs or not t["bytes"]:
                continue
            p = os.path.join(d, "tex%d.bin" % ti)
            with open(p, "wb") as f:
                f.write(imgs[t["storage"]])
            # A translation picks the array by the access's kind: every kind's binding of the dimension views the
            # same texels (wgrun shares one file's texels).
            if t["image"]:
                bindings = [32 + 8 * k + t["dim"] for k in range(9)]
            else:
                bindings = [1 + 10 * k + t["dim"] for k in range(3)]
            binding = bindings[min(t["kind"], len(bindings) - 1)]
            lw = t["width"] >> (t["level"] if t["image"] else 0) or 1
            lh = t["height"] >> (t["level"] if t["image"] else 0) or 1
            for bnd in bindings:
                args += ["--image", "2,%d,%d=%d,%d,%d,%d,%d,%d,%s" % (
                    bnd, t["id"], t["vk_format"], lw, lh, t["depth"] if t["target"] == 2 else 1,
                    1 if t["target"] == 2 else t["layers"], t["copy_levels"], p)]
            if t["image"]:
                o = os.path.join(d, "img%d.bin" % ti)
                args += ["--image-dump", "2,%d,%d=%s" % (binding, t["id"], o)]
                outs.append((t["storage"], o))
            else:
                struct.pack_into("<I", dims, 4 * t["id"], t["dim"] | t["kind"] << 8)
        dp = os.path.join(d, "texdims.bin")
        with open(dp, "wb") as f:
            f.write(dims)
        args += ["--uniform", "2,31=" + dp]
        for g in groups:
            args += ["--group", "%d,%d,%d" % tuple(g)]
        args += list(extra)
        with open(os.path.join(d, "args"), "w") as f:
            f.write("\n".join(args) + "\n")
        r = subprocess.run(args, capture_output=True, text=True)
        notes = (r.stdout + r.stderr)[-3000:]
        if r.returncode:
            return None, None, None, notes
        bufs = {lo: open(os.path.join(d, "out%d.bin" % k), "rb").read() for k, (lo, hi, gpu) in enumerate(regs)}
        im = {st: open(o, "rb").read() for st, o in outs}
        stores = []
        img_at = {}
        for line in open(os.path.join(d, "stores.txt")):
            w = line.split()
            addr = int(w[0], 16)
            guest = None
            for lo, hi, gpu in regs:
                if gpu <= addr < gpu + (hi - lo):
                    guest = lo + (addr - gpu)
            stores.append((guest, addr, int(w[1]), int(w[2]), w[3], w[5]))
        del img_at
        return bufs, im, stores, notes


def state_before(cap, upto, chained=None):
    """Guest memory and images the game's dispatch upto read: first-touch spans, then the game's (or, chained, the
    replay's) outputs of the dispatches before it."""
    mem = Mem()
    imgs = {}
    for s in cap.spans:
        if s["what"] == "pre":
            mem.under(s["guest"], cap.bytes_of(s))
    pre_imgs = [s for s in cap.spans if s["what"] == "pre_image"]
    for s in reversed(pre_imgs):
        imgs[s["guest"]] = cap.bytes_of(s)
    for e in cap.disp[:upto]:
        if chained and e["i"] in chained:
            bufs, im = chained[e["i"]]
            for lo, b in bufs.items():
                mem.put(lo, b)
            imgs.update(im)
            continue
        for s in cap.spans:
            if s["disp"] == e["i"] and s["what"] == "post":
                mem.put(s["guest"], cap.bytes_of(s))
            elif s["disp"] == e["i"] and s["what"] == "post_image":
                imgs[s["guest"]] = cap.bytes_of(s)
    return mem, imgs


def exact_check(cap, e, mem, imgs):
    """For a dispatch captured exactly (RELIFT_NVN_DIFF_EXACT): what changed in its inputs between the end of the
    frame's earlier dispatches and its start ("before": a writer outside the captured dispatches, such as the CPU,
    a copy or another queue), and between its start and its end in what it only reads ("during": a writer that ran
    beside it). Returns (findings, mem and imgs as it really started)."""
    found = []
    exact_mem = Mem()
    exact_imgs = dict(imgs)
    for s in cap.spans:
        if s["disp"] != e["i"]:
            continue
        if s["what"] == "exact":
            b = cap.bytes_of(s)
            offs = word_diff(mem.read(s["guest"], s["size"]), b)
            if offs:
                found.append({"when": "before", "what": "buf", "at": "0x%x" % s["guest"], "words": len(offs),
                              "bytes": [offs[0], offs[-1] + 4]})
            exact_mem.put(s["guest"], b)
        elif s["what"] == "exact_image":
            b = cap.bytes_of(s)
            old = imgs.get(s["guest"])
            if old is not None and len(old) == len(b):
                offs = word_diff(old, b)
                if offs:
                    t = e["textures"][s["tex"]]
                    found.append({"when": "before", "what": "img", "at": "0x%x" % s["guest"], "words": len(offs),
                                  "region": image_region(offs, t, BPP.get(t["vk_format"], 4))})
            exact_imgs[s["guest"]] = b
    for s in cap.spans:
        if s["disp"] != e["i"] or s["what"] not in ("after", "after_image"):
            continue
        b = cap.bytes_of(s)
        if s["what"] == "after":
            offs = word_diff(exact_mem.read(s["guest"], s["size"]), b)
            if offs:
                found.append({"when": "during", "what": "buf", "at": "0x%x" % s["guest"], "words": len(offs),
                              "bytes": [offs[0], offs[-1] + 4]})
        else:
            offs = word_diff(exact_imgs.get(s["guest"], b), b)
            if offs:
                t = e["textures"][s["tex"]]
                found.append({"when": "during", "what": "img", "at": "0x%x" % s["guest"], "words": len(offs),
                              "region": image_region(offs, t, BPP.get(t["vk_format"], 4))})
    mem.segs.extend(exact_mem.segs)
    return found, mem, exact_imgs


def game_after(cap, e, mem, imgs):
    """The game's output of e: {('buf', guest_lo): bytes} for each written region, {('img', storage): bytes}."""
    out = {}
    for s in cap.spans:
        if s["disp"] != e["i"]:
            continue
        if s["what"] == "post":
            out[("buf", s["guest"])] = cap.bytes_of(s)
        elif s["what"] == "post_image":
            out[("img", s["guest"])] = cap.bytes_of(s)
    return out


def replay_view(e, bufs, im, game):
    """The replay's bytes for each key of game."""
    out = {}
    for (kind, lo), b in game.items():
        if kind == "img":
            out[(kind, lo)] = im.get(lo)
            continue
        for rlo, rb in bufs.items():
            if rlo <= lo and lo + len(b) <= rlo + len(rb):
                out[(kind, lo)] = rb[lo - rlo:lo - rlo + len(b)]
    return out


def compare(e, a, b, imgs_meta):
    """Differences of two output views: [{key, words, first, region}]."""
    res = []
    for k, x in a.items():
        y = b.get(k)
        if x is None or y is None:
            continue
        offs = word_diff(x, y)
        close = 0
        zeros = []
        if k[0] == "img" and imgs_meta.get(k[1]):
            fmt = imgs_meta[k[1]]["vk_format"]
            bpp = BPP.get(fmt, 4)
            keep = []
            for o in offs:
                o -= o % bpp
                if keep and keep[-1] == o:
                    continue
                a8, b8 = x[o:o + bpp], y[o:o + bpp]
                if not rgb(a8, fmt) and rgb(b8, fmt):
                    zeros.append(o)
                if channels_close(a8, b8, fmt):
                    close += 1
                else:
                    keep.append(o)
            offs = keep
        if not offs:
            continue
        item = {"what": k[0], "at": "0x%x" % k[1], "words": len(offs), "first_byte": offs[0],
                "first_words": [x[offs[0]:offs[0] + 4].hex(), y[offs[0]:offs[0] + 4].hex()]}
        if close:
            item["within_rounding"] = close
        if zeros:
            item["zero_only_in_first"] = {"texels": len(zeros),
                                          "region": image_region(zeros, imgs_meta[k[1]], BPP.get(fmt, 4))}
        if k[0] == "img":
            t = imgs_meta.get(k[1])
            if t:
                item["region"] = image_region(offs, t, BPP.get(t["vk_format"], 4))
        else:
            item["region"] = {"bytes": [offs[0], offs[-1] + 4]}
        res.append(item)
    return res


def chain_of(cap, target):
    """The dispatches the target depends on: its last writer and, recursively, the last earlier writers of what each
    read."""
    def writes(e, lo, hi):
        for r in e["ranges"]:
            if r["write"] and r["guest"] < hi and lo < r["guest"] + r["size"]:
                return True
        return any(t["image"] and t["storage"] == lo for t in e["textures"])

    want, todo = set(), []
    for e in reversed(cap.disp):
        if writes(e, target, target + 1):
            todo.append(e["i"])
            break
    while todo:
        i = todo.pop()
        if i in want:
            continue
        want.add(i)
        e = cap.disp[i]
        reads = [(r["guest"], r["guest"] + r["size"]) for r in e["ranges"]]
        reads += [(t["storage"], t["storage"] + 1) for t in e["textures"]]
        for lo, hi in reads:
            for p in reversed(cap.disp[:i]):
                if writes(p, lo, hi):
                    todo.append(p["i"])
                    break
    return sorted(want)


def find_target(cap):
    """The first image a dispatch filled with new zero bands: (dispatch, storage, bands) or None."""
    for e in cap.disp:
        for ti, t in enumerate(e["textures"]):
            if not t["image"]:
                continue
            post = [s for s in cap.spans if s["disp"] == e["i"] and s["what"] == "post_image" and s.get("tex") == ti]
            pre = [s for s in cap.spans if s["what"] == "pre_image" and s["guest"] == t["storage"]]
            bpp = BPP.get(t["vk_format"])
            if not post or not pre or not bpp:
                continue
            b = cap.bytes_of(post[0])
            bands = zero_bands(b, t, bpp, cap.bytes_of(pre[0]))
            if bands:
                return e["i"], t["storage"], bands
    return None


def groups_for(e, diffs, n):
    """Workgroups to interpret: spread over the grid (the word-to-group map is the program's), at most n."""
    gx, gy, gz = e["groups"]
    total = gx * gy * gz
    if not total:
        return []
    step = max(1, total // n)
    out = []
    for k in range(0, total, step):
        out.append((k % gx, (k // gx) % gy, k // (gx * gy)))
        if len(out) == n:
            break
    return out


def image_bases(e, imgs):
    """The addresses wgrun gives the images of dispatch e, in the order run_spvi passes them: storage -> base."""
    at, out = 0x7d00_0000_0000_0000, {}
    for t in e["textures"]:
        if t["storage"] not in imgs or not t["bytes"]:
            continue
        out.setdefault(t["storage"], at)
        at += (len(imgs[t["storage"]]) + 0xffff) & ~0xffff
    return out


def written_boxes(stores, e, imgs, key):
    """Per workgroup, the box of what it stored into output key: ('img', storage) -> pixel box (x0, y0, x1, y1);
    ('buf', guest) -> byte range (lo, hi, 0, 0)."""
    bases = image_bases(e, imgs)
    meta = {t["storage"]: t for t in e["textures"]}
    boxes = {}
    for guest, addr, n, inv, grp, block in stores:
        if key[0] == "img":
            base = bases.get(key[1])
            t = meta[key[1]]
            bpp = BPP.get(t["vk_format"], 4)
            if base is None or not base <= addr < base + len(imgs[key[1]]):
                continue
            o = addr - base
            x, y = (o // bpp) % t["width"], (o // bpp) // t["width"]
            b = boxes.get(grp, (x, y, x + 1, y + 1))
            boxes[grp] = (min(b[0], x), min(b[1], y), max(b[2], x + 1), max(b[3], y + 1))
        elif guest is not None and key[1] <= guest:
            b = boxes.get(grp, (guest, guest + n, 0, 0))
            boxes[grp] = (min(b[0], guest), max(b[1], guest + n), 0, 0)
    return boxes


def group_of(point, probe, e):
    """The workgroup that writes point ((x, y) of an image or (offset, 0) of a buffer), from the boxes the probe
    groups (0,0,0), (1,0,0) and (0,1,0) wrote, assuming a regular tiling; None when it cannot be told."""
    g0, gx, gy = probe.get("0,0,0"), probe.get("1,0,0"), probe.get("0,1,0")
    if not g0:
        return None
    sx = (gx[0] - g0[0], gx[1] - g0[1]) if gx else (0, 0)
    sy = (gy[0] - g0[0], gy[1] - g0[1]) if gy else (0, 0)
    px, py = point[0] - g0[0], point[1] - g0[1]
    det = sx[0] * sy[1] - sx[1] * sy[0]
    if det:
        u = (px * sy[1] - py * sy[0]) / det
        v = (sx[0] * py - sx[1] * px) / det
        gxn, gyn = int(u), int(v)
    elif sx[0]:
        lin = px // sx[0]
        gxn, gyn = lin % e["groups"][0], lin // e["groups"][0]
    else:
        return None
    if not (0 <= gxn < e["groups"][0] and 0 <= gyn < max(1, e["groups"][1])):
        return None
    return (gxn, gyn, 0)


def rgb(t, fmt):
    """A texel's color bits without alpha (all its bits for formats without a known alpha)."""
    v = int.from_bytes(t, "little")
    return v & {64: 0x3FFFFFFF, 37: 0xFFFFFF, 44: 0xFFFFFF, 97: 0xFFFFFFFFFFFF}.get(fmt, (1 << (8 * len(t))) - 1)


def channels_close(x, y, fmt):
    """Whether two texels of an unorm format differ by at most 1 in each channel (the rounding Vulkan allows), and
    neither is all zeros where the other is not."""
    a, b = int.from_bytes(x, "little"), int.from_bytes(y, "little")
    if (a == 0) != (b == 0):
        return False
    fields = {37: (8, 8, 8, 8), 44: (8, 8, 8, 8), 64: (10, 10, 10, 2), 9: (8,), 16: (8, 8)}.get(fmt)
    if not fields:
        return False
    for bits in fields:
        m = (1 << bits) - 1
        if abs((a & m) - (b & m)) > 1:
            return False
        a, b = a >> bits, b >> bits
    return True


def value_log_diff(rp, e, mem, imgs, invocation, group):
    """Runs one invocation's value log (spv_log.py) on the GPU and on the interpreter, from the same inputs and the
    same translation (debug info, HMMA without cooperative matrices), and returns the first record that differs:
    the SPIR-V id and opcode, the guest PC and instruction, the record's index and both values; or None."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import spv_log
    d = os.path.join(rp.out, "d%03d" % e["i"], "log")
    os.makedirs(d, exist_ok=True)
    dbg = os.path.join(d, "prog-dbg.spv")
    r = subprocess.run([rp.a.xlate, rp.binary(e["key"]), dbg, rp.a.device, "--no-coop-hmma", "--debug-info"],
                       capture_output=True, text=True)
    if r.returncode or not r.stdout.startswith("ok"):
        return {"error": "xlate --debug-info failed"}
    with open(dbg, "rb") as f:
        b = f.read()
    words, info = spv_log.instrument(list(struct.unpack("<%dI" % (len(b) // 4), b)), rp.a.log_records)
    logspv = os.path.join(d, "prog-log.spv")
    with open(logspv, "wb") as f:
        f.write(struct.pack("<%dI" % len(words), *words))
    vals = [invocation] + list(group)
    nwords = 2 * (rp.a.log_records + 1)
    gpu_log = os.path.join(d, "gpu.bin")
    extra = ["spec %d %d" % (spv_log.SPEC_INDEX + k, v) for k, v in enumerate(vals)]
    extra.append("trace %s %d" % (gpu_log, nwords))
    if rp.run_gpu(e, mem, imgs, "log/gpu", spv=logspv, extra=extra) is None:
        return {"error": "GPU run of the value log failed"}
    spvi_log = os.path.join(d, "spvi.bin")
    extra = ["--buffer", "3,7=zero:%d" % (4 * nwords), "--buffer-dump", "3,7=" + spvi_log]
    for k, v in enumerate(vals):
        extra += ["--spec", "%d=%d" % (spv_log.SPEC_INDEX + k, v)]
    got = rp.run_spvi(e, mem, imgs, [group], "log/spvi", spv=logspv, extra=extra, steps=rp.a.steps * 20,
                      reloc=dict(rp.addrs))
    if got[0] is None:
        return {"error": "interpreter run of the value log failed", "notes": got[3][-600:]}

    def load(p):
        with open(p, "rb") as f:
            g = f.read()
        n = len(g) // 4
        return struct.unpack("<%dI" % n, g[:4 * n])

    a, b = load(gpu_log), load(spvi_log)
    n = min(len(a), len(b)) // 2 - 1
    for k in range(n):
        if a[2 * k] == 0 and b[2 * k] == 0:
            return None
        if a[2 * k] != b[2 * k] or a[2 * k + 1] != b[2 * k + 1]:
            rid = a[2 * k] & 0xFFFFFF
            out = {"record": k, "spirv_id": rid, "component": a[2 * k] >> 24, "gpu": "0x%08x" % a[2 * k + 1],
                   "spvi": "0x%08x" % b[2 * k + 1], "invocation": invocation, "group": list(group)}
            out.update(info["ids"].get(rid, {}))
            if a[2 * k] != b[2 * k]:
                out["spvi_id"] = b[2 * k] & 0xFFFFFF
                out["note"] = "the two runs took different paths here"
            return out
    return None


def spvi_check(rp, e, mem, imgs, game, gpu, rec, n):
    """Runs workgroups of e on the interpreter: probe groups to learn the tiling, then the groups that hold the
    first differing words (or the probe groups alone when nothing differs); fills rec's gpu_vs_spvi, spvi_groups,
    spvi_notes and, for each difference, the invocation and SPIR-V block that stored the word."""
    gx, gy, gz = e["groups"]
    probe_groups = [(0, 0, 0)] + ([(1, 0, 0)] if gx > 1 else []) + ([(0, 1, 0)] if gy > 1 else [])
    bufs, im, stores, notes = rp.run_spvi(e, mem, imgs, probe_groups, "spvi-probe")
    rec["spvi_notes"] = notes.strip().splitlines()[-8:] if notes else []
    if bufs is None:
        return False
    keys = [k for k in game]
    diffs = rec["game_vs_gpu"] or rec["gpu_vs_gpu"]
    targets = []
    for d in diffs[:1]:
        key = (d["what"], int(d["at"], 16))
        if d["what"] == "img" and d.get("region"):
            pts = [(d["region"]["x"][0], d["region"]["y"][0]), (d["region"]["x"][1] - 1, d["region"]["y"][1] - 1)]
        else:
            pts = [(key[1] + d["first_byte"], 0)]
        boxes = written_boxes(stores, e, imgs, key)
        probe = {g: b for g, b in boxes.items()}
        for pt in pts:
            g = group_of(pt, probe, e)
            if g and g not in targets:
                targets.append(g)
    groups = probe_groups
    if targets:
        groups = targets[:n]
        bufs, im, stores, notes = rp.run_spvi(e, mem, imgs, groups, "spvi")
        rec["spvi_notes"] = notes.strip().splitlines()[-8:] if notes else []
        if bufs is None:
            return False
    rec["spvi_groups"] = groups
    sv = replay_view(e, bufs, im, game)
    rec["gpu_vs_spvi"] = []
    bases = image_bases(e, imgs)
    meta = {t["storage"]: t for t in e["textures"]}
    for key in keys:
        sb, gb = sv.get(key), gpu.get(key)
        if sb is None or gb is None:
            continue
        if key[0] == "img":
            base = bases.get(key[1])
            mine = sorted({addr - base for g, addr, nb, *_ in stores if base is not None and base <= addr < base + len(sb)})
            fmt = meta[key[1]]["vk_format"]
            bpp = BPP.get(fmt, 4)
            offs = [o for o in mine if sb[o:o + bpp] != gb[o:o + bpp] and not channels_close(sb[o:o + bpp], gb[o:o + bpp], fmt)]
        else:
            mine = sorted({g - key[1] for g, addr, nb, *_ in stores if g is not None and key[1] <= g < key[1] + len(sb)})
            offs = [o for o in mine if sb[o:o + 4] != gb[o:o + 4]]
        if not offs:
            continue
        item = {"what": key[0], "at": "0x%x" % key[1], "words": len(offs), "first_byte": offs[0],
                "first_words": [gb[offs[0]:offs[0] + 4].hex(), sb[offs[0]:offs[0] + 4].hex()]}
        if key[0] == "img":
            item["region"] = image_region(offs, meta[key[1]], BPP.get(meta[key[1]]["vk_format"], 4))
        who = [s for s in stores if (s[0] if key[0] == "buf" else s[1] - bases.get(key[1], 0) + key[1]) is not None]
        for st in stores:
            o = (st[0] - key[1]) if key[0] == "buf" and st[0] is not None else (st[1] - bases[key[1]] if key[0] == "img" else None)
            if o is not None and o <= offs[0] < o + st[2]:
                item["stored_by"] = {"invocation": st[3], "group": st[4], "spirv_block": st[5]}
        del who
        rec["gpu_vs_spvi"].append(item)
    if rec["gpu_vs_spvi"] and rec["gpu_vs_spvi"][0].get("stored_by"):
        sb = rec["gpu_vs_spvi"][0]["stored_by"]
        grp = tuple(int(x) for x in sb["group"].split(","))
        rec["first_divergent_value"] = value_log_diff(rp, e, mem, imgs, sb["invocation"], grp)
    return True


def main():
    ap = argparse.ArgumentParser(description="Differential replay of a captured frame.")
    ap.add_argument("capture")
    ap.add_argument("--programs", required=True)
    ap.add_argument("--device", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--krun", default="krun")
    ap.add_argument("--xlate", default="xlate")
    ap.add_argument("--wgrun")
    ap.add_argument("--gpu", default="NVIDIA")
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--target")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--chain", action="store_true")
    ap.add_argument("--spvi-groups", type=int, default=4)
    ap.add_argument("--no-spvi", action="store_true")
    ap.add_argument("--spvi", type=lambda v: [int(x) for x in v.split(",")],
                    help="run the interpreter only on these dispatches (default: every replayed one)")
    ap.add_argument("--log-records", type=int, default=1000000)
    ap.add_argument("--steps", type=int, default=20000000)
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    cap = Capture(a.capture)
    rp = Replayer(cap, a, out)
    report = {"capture": os.path.abspath(a.capture), "present": cap.j["present"], "dispatches": []}
    trig = os.path.join(a.capture, "trigger.json")
    if os.path.exists(trig):
        report["trigger"] = json.load(open(trig))
    tgt = None
    if a.target:
        tgt = int(a.target, 16)
    else:
        found = find_target(cap)
        if found:
            report["zero_bands"] = {"dispatch": found[0], "key": cap.disp[found[0]]["key"],
                                    "image": "0x%x" % found[1], "bands": found[2][:32]}
            tgt = found[1]
    sel = list(range(len(cap.disp))) if a.all or tgt is None else chain_of(cap, tgt)
    report["selected"] = sel
    chained = {} if a.chain else None
    first = None
    for i in sel:
        e = cap.disp[i]
        mem, imgs = state_before(cap, i, chained)
        changed = []
        if e.get("exact"):
            changed, mem, imgs = exact_check(cap, e, mem, imgs)
        meta = {t["storage"]: t for t in e["textures"]}
        game = game_after(cap, e, mem, imgs)
        rec = {"i": i, "key": e["key"], "groups": e["groups"], "index": e["index"], "seq": e["seq"]}
        if changed:
            rec["inputs_changed"] = changed
        runs = []
        for k in range(a.repeat):
            r = rp.run_gpu(e, mem, imgs, "gpu%d" % k)
            if r is None:
                break
            runs.append(replay_view(e, r[0], r[1], game))
            if chained is not None and k == 0:
                chained[i] = r
        if not runs or e["post_dropped"] or not game:
            rec["verdict"] = "incomplete"
            rec["why"] = "no GPU replay" if not runs else "outputs dropped" if e["post_dropped"] else "no outputs"
            report["dispatches"].append(rec)
            continue
        rec["game_vs_gpu"] = compare(e, game, runs[0], meta)
        rec["gpu_vs_gpu"] = []
        for k in range(1, len(runs)):
            rec["gpu_vs_gpu"] += compare(e, runs[0], runs[k], meta)
        spvi = False
        if not a.no_spvi and (a.spvi is None or i in a.spvi):
            spvi = spvi_check(rp, e, mem, imgs, game, runs[0], rec, a.spvi_groups)
        if changed:
            v = "sync"
        elif rec["gpu_vs_gpu"]:
            v = "race"
        elif spvi and rec.get("gpu_vs_spvi"):
            v = "translation"
        elif rec["game_vs_gpu"]:
            v = "sync"
        else:
            v = "match"
        rec["verdict"] = v
        report["dispatches"].append(rec)
        if v != "match" and first is None:
            first = rec
        print("d%-3d %-16s %-11s game/gpu %s gpu/gpu %s" % (
            i, e["key"][:16], v, sum(x["words"] for x in rec["game_vs_gpu"]),
            sum(x["words"] for x in rec["gpu_vs_gpu"])))
    report["first_divergent"] = first["i"] if first else None
    with open(os.path.join(out, "report.json"), "w") as f:
        json.dump(report, f, indent=1)
    lines = ["present %d: %d dispatches captured, %d replayed (%s)" % (
        cap.j["present"], len(cap.disp), len(sel), "all" if a.all or tgt is None else "writer chain of 0x%x" % tgt)]
    if "zero_bands" in report:
        z = report["zero_bands"]
        lines.append("first image with new zero bands: 0x%s written by dispatch %d (%s), bands %s" % (
            z["image"][2:], z["dispatch"], z["key"][:16], z["bands"][:6]))
    if first:
        lines.append("first divergent dispatch: %d, program %s, verdict %s" % (first["i"], first["key"], first["verdict"]))
        for c in first.get("inputs_changed", [])[:4]:
            lines.append("  input changed %s the dispatch outside the captured dispatches: %s %s, %d words %s" % (
                c["when"], c["what"], c["at"], c["words"], json.dumps(c.get("region") or c.get("bytes"))))
        fv = first.get("first_divergent_value")
        if fv and "spirv_id" in fv:
            lines.append("  first value that differs between GPU and interpreter: invocation %d of group %s, record %d, "
                         "SPIR-V %%%d (%s, component %d), guest PC 0x%x %s: GPU %s, interpreter %s" % (
                             fv["invocation"], fv["group"], fv["record"], fv["spirv_id"], fv.get("op", "?"),
                             fv["component"], fv.get("pc", -1), fv.get("sass", ""), fv["gpu"], fv["spvi"]))
        elif fv:
            lines.append("  value log: %s" % json.dumps(fv))
        for name in ("game_vs_gpu", "gpu_vs_gpu", "gpu_vs_spvi"):
            for x in first.get(name, [])[:4]:
                lines.append("  %s %s %s: %d words, first at byte %d %s" % (
                    name, x["what"], x["at"], x["words"], x["first_byte"], json.dumps(x.get("region"))))
    else:
        lines.append("no dispatch differs: the game's outputs match every replay")
    for r in report["dispatches"]:
        lines.append("  d%d %s %s" % (r["i"], r["key"][:16], r["verdict"]))
    with open(os.path.join(out, "report.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0 if not first else 1


if __name__ == "__main__":
    sys.exit(main())
