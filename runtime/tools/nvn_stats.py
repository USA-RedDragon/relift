#!/usr/bin/env python3
"""Reads the NVN use histograms that runs write with RELIFT_NVN_STATS=<file>.

    nvn_stats.py <game>=<file> [<game>=<file> ...] [--all]

Prints one table of the gaps (procs relift records with no function or does not implement, and commands the host
device ignored, ran in part, stopped at or has no handler for), ranked by how many of the games hit each one and then
by its count. With --all every proc and command is listed. A file without a name before '=' is named by its folder.
"""

import os
import sys


def read(path):
    """The procs and commands of one histogram: {name: row}."""
    procs, cmds = {}, {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            cols = line.rstrip("\n").split("\t")
            if cols[0] == "proc":
                procs[cols[1]] = {
                    "kind": cols[2],
                    "lookups": int(cols[3]),
                    "calls": None if cols[4] == "-" else int(cols[4]),
                    "gap": cols[5] == "gap",
                }
            elif cols[0] == "cmd":
                n = [int(x) for x in cols[2:8]]
                cmds[cols[1]] = {
                    "recorded": n[0],
                    "ok": n[1],
                    "ignored": n[2],
                    "partial": n[3],
                    "stopped": n[4],
                    "unhandled": n[5],
                    "gap": cols[8] == "gap",
                }
    return procs, cmds


def status(row):
    """The worst outcome of a command row."""
    for k in ("unhandled", "stopped", "partial", "ignored"):
        if row[k]:
            return k
    return "ok" if row["ok"] else "recorded"


def main(argv):
    show_all = "--all" in argv
    runs = []
    for a in argv:
        if a.startswith("--"):
            continue
        name, _, path = a.rpartition("=")
        runs.append((name or os.path.basename(os.path.dirname(os.path.abspath(path))), path))
    if not runs:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    table = {}
    for game, path in runs:
        procs, cmds = read(path)
        for name, row in procs.items():
            if not (show_all or row["gap"]):
                continue
            e = table.setdefault(("proc", name), {"what": row["kind"], "games": {}})
            e["games"][game] = row["calls"] if row["calls"] is not None else row["lookups"]
        for name, row in cmds.items():
            if not (show_all or row["gap"] or status(row) == "recorded"):
                continue
            e = table.setdefault(("cmd", name), {"what": status(row), "games": {}})
            if status(row) in ("unhandled", "stopped") or e["what"] in ("ok", "recorded", "ignored"):
                e["what"] = status(row) if e["what"] not in ("unhandled", "stopped") else e["what"]
            e["games"][game] = row["recorded"]
    rows = sorted(table.items(), key=lambda kv: (-len(kv[1]["games"]), -sum(kv[1]["games"].values()), kv[0][1]))
    games = [g for g, _ in runs]
    print("\t".join(["type", "name", "relift"] + games))
    for (kind, name), e in rows:
        print("\t".join([kind, name, e["what"]] + [str(e["games"].get(g, "")) for g in games]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
