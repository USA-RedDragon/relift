#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compares relift's SN76489 (runtime/guests/sms/psg.c) with ares on random write scripts.

Usage: psg-compare.py --ares FILE --relift FILE [--scripts N] [--events N] [--seed N] [--work DIR]

FILE for --ares is psg-step (check/oracle/ares/build-step.sh), for --relift the
sms_psg_script binary (make -C runtime BUILD=... build/.../sms_psg_script). Each
script runs on a Master System and on a Game Gear; the per-sample outputs (the
four attenuations and the two mixed values, as hexadecimal floats) must be
identical. Prints the number of samples compared and the mismatches; exits 1
on any mismatch.
"""
import argparse
import os
import random
import subprocess
import sys


def make_script(rng, events, gg):
    mode = rng.randrange(4)
    t = 0
    lines = []
    for _ in range(events):
        t += rng.choice([0, 1, 3, 7, 16, 17, 31, 50, 120, 400, 2000]) if mode != 3 else rng.randrange(0, 40)
        r = rng.random()
        if r < 0.05 and gg:
            lines.append(f"{t} s {rng.randrange(256)}")
            continue
        if mode == 0:
            v = rng.randrange(256)
        elif r < 0.30:
            ch = rng.randrange(4)
            v = 0x80 | ch << 5 | rng.randrange(16)
        elif r < 0.50:
            v = 0x90 | rng.randrange(4) << 5 | rng.randrange(16)
        elif r < 0.70:
            v = rng.randrange(0x40)
        elif r < 0.85:
            v = 0xE0 | rng.randrange(8)
        else:
            v = 0x80 | rng.randrange(3) << 5 | rng.randrange(4)
        lines.append(f"{t} w {v}")
    lines.append(f"{t + rng.randrange(1, 5000)} end")
    return "\n".join(lines) + "\n"


def run(binary, script, gg, out):
    with open(out, "w") as f:
        subprocess.run([binary] + (["--gg"] if gg else []), input=script, text=True, stdout=f, check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ares", required=True)
    ap.add_argument("--relift", required=True)
    ap.add_argument("--scripts", type=int, default=100)
    ap.add_argument("--events", type=int, default=1500)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--work", default=os.environ.get("TMPDIR", "."))
    a = ap.parse_args()
    rng = random.Random(a.seed)
    samples = 0
    mismatches = 0
    first = None
    for i in range(a.scripts):
        for gg in (False, True):
            script = make_script(rng, a.events, gg)
            pa = os.path.join(a.work, "psg-ares.out")
            pr = os.path.join(a.work, "psg-relift.out")
            run(a.ares, script, gg, pa)
            run(a.relift, script, gg, pr)
            with open(pa) as fa, open(pr) as fr:
                n = 0
                for la, lr in zip(fa, fr):
                    n += 1
                    if la != lr:
                        mismatches += 1
                        if first is None:
                            first = (i, gg, n, la.strip(), lr.strip())
                extra = len(fa.read()) + len(fr.read())
                if extra:
                    mismatches += 1
            samples += n
            if mismatches and first:
                with open(os.path.join(a.work, "psg-fail.script"), "w") as f:
                    f.write(script)
    print(f"samples compared: {samples}")
    print(f"mismatches: {mismatches}")
    if mismatches:
        print(f"first: script {first[0]} gg={first[1]} sample {first[2]}: ares {first[3]} relift {first[4]}")
        sys.exit(1)


main()
