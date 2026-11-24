#!/usr/bin/env python3
"""Reads a frame trace the NVN layer wrote (RELIFT_NVN_TRACE=<dir>:<present>).

  python3 -I nvn_trace.py summary FRAME_DIR
  python3 -I nvn_trace.py show FRAME_DIR COMMAND
  python3 -I nvn_trace.py writes FRAME_DIR [ADDRESS]
  python3 -I nvn_trace.py buffer FRAME_DIR COMMAND STAGE:KIND:SLOT [--when pre|post] [--as f32|u32] [--count N]
  python3 -I nvn_trace.py image FRAME_DIR COMMAND STORAGE [--when pre|post] [--level N] [--layer N] [--ppm FILE]
  python3 -I nvn_trace.py log FRAME_DIR FIRST LAST
  python3 -I nvn_trace.py order FRAME_DIR [--queue Q] [--name TEXT]

FRAME_DIR is the frame-<n> folder holding trace.jsonl. A command is its number in the trace; an address is a GPU
or guest address of a buffer, or a storage address of an image, as hex. `writes` lists the commands after which a
buffer or an image with that address (any, when none is given) holds other bytes than before. `image` prints each
channel's range and mean of the chosen level (default 0) and layer (default 0) and, with --ppm, writes it as a
picture (values clamped to 0..1). An image file holds level 0 of every layer, then each further level, every layer
of it, in the host image's format (uncompressed color images; block formats and volumes keep level 0).
`log` prints the NVN commands the layer replayed before commands FIRST to LAST (names, arguments, the target and
bytes of inline uniform updates) and each change of the range RELIFT_NVN_TRACE_WATCH names.
`order` prints order.jsonl, the order the frame's submits, commands, event waits and signals, queue syncs,
conditional render checks (the counter words compared and whether the command ran) and counter reports written ran in,
one per line with its queue, submit and index in the set; a draw or dispatch shows its command number in trace.jsonl.
--queue keeps one queue, --name the lines whose call holds TEXT.
Standard library only.
"""

import argparse
import json
import os
import struct
import sys

HALF = {}


def half(h):
    v = HALF.get(h)
    if v is None:
        v = struct.unpack("<e", struct.pack("<H", h))[0]
        HALF[h] = v
    return v


def small_float(bits, mant):
    exp = bits >> mant
    man = bits & ((1 << mant) - 1)
    if exp == 0:
        return man * 2.0 ** (-14 - mant)
    if exp == 31:
        return float("inf") if man == 0 else float("nan")
    return (1 + man / (1 << mant)) * 2.0 ** (exp - 15)


def load_all(frame):
    out = []
    with open(os.path.join(frame, "trace.jsonl")) as f:
        for line in f:
            if line.strip():
                out.append(json.loads(line))
    return out


def load(frame):
    """The draws and dispatches of the trace, without its command log and watch lines."""
    return [c for c in load_all(frame) if c["kind"] in ("draw", "dispatch")]


def cmd_log(frame, first, last):
    """Prints the NVN commands replayed before draws or dispatches first..last and the watched range's changes."""
    for c in load_all(frame):
        if c["kind"] == "cmd" and first <= c["before"] <= last:
            extra = " -> %s data %s" % (c["dst"], c.get("data", "")) if "dst" in c else ""
            print("before %d: %s %s%s" % (c["before"], c["name"], " ".join(c["x"][:6]), extra))
        elif c["kind"] == "watch" and first <= c["before"] <= last:
            data = read(frame, c["bytes"])
            print("before %d: watch %s changed (after %s): %s" % (c["before"], c["guest"], c["after"], data[:64].hex()))


def read(frame, name):
    with open(os.path.join(frame, name), "rb") as f:
        return f.read()


def num(text):
    return int(text, 0)


def decode(data, vk_format, width, height):
    """Returns (channel count, list of rows of tuples) for the first layer of a picture."""
    n = width * height
    if vk_format in (37, 43, 44, 50):
        px = [tuple(data[4 * i + k] / 255.0 for k in range(4)) for i in range(n)]
        return 4, px
    if vk_format == 97:
        v = struct.unpack("<%dH" % (4 * n), data[: 8 * n])
        return 4, [tuple(half(v[4 * i + k]) for k in range(4)) for i in range(n)]
    if vk_format == 83:
        v = struct.unpack("<%dH" % (2 * n), data[: 4 * n])
        return 2, [tuple(half(v[2 * i + k]) for k in range(2)) for i in range(n)]
    if vk_format == 76:
        v = struct.unpack("<%dH" % n, data[: 2 * n])
        return 1, [(half(x),) for x in v]
    if vk_format == 100:
        v = struct.unpack("<%df" % n, data[: 4 * n])
        return 1, [(x,) for x in v]
    if vk_format == 126:
        v = struct.unpack("<%df" % n, data[: 4 * n])
        return 1, [(x,) for x in v]
    if vk_format == 122:
        v = struct.unpack("<%dI" % n, data[: 4 * n])
        return 3, [(small_float(w & 2047, 6), small_float(w >> 11 & 2047, 6), small_float(w >> 22, 5)) for w in v]
    raise SystemExit("image format %d is not decoded by this reader" % vk_format)


