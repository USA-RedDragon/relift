#!/usr/bin/env python3
"""Writes ../src/translate/mufu/tables.rs from a CUDA device.

    mufu_tables.py <work dir> <tables.rs> [function ...]

Needs nvcc, an SM86 device (an RTX 3090 for the committed table), numpy
and scipy. For each function it runs MUFU on every input of the range its
table covers (one binade, or two for RSQ and SQRT, or a quarter turn for
SIN), keeps the outputs in the work dir, and solves each table row: the
integers A, B, C for which the interpolator (see ../src/translate/mufu.rs)
gives the device's output for every input of the row's segment. A row is
the integer point of a linear feasibility problem; the solver bounds B and
C by linear programs over the segment, then scans them and takes the
smallest A. See README.md.
"""
import json
import math
import multiprocessing as mp
import os
import struct
import subprocess
import sys

import numpy as np
from scipy.optimize import linprog

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen  # noqa: E402
import sass as s  # noqa: E402

# name: (MUFU function, inputs, input kind, segment bits, squarer column,
#        target)
#   input kind: ('add', base): base + i; ('turns', k): i * 2^-k
#   target: ('up', K, scale): lo = floor(y * scale) << K, hi = lo + 2^K - 1;
#           ('cut', K, r): the real interval that cuts to y, times 2^(K + r)
FUNCS = {
    'RCP': ('RCP', 1 << 23, ('add', 0x3f800000), 16, 17, ('up', 16, 2**24)),
    'RSQ': ('RSQ', 1 << 24, ('add', 0x3f800000), 17, 19, ('up', 18, 2**24)),
    'SQRT': ('SQRT', 1 << 24, ('add', 0x3f800000), 17, 19, ('up', 18, 2**23)),
    'EX2': ('EX2', 1 << 23, ('turns', 23), 17, 19, ('up', 16, 2**23)),
    'LG2': ('LG2', 1 << 23, ('add', 0x3f800000), 17, 19, ('cut', 36, 2)),
    'SIN': ('SIN', 1 << 23, ('turns', 25), 17, 19, ('cut', 37, 0)),
}

HEADER = """//! Coefficient rows of the MUFU interpolator, one per table index: `A` as
//! two words (low, high), then `B` and `C` as two's-complement words. Each
//! row was solved as the integer point of a linear feasibility problem over
//! every input of its segment, from the outputs of an RTX 3090, and the
//! constructions in [`super`] check against the device on all 2^32 inputs.
//! Written by gpus/sm86/kat/mufu_tables.py; do not edit.
//!
//! Units: RCP `P >> 16`, RSQ and SQRT `P >> 18`, EX2 `P >> 16` give the
//! 24-bit mantissa; LG2 has 38 fraction bits, SIN 37.

"""


def sweep(fn, kind):
    """A kernel: x from the global thread index, out[i] = MUFU.fn(x)."""
    i2f_u32 = s.W(0x106, 1, [(16, 8, 4), (32, 8, 0), (84, 2, 2), (75, 3, 2)])
    if kind[0] == 'add':
        make = [s.W(0x010, 4, [(16, 8, 4), (24, 8, 0), (32, 32, kind[1]), (64, 8, s.RZ)])]
    else:
        make = [i2f_u32, s.W(0x020, 4, [(16, 8, 4), (24, 8, 4), (32, 32, (127 - kind[1]) << 23), (84, 3, 4)])]
    body = [
        s.s2r(0, s.SR_TID_X),
        s.s2r(1, s.SR_CTAID_X),
        s.uldc64(4, 0x118),
        s.mov_imm(11, 4),
        s.imad_rcr(0, 1, 0x0, 0),
        s.imad_wide_rrc(8, 0, 11, 0x168),
    ] + make + [s.mufu(12, 4, fn), s.stg(8, 0, 12), s.exit_(), s.bra_self()]
    return [s.sched(w) for w in body]


def dump(name, work, cub, run):
    fn, n, kind, *_ = FUNCS[name]
    path = os.path.join(work, f'{name.lower()}.bin')
    if os.path.exists(path):
        return np.fromfile(path, '<u4')
    ws = sweep(fn, kind)
    jp = os.path.join(work, 'sweep.bin')
    with open(jp, 'wb') as f:
        f.write(struct.pack('<I', 1) + struct.pack('<I', len(ws)) + s.words_bytes(ws) + struct.pack('<3I', n, 0, 1))
    subprocess.run([run, cub, jp, path], check=True)
    return np.fromfile(path, '<u4')


def fold_sq(t, k, nb):
    """The squarer: diagonal t_i at 2^(2i), cross 2 t_i t_j at 2^(i+j+1),
    columns from k only."""
    S = np.zeros_like(t)
    bits = [(t >> i) & 1 for i in range(nb)]
    for i in range(nb):
        if 2 * i >= k:
            S += bits[i] << (2 * i)
        for j in range(i + 1, nb):
            if i + j + 1 >= k:
                S += (bits[i] & bits[j]) << (i + j + 1)
    return S


def _lp(V, lo, hi, cost, idx):
    Vs = V[idx]
    A = np.vstack([-Vs, Vs])
    b = np.concatenate([-lo[idx], hi[idx]])
    return linprog(cost, A_ub=A, b_ub=b, bounds=[(None, None)] * V.shape[1], method='highs')


