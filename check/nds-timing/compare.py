#!/usr/bin/env python3
"""compare.py RUN.csv [--clang]: checks a run of the timing test ROM against the consoles (hw.csv).

A case the consoles agree on (all three within 2%) must come out within max(0.26, 1%) cycles of
the first console, unless open.csv lists it: an open case must come out at the value open.csv
records, so a change to it shows. Cases the consoles disagree on are reported, not checked. Exits 1
on a mismatch or a missing case. With --clang the run is of the clang build (build.py): it does not
run the cases whose code is copied to the instruction TCM, and the open cases of the devkitPro build
only (open.csv's rom column) are checked as the others."""
import csv
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def rows(path):
    with open(path) as f:
        return list(csv.DictReader(line for line in f if not line.startswith("#")))


def cycles(v):
    return int(v) / 100 if v not in ("", None) else None


hw = rows(os.path.join(HERE, "hw.csv"))
clang = "--clang" in sys.argv[2:]
run = {(r["cpu"], r["idx"]): r for r in rows(sys.argv[1]) if r.get("cpu_x100") not in (None, "")}
open_cases = {(r["cpu"], r["idx"]): r for r in rows(os.path.join(HERE, "open.csv"))
              if not (clang and r["rom"] == "sdk")}
bad = ok = disagree = known = 0
for h in hw:
    key = (h["cpu"], h["idx"])
    want = [cycles(h[c]) for c in ("c3ds", "dsi", "dsi2")]
    got = cycles(run[key]["cpu_x100"]) if key in run else None
    label = f"{h['cpu']} {h['idx']:>3} {h['name']:<14} {h['region']:<5} {h['place']:<4} {h['mode']:<9} {h['attr']}"
    if clang and h["place"] == "itcm":
        continue
    if any(abs(w - want[0]) > 0.02 * max(want[0], 1) for w in want[1:]):
        disagree += 1
        continue
    if key in open_cases:
        rec = cycles(open_cases[key]["relift"])
        if (got is None) != (rec is None) or (got is not None and abs(got - rec) > 0.01):
            bad += 1
            print(f"open case changed: {label}: hw {want[0]:g}, recorded {rec}, now {got}")
        else:
            known += 1
        continue
    if got is None or abs(got - want[0]) > max(0.26, 0.01 * want[0]):
        bad += 1
        print(f"mismatch: {label}: hw {want[0]:g}, relift {got}")
    else:
        ok += 1
print(f"{ok} cases match the consoles, {known} open cases as recorded, {disagree} where the consoles "
      f"disagree, {bad} failures")
sys.exit(1 if bad else 0)
