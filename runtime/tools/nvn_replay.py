#!/usr/bin/env python3
"""Replays one dispatch of a frame trace (RELIFT_NVN_TRACE) on a Vulkan device, alone, in seconds.

  python3 -I nvn_replay.py FRAME_DIR COMMAND --programs DIR --device DEVICE_JSON --out DIR
                           [--trace COND] [--krun PATH] [--xlate PATH] [--gpu NAME]

FRAME_DIR is the frame-<n> folder of the trace and COMMAND the dispatch's number in it (`nvn_trace.py summary`).
The program comes from DIR/<key>.bin (a build's shaders/programs) and is translated again with the sm86 `xlate`
example for DEVICE_JSON (the run's shaders/device.json), in trace mode with COND (`nan,inf`, `any@0x340/R10`; see
`relift shaders --trace`) when given. `krun` (gpus/sm86/kat/compute/krun.c, built with `cc -O1 -o krun krun.c
-lvulkan`) runs it with what the trace dumped: c[0] and c[1] as the runtime built them (their buffer addresses
pointed at the replay's own copies of the buffers), each user bank, every storage buffer's bytes from before the
dispatch, the textures' texels, the workgroup size and the specialization constants. The outputs go to --out, each
storage buffer is compared with the trace's bytes after the dispatch, and in trace mode each instruction's first hit
is printed (PC, register, value, its sources, invocation).

Limits: the trace keeps no sampler state, so sampled textures use linear filtering with clamped normalized
coordinates; texture arrays and the dimension table (set 2 binding 31) are not rebuilt. The trace needs the "c0" and
"specs" fields (runtimes from the gpu-debug lane on). Standard library only.
"""

import argparse
import json
import os
import struct
import subprocess
import sys

# Bytes per texel of the VkFormat numbers a trace's images hold.
BPP = {37: 4, 43: 4, 44: 4, 50: 4, 64: 4, 76: 2, 83: 2, 91: 8, 97: 8, 98: 4, 99: 4, 100: 4, 103: 8, 106: 8, 107: 12,
       109: 16, 110: 16, 122: 4, 9: 1, 16: 2, 70: 2, 81: 2, 124: 2, 126: 4, 129: 4, 130: 5}
TYPES = {0: "1d", 1: "2d", 2: "3d", 3: "cube", 4: "1darr", 5: "2darr", 6: "cubearr"}


def load(frame):
    with open(os.path.join(frame, "trace.jsonl")) as f:
        return [json.loads(l) for l in f if l.strip()]