def cmd_order(frame, queue, name):
    traced = {c["seq"]: c["i"] for c in load(frame) if c.get("seq")}
    with open(os.path.join(frame, "order.jsonl")) as f:
        for line in f:
            if not line.strip():
                continue
            o = json.loads(line)
            if queue is not None and num(o["queue"]) != queue:
                continue
            if name and name not in o["name"]:
                continue
            where = {"queue": "queue call", "report": "report written"}.get(o["kind"], "submit %d" % o["submit"])
            if o["kind"] in ("command", "render"):
                where += " pos %d" % o["pos"]
            extra = ""
            if "runs" in o:
                extra = " runs" if o["runs"] else " skipped"
            if "value" in o:
                extra = " word %d%s" % (o["value"], "" if o["met"] else " (waits)")
            if o["seq"] in traced:
                extra += " -> trace command %d" % traced[o["seq"]]
            print("%6d queue %s %-16s %s %s%s" % (o["seq"], o["queue"], where, o["name"], " ".join(o["args"]), extra))


def cmd_summary(frame):
    for c in load(frame):
        progs = ",".join("%s:%s" % (k, v["key"][:8]) for k, v in c["programs"].items())
        what = "groups %s" % c["groups"] if c["kind"] == "dispatch" else "count %d inst %d" % (c["count"], c["instances"])
        extra = ""
        if c["kind"] == "draw":
            extra = " targets " + ",".join(
                "%s%dx%d" % ("D" if t["depth"] else "", t["width"], t["height"]) for t in c["targets"])
        print("%4d %-8s %-40s %s textures %d buffers %d%s" % (
            c["i"], c["kind"], progs, what, len(c["textures"]), len(c["buffers"]), extra))


def cmd_show(frame, index):
    for c in load(frame):
        if c["i"] == index:
            print(json.dumps(c, indent=1))
            return
    raise SystemExit("no command %d" % index)


def cmd_writes(frame, address):
    for c in load(frame):
        hits = []
        for b in c["buffers"]:
            if b.get("pre") != b.get("post") and (
                    address is None or address in (num(b["gpu"]), num(b["guest"]))):
                hits.append("buffer %s %s slot %d gpu %s" % (b["stage"], b["kind"], b["slot"], b["gpu"]))
        for t in c["textures"] + c.get("targets", []):
            if t.get("pre") != t.get("post") and (address is None or address == num(t["storage"])):
                hits.append("image storage %s fmt %d %dx%d" % (t["storage"], t["format"], t["width"], t["height"]))
        for h in hits:
            print("command %d (%s %s): %s" % (c["i"], c["kind"], ",".join(c["programs"]), h))


