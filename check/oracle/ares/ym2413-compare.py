#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compares relift's YM2413 (runtime/guests/sms/ym2413.c) with ares on random write scripts.

Usage: ym2413-compare.py --ares FILE --relift FILE [--scripts N] [--events N] [--seed N] [--work DIR]

FILE for --ares is ym2413-step (check/oracle/ares/build-step.sh), for --relift the
sms_ym2413_script binary (make -C runtime BUILD=... build/.../sms_ym2413_script).
The per-sample mixes must be identical. Prints the number of samples compared and
the mismatches; exits 1 on any mismatch.
"""
import argparse
import os
import random
import subprocess
import sys


def make_script(rng, events):
    t = 0
    lines = []
    rhythm = rng.random() < 0.4
    for _ in range(events):
        t += rng.choice([0, 1, 30, 72, 73, 200, 1000, 5000, 20000])
        r = rng.random()
        if r < 0.10:
            reg = rng.randrange(8)
        elif r < 0.20:
            reg = 0x0E
        elif r < 0.45:
            reg = 0x10 + rng.randrange(9)
        elif r < 0.75:
            reg = 0x20 + rng.randrange(9)
        elif r < 0.97:
            reg = 0x30 + rng.randrange(9)
        else:
            reg = rng.randrange(256)
        v = rng.randrange(256)
        if reg == 0x0E and not rhythm:
            v &= ~0x20
        lines.append(f"{t} a {reg}")
        lines.append(f"{t + rng.randrange(0, 90)} w {v}")
        t += 90
    lines.append(f"{t + rng.randrange(1, 50000)} end")
    return "\n".join(lines) + "\n"


def run(binary, script, out):
    with open(out, "w") as f:
        subprocess.run([binary], input=script, text=True, stdout=f, check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ares", required=True)
    ap.add_argument("--relift", required=True)
    ap.add_argument("--scripts", type=int, default=60)
    ap.add_argument("--events", type=int, default=400)
    ap.add_argument("--seed", type=int, default=11)
    ap.add_argument("--work", default=os.environ.get("TMPDIR", "."))
    a = ap.parse_args()
    rng = random.Random(a.seed)
    samples = 0
    mismatches = 0
    first = None
    for i in range(a.scripts):
        script = make_script(rng, a.events)
        pa = os.path.join(a.work, "ym-ares.out")
        pr = os.path.join(a.work, "ym-relift.out")
        run(a.ares, script, pa)
        run(a.relift, script, pr)
        with open(pa) as fa, open(pr) as fr:
            n = 0
            for la, lr in zip(fa, fr):
                n += 1
                if la != lr:
                    mismatches += 1
                    if first is None:
                        first = (i, n, la.strip(), lr.strip())
            if len(fa.read()) + len(fr.read()):
                mismatches += 1
        samples += n
        if first and first[0] == i:
            with open(os.path.join(a.work, "ym-fail.script"), "w") as f:
                f.write(script)
    print(f"samples compared: {samples}")
    print(f"mismatches: {mismatches}")
    if mismatches:
        print(f"first: script {first[0]} sample {first[1]}: ares {first[2]} relift {first[3]}")
        sys.exit(1)


main()
