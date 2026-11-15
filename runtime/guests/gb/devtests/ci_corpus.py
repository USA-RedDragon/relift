#!/usr/bin/env python3
"""Runs the corpus cases of ci_cases.toml through relift: for each ROM,
`relift import` and `relift build` in a project of its own, then the built
program with `--case` (the case's pass protocol, graded by the device
library's harness) and `--check` (lockstep against the library's own CPU).

  python3 -I ci_corpus.py --relift BIN --roms DIR --work DIR [--jobs N]

DIR for --roms holds the c-sp game-boy-test-roms v7.0 release; with
--fetch it is downloaded there first and its sha256 checked. The ROMs are
never part of the repository. Exit status 0 when every case passes.
Standard library only.
"""

import argparse
import concurrent.futures as cf
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tomllib
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
RELEASE = "https://github.com/c-sp/game-boy-test-roms/releases/download/v7.0/game-boy-test-roms-v7.0.zip"
RELEASE_SHA256 = "b9a9d7a1075aa35a3d07c07c34974048672d8520dca9e07a50178f5860c3832c"
VERDICT = re.compile(r"relift-gb: (pass|fail|unsupported): (.*)")
STATS = re.compile(r"relift-gb stats: (.*)")


def fetch(roms):
    os.makedirs(roms, exist_ok=True)
    if os.path.isfile(os.path.join(roms, "README.md")):
        return
    zpath = os.path.join(roms, "release.zip")
    urllib.request.urlretrieve(RELEASE, zpath)
    h = hashlib.sha256()
    with open(zpath, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    if h.hexdigest() != RELEASE_SHA256:
        sys.exit(f"{RELEASE}: sha256 {h.hexdigest()}, expected {RELEASE_SHA256}")
    with zipfile.ZipFile(zpath) as z:
        z.extractall(roms)
    os.remove(zpath)


def spec(case, roms):
    lines = [f"protocol={case['protocol']}"]
    if "timeout_frames" in case:
        lines.append(f"frames={case['timeout_frames']}")
    if "timeout_cycles" in case:
        lines.append(f"cycles={case['timeout_cycles']}")
    for k in ("marker", "addr", "expect"):
        if k in case:
            lines.append(f"{k}={case[k]:#x}")
    for k in ("capture", "pass_text", "region", "input", "expect_hex", "cart", "mode"):
        if k in case:
            lines.append(f"{k}={case[k]}")
    if "audible" in case:
        lines.append(f"audible={'true' if case['audible'] else 'false'}")
    texts = case.get("fail_text", [])
    for t in [texts] if isinstance(texts, str) else texts:
        lines.append(f"fail_text={t}")
    for r in case.get("refs", []):
        lines.append(f"ref={os.path.join(roms, r)}")
    for s, e in case.get("skip", []):
        lines.append(f"skip={s}-{e}")
    return "\n".join(lines) + "\n"


def run(cmd, env, cwd=None, timeout=600):
    try:
        p = subprocess.run(cmd, env=env, cwd=cwd, capture_output=True, text=True, errors="replace", timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return -9, "", "timed out"


def one_rom(args, index, rom, cases):
    env = dict(os.environ, RELIFT_CACHE_DIR=os.path.join(args.work, "cache"), RELIFT_JOBS="1")
    env.setdefault("RELIFT_PACKAGE_APPIMAGE", "never")
    name = f"rom{index:03d}"
    proj = os.path.join(args.work, "projects", name)
    os.makedirs(proj, exist_ok=True)
    rc, so, se = run([args.relift, "-q", "import", os.path.join(args.roms, rom)], env, proj)
    if rc == 0:
        rc, so, se = run([args.relift, "-q", "build"], env, proj)
    game = os.path.join(args.work, "cache", "projects", name, "build", "host", "game")
    out = []
    for case in cases:
        if rc != 0:
            out.append((case["id"], "build", (se or so).strip()[-300:], ""))
            continue
        sp = os.path.join(proj, "case.txt")
        with open(sp, "w") as f:
            f.write(spec(case, args.roms))
        hw = case.get("hardware", case["mode"])
        crc, cso, cse = run([game, "--case", sp, "--model", hw, "--check", "--stats"], env)
        m = VERDICT.search(cso)
        verdict = m.group(1) if m else ("diverged" if crc == 70 else "crash")
        detail = m.group(2) if m else (cse or cso).strip()[-300:]
        if crc == 70:
            verdict, detail = "diverged", cse.strip()[-300:]
        s = STATS.search(cse)
        out.append((case["id"], verdict, detail, s.group(1) if s else ""))
    shutil.rmtree(proj, ignore_errors=True)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--relift", required=True)
    ap.add_argument("--roms", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--fetch", action="store_true")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    args = ap.parse_args()
    args.relift = os.path.abspath(args.relift)
    args.roms = os.path.abspath(args.roms)
    args.work = os.path.abspath(args.work)
    if args.fetch:
        fetch(args.roms)
    with open(os.path.join(HERE, "ci_cases.toml"), "rb") as f:
        cases = tomllib.load(f)["case"]
    by_rom = {}
    for c in cases:
        by_rom.setdefault(c["rom"], []).append(c)
    first = sorted(by_rom)[0]
    results = one_rom(args, 0, first, by_rom.pop(first))
    with cf.ThreadPoolExecutor(args.jobs) as ex:
        futs = [ex.submit(one_rom, args, i + 1, r, cs) for i, (r, cs) in enumerate(sorted(by_rom.items()))]
        for fu in futs:
            results.extend(fu.result())
    failed = 0
    for cid, verdict, detail, stats in sorted(results):
        if verdict != "pass":
            failed += 1
        print(f"{verdict:9} {cid}  {detail[:160]}")
        if stats and verdict != "pass":
            print(f"          {stats}")
    print(f"{len(results) - failed} of {len(results)} cases pass")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