def cmd_buffer(frame, index, which, when, as_type, count):
    stage, kind, slot = which.split(":")
    for c in load(frame):
        if c["i"] != index:
            continue
        for b in c["buffers"]:
            if b["stage"] == stage and b["kind"] == kind and b["slot"] == int(slot):
                name = b.get(when)
                if not name:
                    raise SystemExit("no %s file (empty range)" % when)
                data = read(frame, name)
                n = min(count, len(data) // 4)
                fmt = "<%df" % n if as_type == "f32" else "<%dI" % n
                vals = struct.unpack(fmt, data[: 4 * n])
                nonzero = sum(1 for x in data if x)
                print("%s %s slot %s gpu %s guest %s size %d, %d nonzero bytes, file %s" % (
                    stage, kind, slot, b["gpu"], b["guest"], b["size"], nonzero, name))
                for i in range(0, n, 8):
                    print("%6d: %s" % (4 * i, " ".join("%g" % x if as_type == "f32" else "%08x" % x
                                                        for x in vals[i:i + 8])))
                return
    raise SystemExit("no such buffer")


BYTES = {37: 4, 43: 4, 44: 4, 50: 4, 97: 8, 83: 4, 76: 2, 100: 4, 126: 4, 122: 4}


def level_slice(t, data, level, layer):
    """Returns (width, height, bytes) of one level and layer of an image file."""
    bpp = BYTES.get(t["vk_format"])
    if bpp is None:
        raise SystemExit("image format %d is not decoded by this reader" % t["vk_format"])
    w, h = t["width"], t["height"]
    layers = max(1, len(data) // (w * h * bpp)) if level == 0 else None
    if layers is None:
        total = 0
        layers = 0
        for cand in range(1, 65):
            size = sum(max(1, w >> lv) * max(1, h >> lv) * bpp * cand for lv in range(t["levels"]))
            if size == len(data):
                layers = cand
                break
        if not layers:
            raise SystemExit("this image file holds only level 0")
    off = 0
    for lv in range(level):
        off += max(1, w >> lv) * max(1, h >> lv) * bpp * layers
    lw, lh = max(1, w >> level), max(1, h >> level)
    off += lw * lh * bpp * layer
    return lw, lh, data[off:off + lw * lh * bpp]


def cmd_image(frame, index, storage, when, ppm, level=0, layer=0):
    for c in load(frame):
        if c["i"] != index:
            continue
        for t in c["textures"] + c.get("targets", []):
            if num(t["storage"]) != storage:
                continue
            name = t.get(when)
            if not name:
                raise SystemExit("no %s file for that image" % when)
            w, h, chunk = level_slice(t, read(frame, name), level, layer)
            nch, px = decode(chunk, t["vk_format"], w, h)
            print("storage %s fmt %d (vk %d) level %d layer %d %dx%d file %s" % (t["storage"], t["format"], t["vk_format"], level, layer, w, h, name))
            for k in range(nch):
                col = [p[k] for p in px if p[k] == p[k]]
                if col:
                    print("  channel %d: min %g max %g mean %g" % (k, min(col), max(col), sum(col) / len(col)))
            if ppm:
                with open(ppm, "wb") as f:
                    f.write(b"P6\n%d %d\n255\n" % (w, h))
                    out = bytearray()
                    for p in px:
                        rgb = list(p[:3]) + [0.0] * (3 - min(3, len(p)))
                        if nch == 1:
                            rgb = [p[0]] * 3
                        out += bytes(max(0, min(255, int(v * 255 + 0.5))) if v == v else 0 for v in rgb)
                    f.write(out)
            return
    raise SystemExit("no such image")


def main():
    ap = argparse.ArgumentParser(description="Reads an NVN frame trace.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("summary")
    p.add_argument("frame")
    p = sub.add_parser("show")
    p.add_argument("frame")
    p.add_argument("command", type=int)
    p = sub.add_parser("writes")
    p.add_argument("frame")
    p.add_argument("address", nargs="?", type=num)
    p = sub.add_parser("buffer")
    p.add_argument("frame")
    p.add_argument("command", type=int)
    p.add_argument("which")
    p.add_argument("--when", choices=["pre", "post"], default="post")
    p.add_argument("--as", dest="as_type", choices=["f32", "u32"], default="f32")
    p.add_argument("--count", type=int, default=64)
    p = sub.add_parser("log")
    p.add_argument("frame")
    p.add_argument("first", type=int)
    p.add_argument("last", type=int)
    p = sub.add_parser("image")
    p.add_argument("frame")
    p.add_argument("command", type=int)
    p.add_argument("storage", type=num)
    p.add_argument("--when", choices=["pre", "post"], default="post")
    p.add_argument("--level", type=int, default=0)
    p.add_argument("--layer", type=int, default=0)
    p.add_argument("--ppm")
    p = sub.add_parser("order")
    p.add_argument("frame")
    p.add_argument("--queue", type=num)
    p.add_argument("--name")
    a = ap.parse_args()
    if a.cmd == "summary":
        cmd_summary(a.frame)
    elif a.cmd == "show":
        cmd_show(a.frame, a.command)
    elif a.cmd == "writes":
        cmd_writes(a.frame, a.address)
    elif a.cmd == "log":
        cmd_log(a.frame, a.first, a.last)
    elif a.cmd == "buffer":
        cmd_buffer(a.frame, a.command, a.which, a.when, a.as_type, a.count)
    elif a.cmd == "order":
        cmd_order(a.frame, a.queue, a.name)
    else:
        cmd_image(a.frame, a.command, a.storage, a.when, a.ppm, a.level, a.layer)


if __name__ == "__main__":
    sys.exit(main())