def main():
    ap = argparse.ArgumentParser(description="Replays one dispatch of an NVN frame trace.")
    ap.add_argument("frame")
    ap.add_argument("command", type=int)
    ap.add_argument("--programs", required=True)
    ap.add_argument("--device", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--trace")
    ap.add_argument("--krun", default="krun")
    ap.add_argument("--xlate", default="xlate")
    ap.add_argument("--gpu", default="NVIDIA")
    a = ap.parse_args()
    fr = os.path.abspath(a.frame)
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    cmd = [c for c in load(fr) if c.get("i") == a.command and c["kind"] in ("draw", "dispatch")]
    if not cmd or cmd[0]["kind"] != "dispatch":
        sys.exit("command %d is no dispatch of the trace" % a.command)
    c = cmd[0]
    if "c0" not in c:
        sys.exit("the trace has no c0 for this dispatch (a runtime from before the replay tool wrote it)")
    key = c["programs"]["compute"]["key"]
    spv = os.path.join(out, "program.spv")
    xa = [a.xlate, os.path.join(a.programs, key + ".bin"), spv, a.device]
    if a.trace:
        xa += ["--trace", a.trace]
    r = subprocess.run(xa, capture_output=True, text=True)
    if r.returncode or not r.stdout.startswith("ok"):
        sys.exit("xlate: %s %s" % (r.stdout[-2000:], r.stderr[-2000:]))
    words = 0
    if "trace_words: " in r.stdout:
        words = int(r.stdout.split("trace_words: ")[1].split(",")[0])
    c0 = bytearray(open(os.path.join(fr, c["c0"]), "rb").read())
    spec = ["plainspecs", "spv " + spv, "groups %d %d %d" % tuple(c["groups"])]
    specs = dict((k, v) for k, v in c.get("specs", []))
    spec.append("local %d %d %d" % (specs.get(0, 1), specs.get(1, 1), specs.get(2, 1)))
    for k, v in sorted(specs.items()):
        if k > 2:
            spec.append("spec %d %d" % (k, v))
    # Storage buffers: one replay buffer per distinct range, pointed at from c[0].
    bufs = []
    puts = []
    for b in c["buffers"]:
        if b["kind"] != "storage" or "pre" not in b:
            continue
        gpu, size = int(b["gpu"], 16), b["size"]
        if any(x[0] == gpu and x[1] == size for x in bufs):
            continue
        k = len(bufs)
        bufs.append((gpu, size, b))
        spec.append("outf %s %d %s" % (os.path.join(out, "buf%d.bin" % k), size, os.path.join(fr, b["pre"])))
        for off in range(0x10, len(c0) - 7, 8):
            if struct.unpack_from("<Q", c0, off)[0] == gpu:
                puts.append("put 0 %d addr %d" % (off, k))
    spec.append("bank 0 " + os.path.join(fr, c["c0"]))
    if "c1" in c:
        spec.append("bank 1 " + os.path.join(fr, c["c1"]))
    for b in c["buffers"]:
        if b["kind"] == "uniform" and "pre" in b and b["slot"] < 15:
            spec.append("bank %d %s" % (3 + b["slot"], os.path.join(fr, b["pre"])))
    spec.extend(puts)
    top = 4
    for t in c.get("textures", []):
        if t.get("image") or "pre" not in t:
            continue
        bpp = BPP.get(t["vk_format"])
        if not bpp:
            sys.exit("texture %d: no texel size for VkFormat %d" % (t["id"], t["vk_format"]))
        ty = TYPES.get(t["dim"], "2d")
        spec.append("imagefile %s %d %d %d %d %d %d %s" % (ty, t["width"], t["height"], t["depth"], t["levels"],
                                                         t["vk_format"], bpp, os.path.join(fr, t["pre"])))
        spec.append("view %d %d" % (1 + 10 * t["kind"] + t["dim"], t["id"]))
        top = max(top, t["id"] + 1)
    if top > 4:
        spec.insert(1, "texcount %d" % top)
        spec.append("sampler 0 1 1 0")
    if words:
        spec.append("trace %s %d" % (os.path.join(out, "trace.bin"), words))
    with open(os.path.join(out, "spec"), "w") as f:
        f.write("\n".join(spec) + "\n")
    r = subprocess.run([a.krun, os.path.join(out, "spec"), a.gpu], capture_output=True, text=True)
    if r.returncode:
        sys.exit("krun: %s %s" % (r.stdout[-2000:], r.stderr[-2000:]))
    same = True
    for k, (gpu, size, b) in enumerate(bufs):
        got = open(os.path.join(out, "buf%d.bin" % k), "rb").read()
        want = open(os.path.join(fr, b["post"]), "rb").read() if "post" in b else None
        if want is None:
            continue
        diff = [i for i in range(0, min(len(got), len(want)), 4) if got[i:i + 4] != want[i:i + 4]]
        same &= not diff
        print("storage %#x (%d bytes): %s" % (gpu, size, "same as the trace" if not diff else
                                                "%d words differ, first at byte %d: replay %s trace %s" % (
                                                    len(diff), diff[0], got[diff[0]:diff[0] + 4].hex(),
                                                    want[diff[0]:diff[0] + 4].hex())))
    if words:
        report(open(os.path.join(out, "trace.bin"), "rb").read(), key)
    return 0 if same else 1


def name(i):
    if i & 0x80000000:
        return "c%d[0x%x]" % ((i >> 16) & 0x7fff, i & 0xffff)
    return "UR%d" % (i - 0x100) if i >= 0x100 else "R%d" % i


def report(t, key):
    """Prints each instruction's first hit of a trace buffer (layout: the translator's `trace` module)."""
    w = struct.unpack("<%dI" % (len(t) // 4), t)
    slots = (len(w) - 16) // 16 - 1
    hits = [w[16 + 16 * k:32 + 16 * k] for k in range(slots) if w[16 + 16 * k]]
    if not hits:
        print("trace %s: no hits" % key[:8])
        return
    first = min(hits, key=lambda s: s[1])
    for s in hits:
        f = struct.unpack("<f", struct.pack("<I", s[4]))[0]
        src = " ".join("%s=%08x (%g)" % (name(s[8 + 2 * j]), s[9 + 2 * j],
                                         struct.unpack("<f", struct.pack("<I", s[9 + 2 * j]))[0])
                       for j in range(4) if s[8 + 2 * j] != 0xffffffff)
        print("pc 0x%04x %s = %08x (%g), %d hits, seq %d, invocation (%d, %d, %d): %s%s" % (
            s[2], name(s[3]), s[4], f, s[0], s[1], s[5], s[6], s[7], src, " [first in time]" if s is first else ""))


if __name__ == "__main__":
    sys.exit(main())
