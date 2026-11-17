#!/usr/bin/env python3
"""compare_arb.py RUN.csv: checks a run of the main RAM arbitration ROM (make ARB=1, arb.c) against the consoles
(arb.csv). Every row's outcome flags (kaddr) must be the consoles'. A gapless stream of the CPU main RAM favors
keeps the other out until it ends: load_itcm p7's load must end with the ARM7's give-up (its 2048-tick wait
apart, within 256 ticks), and the hold rows at p7 must get the consoles' loads in, with the longest gap within 1%
of theirs (the same kernels run, so the same stream length). At p9 a load costs what it does on the consoles:
load_itcm within 2 ticks, the hold rows' counts within 1% and gaps within 4 ticks. The run may use shorter
give-ups (ARB_SPINS, ARB_WAIT9) than the consoles; no check depends on them. Exits 1 on a mismatch."""
import csv
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def rows(path):
    with open(path) as f:
        return list(csv.DictReader(line for line in f if not line.startswith("#")))


run = {r["idx"]: r for r in rows(sys.argv[1]) if r.get("cpu") == "9"}
bad = 0
for h in rows(os.path.join(HERE, "arb.csv")):
    r = run.get(h["idx"])
    label = f"{h['idx']} {h['name']} {h['region']}"
    if r is None:
        print(f"missing: {label}")
        bad += 1
        continue
    flags, t16, t48 = int(r["kaddr"], 16), int(r["t16"]), int(r["t48"])
    hw16 = [int(h["c3ds_t16"]), int(h["dsi_t16"])]
    hw48 = [int(h["c3ds_t48"]), int(h["dsi_t48"])]
    why = []
    if (flags & 7) != int(h["kaddr"]):
        why.append(f"flags {flags & 7}, consoles {h['kaddr']}")
    if h["name"] == "load_itcm" and h["region"] == "p7" and abs(t16 + 2048 - t48) > 256:
        why.append(f"load {t16} does not end with the ARM7's {t48}")
    if h["name"] == "load_itcm" and h["region"] == "p9" and not min(hw16) - 2 <= t16 <= max(hw16) + 2:
        why.append(f"load {t16}, consoles {hw16}")
    if h["name"].startswith("hold") and h["region"] == "p7":
        if t16 != hw16[0] or abs(t48 - hw48[0]) > hw48[0] / 100:
            why.append(f"loads {t16} gap {t48}, consoles {hw16[0]} and {hw48[0]}")
    if h["name"].startswith("hold") and h["region"] == "p9":
        if abs(t16 - hw16[0]) > hw16[0] / 100 or abs(t48 - hw48[0]) > 4:
            why.append(f"loads {t16} gap {t48}, consoles {hw16} and {hw48}")
    if why:
        bad += 1
        print(f"mismatch: {label}: {'; '.join(why)}")
print(f"{len(run)} rows run, {bad} failures")
sys.exit(1 if bad else 0)
