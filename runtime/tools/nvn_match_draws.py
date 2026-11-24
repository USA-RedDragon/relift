#!/usr/bin/env python3
"""Matches the draws of two frames of an RELIFT_NVN_TRACE trace and says which uniforms changed between them.

  python3 -I nvn_match_draws.py TRACE_DIR [--slot N]

TRACE_DIR holds frame-<n> folders (RELIFT_NVN_TRACE=<dir>:<present>:2); the first two are compared. Draws match by
vertex program, fragment program and their order among that pair's draws (a game's uniform ring moves its buffers
every frame, so addresses do not match). Prints the matched count, then per vertex program and uniform slot how many
matched draws changed that uniform. With --slot N it also reads the slot's first 12 floats as a 3x4 object matrix and
prints, per vertex program, how many draws' matrix changed and the largest element change: the draws that move an
object, which motion vectors then must describe.

Needs only the standard library. A trace line cut short at the end of a file ends that frame.
"""

import argparse
import glob
import json
import os
import struct
import sys
from collections import Counter


def draws(folder):
    out = []
    with open(os.path.join(folder, "trace.jsonl")) as f:
        for line in f:
            try:
                x = json.loads(line)
            except ValueError:
                break
            if x["kind"] == "draw":
                x["_dir"] = folder
                out.append(x)
    return out


def keyed(ds):
    seen, m = Counter(), {}
    for x in ds:
        pair = (x["programs"]["vertex"]["key"][:8], x["programs"].get("fragment", {}).get("key", "")[:8])
        m[pair + (seen[pair],)] = x
        seen[pair] += 1
    return m


def uniform(x, slot):
    for b in x["buffers"]:
        if b["stage"] == "vertex" and b["kind"] == "uniform" and b["slot"] == slot:
            with open(os.path.join(x["_dir"], b["post"]), "rb") as f:
                return f.read()
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--slot", type=int)
    a = ap.parse_args()
    frames = sorted(glob.glob(os.path.join(a.dir, "frame-*")))
    if len(frames) < 2:
        print("need two frame-<n> folders in", a.dir)
        return 1
    ma, mb = keyed(draws(frames[0])), keyed(draws(frames[1]))
    both = [k for k in mb if k in ma and ma[k]["count"] == mb[k]["count"] and ma[k]["instances"] == mb[k]["instances"]]
    print("%s: %d draws, %s: %d draws, matched with the same counts: %d" % (
        os.path.basename(frames[0]), len(ma), os.path.basename(frames[1]), len(mb), len(both)))
    changed, total = Counter(), Counter()
    for k in both:
        for slot in sorted({b["slot"] for b in ma[k]["buffers"] if b["stage"] == "vertex" and b["kind"] == "uniform"}):
            ua, ub = uniform(ma[k], slot), uniform(mb[k], slot)
            if ua is None or ub is None:
                continue
            total[(k[0], slot)] += 1
            changed[(k[0], slot)] += ua != ub
    for (v, slot), n in sorted(total.items()):
        print("  vertex %s uniform slot %d: changed in %d of %d" % (v, slot, changed[(v, slot)], n))
    if a.slot is None:
        return 0
    moved = {}
    for k in both:
        ua, ub = uniform(ma[k], a.slot), uniform(mb[k], a.slot)
        if ua is None or ub is None or len(ua) < 48 or len(ub) < 48:
            continue
        fa, fb = struct.unpack("<12f", ua[:48]), struct.unpack("<12f", ub[:48])
        d = max(abs(p - q) for p, q in zip(fa, fb))
        m = moved.setdefault(k[0], [0, 0, 0.0])
        m[0] += 1
        if d > 0.0:
            m[1] += 1
            m[2] = max(m[2], d)
    for v, (n, c, d) in sorted(moved.items()):
        print("  vertex %s slot %d as an object matrix: %d draws, %d moved, largest element change %.4g" % (
            v, a.slot, n, c, d))
    return 0


if __name__ == "__main__":
    sys.exit(main())