def extent(V, lo, hi, j, sgn, seed=0):
    """min (sgn 1) or max (sgn -1) of coefficient j over {c : lo <= V c <= hi},
    by cutting planes from a sample of the rows."""
    n = len(lo)
    rng = np.random.default_rng(seed)
    idx = np.unique(np.concatenate([[0, n - 1, n // 2], rng.choice(n, min(n, 600), replace=False)]))
    cost = np.zeros(V.shape[1])
    cost[j] = sgn
    for _ in range(80):
        r = _lp(V, lo, hi, cost, idx)
        if r.status != 0:
            return None
        g = V @ r.x
        viol = np.nonzero((g < lo - 1e-7) | (g > hi + 1e-7))[0]
        if len(viol) == 0:
            return r.x[j]
        worst = np.argsort(-np.maximum(lo[viol] - g[viol], g[viol] - hi[viol]))[:300]
        idx = np.unique(np.concatenate([idx, viol[worst]]))
    return None


def solve(lo, hi, t, S, maxn=40000):
    """Integers A, B, C with lo <= A + B t + C S <= hi; the first (A range,
    B, C) found, or None."""
    tn = float(max(t.max(), 1))
    sn = float(max(S.max(), 1))
    V = np.stack([np.ones(len(t)), t / tn, S / sn], 1)
    mid = (lo.astype(np.float64) + hi.astype(np.float64)) / 2
    c0, *_ = np.linalg.lstsq(V, mid - mid[0], rcond=None)
    B0 = int(round(c0[1] / tn))
    C0 = int(round(c0[2] / sn))
    bi = B0 * t + C0 * S
    bi = bi + int(np.median(lo - bi))
    lf = (lo - bi).astype(np.float64) - 0.5
    hf = (hi - bi).astype(np.float64) + 0.5
    ext = []
    for j in (1, 2):
        a = extent(V, lf, hf, j, 1)
        b = extent(V, lf, hf, j, -1)
        if a is None or b is None:
            return None
        ext.append((a, b))
    (b0, b1), (q0, q1) = ext
    Bs = range(B0 + math.floor(b0 / tn) - 1, B0 + math.ceil(b1 / tn) + 2)
    Cs = range(C0 + math.floor(q0 / sn) - 1, C0 + math.ceil(q1 / sn) + 2)
    if len(Bs) * len(Cs) > maxn:
        raise RuntimeError(f'{len(Bs) * len(Cs)} candidates')
    for B in Bs:
        bl = lo - B * t
        bh = hi - B * t
        for C in Cs:
            a0 = (bl - C * S).max()
            a1 = (bh - C * S).min()
            if a0 <= a1:
                return (int(a0), int(a1), B, C)
    return None


def targets(raw, target):
    """Per input, the interval [lo, hi] the interpolator must land in."""
    if target[0] == 'up':
        _, K, scale = target
        y = (raw.view(np.float32).astype(np.float64) * scale).astype(np.int64)
        return y << K, ((y + 1) << K) - 1
    _, K, r = target
    f = raw.view(np.float32)
    y = f.astype(np.float64)
    e = ((raw >> 23) & 0xff).astype(np.int64)
    u = np.ldexp(1.0, np.maximum(e, 1) - 150)
    lo = np.ceil(y * 2.0**K).astype(np.int64)
    hi = np.ceil((y + u) * 2.0**K).astype(np.int64) - 1
    return lo << r, ((hi + 1) << r) - 1


G = {}


def _row(k):
    n = 1 << G['sb']
    sl = slice(k * n, (k + 1) * n)
    m = G['mask'][sl]
    t = np.arange(n, dtype=np.int64)
    return k, solve(G['lo'][sl][m], G['hi'][sl][m], t[m], G['S'][m])


def fit(name, raw, procs):
    fn, n, kind, sb, sqk, target = FUNCS[name]
    lo, hi = targets(raw, target)
    mask = np.ones(n, bool)
    if name in ('RSQ', 'LG2'):
        mask[0] = False
    if name == 'RCP':
        lo[0], hi[0] = (1 << 24) << 16, ((1 << 24) + 1 << 16) - 1
    t = np.arange(1 << sb, dtype=np.int64)
    G.update(lo=lo, hi=hi, sb=sb, mask=mask, S=fold_sq(t, sqk, sb) >> sqk)
    with mp.get_context('fork').Pool(procs) as p:
        rows = p.map(_row, range(n >> sb))
    bad = [k for k, r in rows if r is None]
    if bad:
        raise SystemExit(f'{name}: no coefficients for rows {bad[:8]}')
    return [r for _, r in rows]


def emit(name, rows):
    out = []
    for a, _, b, c in rows:
        out.append('    [%#010x, %#x, %#010x, %#010x],' % (a & 0xffffffff, (a >> 32) & 0xffffffff, b & 0xffffffff,
                                                        c & 0xffffffff))
    return 'pub(super) const %s: [[u32; 4]; %d] = [\n%s\n];\n' % (name, len(rows), '\n'.join(out))


def main():
    work, dest = sys.argv[1], sys.argv[2]
    if not os.path.isabs(work) or not os.path.isabs(dest):
        raise SystemExit('the work dir and the output must be absolute paths')
    names = sys.argv[3:] or list(FUNCS)
    os.makedirs(work, exist_ok=True)
    cub, run = gen.build(work)
    parts = []
    for name in FUNCS:
        if name not in names:
            continue
        raw = dump(name, work, cub, run)
        rows = fit(name, raw, procs=8)
        json.dump(rows, open(os.path.join(work, f'{name.lower()}.json'), 'w'))
        parts.append(emit(name, rows))
        print(name, len(rows), 'rows', flush=True)
    if set(names) == set(FUNCS):
        open(dest, 'w').write(HEADER + '\n'.join(parts))


if __name__ == '__main__':
    main()
