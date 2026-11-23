#!/usr/bin/env python3
"""Records the known-answer vectors of ../tests/kat from a CUDA device.

    gen.py <absolute out dir> [family ...]

Needs nvcc and an SM86 device (an RTX 3090 for the committed files). Each
family is a set of instruction variants that share one input set; every
variant runs as its own kernel (see sass.wrapper) on every input row, and
the device's outputs are written next to the inputs. See README.md.
"""
import os
import random
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sass as s  # noqa: E402
MAGIC = b'RLKAT\x01\x00\x00'
PREFIX = s.wrapper([], [])[:-2]  # everything before the body


def u32(x):
    return x & 0xffffffff


def f32(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


# --- input pools -----------------------------------------------------------

F32_EDGE = [
    0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x00000003, 0x007fffff, 0x807fffff, 0x00400000,
    0x00800000, 0x80800000, 0x00800001, 0x00ffffff, 0x01000000, 0x3f800000, 0xbf800000, 0x3f800001,
    0x3f7fffff, 0x40000000, 0x3f000000, 0x3effffff, 0x3fc00000, 0x40200000, 0xbfc00000, 0x40400000,
    0x7f7fffff, 0xff7fffff, 0x7f000000, 0x7effffff, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000,
    0x7f800001, 0xff800001, 0x7fa00000, 0x7fffffff, 0x7fc00001, 0x33800000, 0x34000000, 0x4b000000,
    0x4b800000, 0x4b7fffff, 0x0c000000, 0x1f800000, 0x5f800000, 0x60000000, 0xe0000000, 0x26800000,
]

F32_INT_EDGE = [
    0x42fe0000, 0x43000000, 0x437f0000, 0x43800000, 0x46fffe00, 0x47000000, 0x477fff00, 0x47800000,
    0x4effffff, 0x4f000000, 0x4f7fffff, 0x4f800000, 0x5effffff, 0x5f000000, 0x5f7fffff, 0x5f800000,
    0xc3000000, 0xc3010000, 0xc7000000, 0xc7000080, 0xcf000000, 0xcf000001, 0xdf000000, 0xdf000001,
    0x3f000001, 0x3f7fffff, 0xbf000000, 0xbf000001, 0x3fc00000, 0x40200000, 0x40600000, 0xc0200000,
    0xbfc00000, 0xbe800000, 0x4b000001, 0x4b400001, 0xcb400001, 0x4affffff, 0x477fefff, 0x477ff000,
]


def rnd_f32(r, elo=0, ehi=255):
    return (r.getrandbits(1) << 31) | (r.randint(elo, ehi) << 23) | r.getrandbits(23)


def rnd_mant(r, e, sign=None):
    sg = r.getrandbits(1) if sign is None else sign
    return (sg << 31) | (e << 23) | r.getrandbits(23)


def f32_pool(r, n):
    """Mixed: random exponents across the range, near one, near the denormal
    edge, with short mantissas (exact-ish results and ties)."""
    out = []
    for _ in range(n):
        k = r.randrange(8)
        if k == 0:
            out.append(r.choice(F32_EDGE))
        elif k == 1:
            out.append(rnd_f32(r))
        elif k == 2:
            out.append(rnd_f32(r, 0, 4))
        elif k == 3:
            out.append(rnd_f32(r, 250, 254))
        elif k == 4:
            e = r.randint(110, 145)
            out.append((r.getrandbits(1) << 31) | (e << 23) | (r.getrandbits(4) << 19))
        else:
            out.append(rnd_f32(r, 100, 155))
    return out


def binary_rows(r, n):
    """Pairs: every edge against every edge, then pairs built to round at
    a tie, to cancel, to land on the denormal and overflow edges."""
    rows = []
    edge = F32_EDGE[:36]
    for a in edge:
        for b in edge:
            rows.append((a, b))
    while len(rows) < n:
        k = r.randrange(10)
        if k == 0:  # a + tiny: halfway and quarter cases
            a = rnd_mant(r, r.randint(100, 150))
            ea = (a >> 23) & 0xff
            b = ((r.getrandbits(1) << 31) | ((ea - 24) << 23) | (r.choice([0, 0x400000, 1, 0x7fffff]))) & 0xffffffff
            rows.append((a, b))
        elif k == 1:  # near cancellation
            a = rnd_mant(r, r.randint(1, 254))
            b = (a ^ 0x80000000) + r.randint(-3, 3)
            rows.append((a, u32(b)))
        elif k == 2:  # tiny results
            rows.append((rnd_f32(r, 0, 3), rnd_f32(r, 0, 3)))
        elif k == 3:  # products near the denormal edge
            e = r.randint(0, 127)
            rows.append((rnd_mant(r, 127 - 63 + r.randint(-2, 2)), rnd_mant(r, max(0, 64 - e) if e < 64 else 1)))
        elif k == 4:  # products near overflow
            rows.append((rnd_mant(r, r.randint(190, 200)), rnd_mant(r, r.randint(190, 200))))
        elif k == 5:  # short mantissas: exact products, ties in sums
            rows.append((rnd_short(r), rnd_short(r)))
        else:
            ea = r.randint(1, 254)
            rows.append((rnd_mant(r, ea), rnd_mant(r, max(0, min(254, ea + r.randint(-30, 30))))))
    return rows[:n]


def rnd_short(r):
    e = r.randint(100, 150)
    return (r.getrandbits(1) << 31) | (e << 23) | (r.getrandbits(12) << 11)


def ternary_rows(r, n):
    rows = []
    edge = F32_EDGE
    while len(rows) < n:
        k = r.randrange(10)
        if k < 2:
            rows.append((r.choice(edge), r.choice(edge), r.choice(edge)))
        elif k == 2:  # a*b exact, + c at a tie
            a, b = rnd_short(r), rnd_short(r)
            p = struct.unpack('<f', struct.pack('<I', a))[0] * struct.unpack('<f', struct.pack('<I', b))[0]
            ep = (f32(p) >> 23) & 0xff
            c = ((r.getrandbits(1) << 31) | (max(0, ep - 25 - r.randint(0, 2)) << 23) | r.choice([0, 0x400000]))
            rows.append((a, b, u32(c)))
        elif k == 3:  # c ~ -a*b: cancellation
            a, b = rnd_f32(r, 100, 150), rnd_f32(r, 100, 150)
            p = struct.unpack('<f', struct.pack('<I', a))[0] * struct.unpack('<f', struct.pack('<I', b))[0]
            c = f32(-p) + r.randint(-2, 2)
            rows.append((a, b, u32(c)))
        elif k == 4:  # tiny products, tiny addends
            rows.append((rnd_f32(r, 40, 80), rnd_f32(r, 40, 80), rnd_f32(r, 0, 3)))
        elif k == 5:  # overflowing products, big addends
            rows.append((rnd_f32(r, 190, 200), rnd_f32(r, 190, 200), rnd_f32(r, 250, 254)))
        elif k == 6:
            rows.append((rnd_short(r), rnd_short(r), rnd_short(r)))
        else:
            rows.append((rnd_f32(r, 90, 160), rnd_f32(r, 90, 160), rnd_f32(r, 70, 190)))
    return rows


def int_pool(r, n):
    sp = [0, 1, 2, 3, 0xffffffff, 0xfffffffe, 0x7fffffff, 0x80000000, 0x80000001, 0xffff, 0x10000, 0xff,
          0x100, 31, 32, 33, 63, 64, 65, 0x55555555, 0xaaaaaaaa, 0x00ff00ff, 0x12345678]
    out = []
    for _ in range(n):
        k = r.randrange(8)
        if k < 2:
            out.append(r.choice(sp))
        elif k == 2:
            out.append(r.randrange(70))
        elif k == 3:
            out.append(u32(-r.randrange(70)))
        elif k == 4:
            out.append(1 << r.randrange(32))
        elif k == 5:
            out.append(u32((1 << r.randrange(33)) - r.randint(0, 2)))
        else:
            out.append(r.getrandbits(32))
    return out


H16_EDGE = [0x0000, 0x8000, 0x0001, 0x8001, 0x03ff, 0x0200, 0x0400, 0x8400, 0x3c00, 0xbc00, 0x3c01, 0x3bff,
            0x3800, 0x4000, 0x7bff, 0xfbff, 0x7c00, 0xfc00, 0x7e00, 0xfe00, 0x7c01, 0x7d00, 0x7fff, 0x5bff,
            0x5c00, 0x1400, 0x0800, 0x6800, 0x77ff, 0x3555, 0x4248, 0x2e66]


def rnd_h16(r):
    k = r.randrange(5)
    if k == 0:
        return r.choice(H16_EDGE)
    if k == 1:
        return r.getrandbits(16)
    if k == 2:
        return (r.getrandbits(1) << 15) | (r.randint(0, 2) << 10) | r.getrandbits(10)
    if k == 3:
        return (r.getrandbits(1) << 15) | (r.randint(27, 30) << 10) | r.getrandbits(10)
    return (r.getrandbits(1) << 15) | (r.randint(10, 20) << 10) | (r.getrandbits(5) << 5)


def h2(r):
    return (rnd_h16(r) << 16) | rnd_h16(r)


def half_rows(r, n):
    rows = []
    for a in H16_EDGE:
        for b in H16_EDGE:
            rows.append(((a << 16) | b, (b << 16) | a, (r.choice(H16_EDGE) << 16) | r.choice(H16_EDGE)))
    while len(rows) < n:
        rows.append((h2(r), h2(r), h2(r)))
    return rows[:n]


# --- families --------------------------------------------------------------
# A family: (name, rows, variants); a row is up to 4 input words (R4..R7),
# a variant is (label, body, output registers).

def v(label, body, outs):
    return (label, body if isinstance(body, list) else [body], outs)


ROUNDS = ['RN', 'RM', 'RP', 'RZ']


def fam_fadd(r):
    vs = []
    for ftz in (0, 1):
        for m in ROUNDS:
            vs.append(v(f'FADD{".FTZ" if ftz else ""}.{m}', s.fadd(12, 4, 5, s.RND[m], ftz), [12]))
    vs.append(v('FADD.SAT', s.fadd(12, 4, 5, sat=1), [12]))
    vs.append(v('FADD.FTZ.RZ.SAT', s.fadd(12, 4, 5, 3, 1, 1), [12]))
    vs.append(v('FADD |a|, -b', s.fadd(12, 4, 5, absa=1, negb=1), [12]))
    return ('fadd', binary_rows(r, 1600), vs)


def fam_fmul(r):
    vs = []
    for ftz in (0, 1):
        for m in ROUNDS:
            vs.append(v(f'FMUL{".FTZ" if ftz else ""}.{m}', s.fmul(12, 4, 5, s.RND[m], ftz), [12]))
    vs.append(v('FMUL.SAT', s.fmul(12, 4, 5, sat=1), [12]))
    vs.append(v('FMUL.D2', s.fmul(12, 4, 5, scale=3), [12]))
    vs.append(v('FMUL.M8.RP', s.fmul(12, 4, 5, rnd=2, scale=7), [12]))
    vs.append(v('FMUL.FTZ.D8.RZ', s.fmul(12, 4, 5, rnd=3, ftz=1, scale=1), [12]))
    return ('fmul', binary_rows(r, 1600), vs)


def fam_ffma(r):
    vs = []
    for ftz in (0, 1):
        for m in ROUNDS:
            vs.append(v(f'FFMA{".FTZ" if ftz else ""}.{m}', s.ffma(12, 4, 5, 6, s.RND[m], ftz), [12]))
    vs.append(v('FFMA.SAT', s.ffma(12, 4, 5, 6, sat=1), [12]))
    vs.append(v('FFMA -a, b, -c', s.ffma(12, 4, 5, 6, nega=1, negc=1), [12]))
    return ('ffma', ternary_rows(r, 1600), vs)


def fam_fcmp(r):
    vs = []
    for ftz in (0, 1):
        vs.append(v(f'FMNMX{".FTZ" if ftz else ""} min, max',
                    [s.fmnmx(12, 4, 5, p=s.PT, ftz=ftz), s.fmnmx(13, 4, 5, p=s.PT, pnot=1, ftz=ftz)], [12, 13]))
    for cmp in ('LT', 'EQ', 'LE', 'GT', 'NE', 'GE', 'NUM', 'NAN', 'LTU', 'EQU', 'GEU', 'NEU'):
        vs.append(v(f'FSETP.{cmp} / .FTZ', [s.fsetp(0, 1, cmp, 4, 5), s.fsetp(2, 3, cmp, 4, 5, ftz=1),
                                            s.p2r(12, 0xf)], [12]))
    return ('fcmp', binary_rows(r, 1400), vs)


def fam_f2i(r):
    rows = [(x,) for x in F32_EDGE + F32_INT_EDGE]
    while len(rows) < 900:
        k = r.randrange(4)
        if k == 0:
            e = r.randint(118, 192)
            rows.append(((r.getrandbits(1) << 31) | (e << 23) | (r.getrandbits(6) << 17),))
        elif k == 1:  # k + 0.5
            kk = r.randint(-300, 300)
            rows.append((f32(kk + 0.5),))
        else:
            rows.append((rnd_f32(r, 100, 195),))
    vs = []
    for signed in (1, 0):
        for size in (0, 1, 2, 3):
            for m in range(4):
                name = f'F2I.{"S" if signed else "U"}{8 << size}.{["RN", "FLOOR", "CEIL", "TRUNC"][m]}'
                outs = [12, 13] if size == 3 else [12]
                vs.append(v(name, s.f2i(12, 4, signed, size, m), outs))
    for m in range(4):
        vs.append(v(f'F2I.FTZ.S32.{m}', s.f2i(12, 4, 1, 2, m, ftz=1), [12]))
    vs.append(v('F2I.NTZ.S32.TRUNC', s.f2i(12, 4, 1, 2, 3, ntz=1), [12]))
    vs.append(v('F2I.FTZ.NTZ.U64', s.f2i(12, 4, 0, 3, 0, ntz=1, ftz=1), [12, 13]))
    return ('f2i', rows, vs)


def fam_f2i_f16(r):
    rows = [((a << 16) | b,) for a, b in zip(H16_EDGE, reversed(H16_EDGE))]
    while len(rows) < 600:
        rows.append((h2(r),))
    vs = []
    for sel in (0, 1):
        for signed, size in ((1, 2), (0, 2), (1, 1), (0, 0), (1, 3)):
            for m in range(4):
                outs = [12, 13] if size == 3 else [12]
                vs.append(v(f'F2I.{"S" if signed else "U"}{8 << size}.F16.{m} H{sel}',
                            s.f2i(12, 4, signed, size, m, sty=1, sel=sel), outs))
    return ('f2i_f16', rows, vs)


def fam_i2f(r):
    edge = [0, 1, 0xffffffff, 0x7fffffff, 0x80000000, 0x80000001, 0x00ffffff, 0x01000001, 0x01000003, 0x01000002,
            0x02000006, 0xfeffffff, 0x0000ffe0, 0x0000fff0, 0x0000ffef, 0x00000801, 0x00000803, 0xffff8000, 0x7fff,
            0x00ff, 0x80, 0x7f, 0xff80, 0xffff, 0x10000]
    rows = []
    for a in edge:
        rows.append((a, r.choice(edge), 0, 0))
    while len(rows) < 900:
        k = r.randrange(4)
        lo = int_pool(r, 1)[0]
        if k == 0:
            hi = r.getrandbits(32)
        elif k == 1:
            hi = r.choice([0, 0xffffffff, 1, 0x80000000, 0x7fffffff])
        else:
            hi = u32((1 << r.randrange(32)) - r.randint(0, 1))
        rows.append((lo, hi, 0, 0))
    vs = []
    for signed in (1, 0):
        for size in (2, 3):
            for m in ROUNDS:
                vs.append(v(f'I2F.F32.{"S" if signed else "U"}{8 << size}.{m}',
                            s.i2f(12, 4, signed, size, s.RND[m]), [12]))
    for signed in (1, 0):
        for size, sel in ((0, 0), (0, 3), (1, 0), (1, 1)):
            vs.append(v(f'I2F.F32.{"S" if signed else "U"}{8 << size} sel{sel}',
                        s.i2f(12, 4, signed, size, 0, sel=sel), [12]))
    for signed in (1, 0):
        for size in (1, 2):
            for m in ROUNDS:
                vs.append(v(f'I2F.F16.{"S" if signed else "U"}{8 << size}.{m}',
                            s.i2f(12, 4, signed, size, s.RND[m], dty=1), [12]))
    for signed in (1, 0):
        for m in (0, 3):
            vs.append(v(f'I2FP.F32.{"S" if signed else "U"}32.{"RZ" if m else "RN"}', s.i2fp(12, 4, signed, m), [12]))
    return ('i2f', rows, vs)


def fam_f2f(r):
    hedge = [0x477fe000, 0x477fefff, 0x477ff000, 0x477ff001, 0x477fffff, 0x47800000, 0x387fc000, 0x38800000,
             0x387fe000, 0x33800000, 0x33000000, 0x33000001, 0x337fffff, 0x33c00000, 0x34000000, 0x34400000,
             0x3f801000, 0x3f803000, 0x3f800fff, 0x3f801001, 0xc77ff000, 0xb3000000, 0xb3000001]
    rows = [(x, 0, 0, 0) for x in F32_EDGE + hedge]
    for a, b in zip(H16_EDGE, reversed(H16_EDGE)):
        rows.append((r.getrandbits(32), (a << 16) | b, 0, 0))
    while len(rows) < 900:
        k = r.randrange(3)
        x = rnd_f32(r, 95, 145) if k else rnd_f32(r, 100, 115)
        rows.append((x, h2(r), 0, 0))
    vs = []
    for ftz in (0, 1):
        for m in ROUNDS:
            vs.append(v(f'F2F.F16.F32{".FTZ" if ftz else ""}.{m}', s.f2f(12, 4, 1, 2, s.RND[m], ftz), [12]))
    for ftz in (0, 1):
        for sel in (0, 1):
            vs.append(v(f'F2F.F32.F16{".FTZ" if ftz else ""} H{sel}', s.f2f(12, 5, 2, 1, 0, ftz, sel), [12]))
    # F2F.F32.F32 is an illegal instruction on SM86.
    return ('f2f', rows, vs)


def fam_frnd(r):
    rows = [(x, 0, 0, 0) for x in F32_EDGE + F32_INT_EDGE]
    for a, b in zip(H16_EDGE, reversed(H16_EDGE)):
        rows.append((r.getrandbits(32), (a << 16) | b, 0, 0))
    while len(rows) < 800:
        k = r.randrange(3)
        if k == 0:
            rows.append((f32(r.randint(-200, 200) + 0.5), h2(r), 0, 0))
        else:
            rows.append((rnd_f32(r, 110, 160), h2(r), 0, 0))
    vs = []
    for ftz in (0, 1):
        for m in range(4):
            vs.append(v(f'FRND{".FTZ" if ftz else ""}.{["RN", "FLOOR", "CEIL", "TRUNC"][m]}',
                        s.frnd(12, 4, 2, m, ftz), [12]))
    for sel in (0, 1):
        for m in range(4):
            vs.append(v(f'FRND.F16.{m} H{sel}', s.frnd(12, 5, 1, m, 0, sel), [12]))
    return ('frnd', rows, vs)


def fam_f2fp(r):
    rows = [(a, b, 0, 0) for a, b in binary_rows(r, 900)]
    hedge = [0x477fe000, 0x477ff000, 0x477ff001, 0x47800000, 0x387fc000, 0x33000000, 0x33000001, 0x337fffff,
             0x3f808000, 0x3f818000, 0x7f7f8000, 0x7f7f7fff, 0xff7f8000]
    for a in hedge:
        for b in hedge[:6]:
            rows.append((a, b, 0, 0))
    vs = []
    for bf in (0, 1):
        for rz in (0, 1):
            for relu in (0, 1):
                vs.append(v(f'F2FP{".RELU" if relu else ""}{".BF16" if bf else ""}.PACK_AB{".RZ" if rz else ""}',
                            s.f2fp(12, 4, 5, relu, bf, 0, rz), [12]))
    vs.append(v('F2FP.SATFINITE.PACK_AB', s.f2fp(12, 4, 5, sat=1), [12]))
    vs.append(v('F2FP.SATFINITE.BF16.PACK_AB.RZ', s.f2fp(12, 4, 5, bf16=1, sat=1, rz=1), [12]))
    return ('f2fp', rows, vs)


def fam_half(r):
    vs = []
    for op, nm in ((0x030, 'HADD2'), (0x032, 'HMUL2')):
        vs.append(v(nm, s.hop(op, 12, 4, 5), [12]))
        vs.append(v(f'{nm}.FTZ', s.hop(op, 12, 4, 5, ftz=1), [12]))
        vs.append(v(f'{nm}.SAT', s.hop(op, 12, 4, 5, sat=1), [12]))
        vs.append(v(f'{nm} -|a|.H1_H1, b.H0_H0', s.hop(op, 12, 4, 5, nega=1, absa=1, swa=3, swb=2), [12]))
        # Bit 85 (BF16_V2 in the decoder) on HADD2 and HMUL2.
        vs.append(v(f'{nm} bit 85', s.hop(op, 12, 4, 5, bf=1), [12]))
    # The .F32 forms are legal only as HADD2.F32 d, -RZ, b (half to float).
    for swb, lane in ((0, ''), (2, '.H0_H0'), (3, '.H1_H1')):
        vs.append(v(f'HADD2.F32 -RZ, b{lane}', s.hop(0x030, 12, s.RZ, 5, f32=1, nega=1, swb=swb), [12]))
    vs.append(v('HADD2.F32 -RZ, -|b|.H1_H1', s.hop(0x030, 12, s.RZ, 5, f32=1, nega=1, negb=1, absb=1, swb=3), [12]))
    vs.append(v('HADD2.F32.FTZ -RZ, b.H1_H1', s.hop(0x030, 12, s.RZ, 5, f32=1, nega=1, ftz=1, swb=3), [12]))
    vs.append(v('HADD2.F32.SAT -RZ, b.H0_H0', s.hop(0x030, 12, s.RZ, 5, f32=1, nega=1, sat=1, swb=2), [12]))
    vs.append(v('HMUL2.FMZ', s.hop(0x032, 12, 4, 5, fmz=1), [12]))
    vs.append(v('HFMA2', s.hop(0x031, 12, 4, 5, 6), [12]))
    vs.append(v('HFMA2.FTZ', s.hop(0x031, 12, 4, 5, 6, ftz=1), [12]))
    vs.append(v('HFMA2.SAT', s.hop(0x031, 12, 4, 5, 6, sat=1), [12]))
    vs.append(v('HFMA2.RELU', s.hop(0x031, 12, 4, 5, 6, relu=1), [12]))
    vs.append(v('HFMA2.FMZ', s.hop(0x031, 12, 4, 5, 6, fmz=1), [12]))
    vs.append(v('HFMA2 a, -b.H1_H1, -c.H0_H0', s.hop(0x031, 12, 4, 5, 6, negb=1, swb=3, negc=1, swc=2), [12]))
    vs.append(v('HFMA2.BF16_V2', s.hop(0x031, 12, 4, 5, 6, bf=1), [12]))
    vs.append(v('HMNMX2 min, max', [s.hmnmx2(12, 4, 5), s.hmnmx2(13, 4, 5, pnot=1)], [12, 13]))
    vs.append(v('HMNMX2.NAN min', s.hmnmx2(12, 4, 5, nan=1), [12]))
    return ('half', half_rows(r, 1400), vs)


def fam_int(r):
    rows = []
    for _ in range(700):
        a, b, c = int_pool(r, 3)
        rows.append((a, b, c, r.getrandbits(32)))
    R2P = s.W(0x004, 4, [(24, 8, 7), (32, 32, 0x7f)])
    vs = [
        v('IADD3 P0, P1', [s.iadd3(12, 4, 5, 6, p0=0, p1=1), s.p2r(13, 3)], [12, 13]),
        v('IADD3 -a, b, -c P0, P1', [s.iadd3(12, 4, 5, 6, p0=0, p1=1, neg=(1, 0, 1)), s.p2r(13, 3)], [12, 13]),
        v('IADD3 a, -b, c P0', [s.iadd3(12, 4, 5, 6, p0=0, neg=(0, 1, 0)), s.p2r(13, 3)], [12, 13]),
        v('IADD3.X P2, P3 <- P0, P1', [R2P, s.iadd3(12, 4, 5, 6, p0=2, p1=3, x=1, cin0=(0, 0), cin1=(1, 0)),
                                       s.p2r(13, 0xc)], [12, 13]),
        v('IADD3.X ~a, b, ~c, !P0, P1', [R2P, s.iadd3(12, 4, 5, 6, p0=2, x=1, neg=(1, 0, 1), cin0=(0, 1),
                                                       cin1=(1, 0)), s.p2r(13, 0x4)], [12, 13]),
        v('64-bit add', [s.iadd3(12, 4, 6, s.RZ, p0=0), s.iadd3(13, 5, 7, s.RZ, x=1, cin0=(0, 0))], [12, 13]),
        v('IMAD', s.imad('lo', 12, 4, 5, 6), [12]),
        v('IMAD.WIDE.U32', s.imad('wide', 12, 4, 5, 6), [12, 13]),
        v('IMAD.WIDE', s.imad('wide', 12, 4, 5, 6, signed=1), [12, 13]),
        v('IMAD.HI.U32', s.imad('hi', 12, 4, 5, 6), [12]),
        v('IMAD.HI', s.imad('hi', 12, 4, 5, 6, signed=1), [12]),
        v('IMAD.WIDE.U32 P0 + IMAD.HI.U32.X', [s.imad('wide', 12, 4, 5, 6, p=0), s.imad('hi', 14, 4, 5, 6, x=1, cin=(0, 0)),
                                               s.p2r(15, 1)], [12, 13, 14, 15]),
        v('IMAD.X <- P0', [R2P, s.imad('lo', 12, 4, 5, 6, x=1, cin=(0, 0))], [12]),
        v('IMAD.X ~c <- P1', [R2P, s.imad('lo', 12, 4, 5, 6, x=1, cin=(1, 0), cneg=1)], [12]),
        v('LEA P0', [s.lea(12, 4, 5, 5, pd=0), s.p2r(13, 1)], [12, 13]),
        v('LEA.HI.X', [R2P, s.lea(12, 4, 5, 7, c=6, hi=1, x=1, ps=(0, 0))], [12]),
        v('LEA.HI.X.SX32', [R2P, s.lea(12, 4, 5, 3, hi=1, x=1, sx=1, ps=(0, 0))], [12]),
        v('LEA -a', s.lea(12, 4, 5, 2, nega=1), [12]),
        v('LEA -a P0', [s.lea(12, 4, 5, 9, nega=1, pd=0), s.p2r(13, 1)], [12, 13]),
        v('LEA.HI -a', s.lea(12, 4, 5, 7, c=6, hi=1, nega=1), [12]),
        v('LEA.HI.X -a', [R2P, s.lea(12, 4, 5, 7, c=6, hi=1, x=1, ps=(0, 0), nega=1)], [12]),
        v('IMNMX min, max, U32 min, max', [s.imnmx(12, 4, 5), s.imnmx(13, 4, 5, pnot=1), s.imnmx(14, 4, 5, signed=0),
                                           s.imnmx(15, 4, 5, signed=0, pnot=1)], [12, 13, 14, 15]),
        v('IABS', s.iabs(12, 4), [12]),
        v('FLO.U32, FLO, FLO.SH, POPC, BREV', [s.flo(12, 4), s.flo(13, 4, signed=1), s.flo(14, 4, sh=1),
                                                s.popc(15, 4), s.brev(16, 4)], [12, 13, 14, 15, 16]),
        v('ISETP 64-bit chains', [s.isetp(0, s.PT, 'LT', 4, 6, signed=0), s.isetp(1, s.PT, 'LT', 5, 7, ex=1, ep=(0, 0)),
                                  s.isetp(2, s.PT, 'EQ', 4, 6, signed=0), s.isetp(3, s.PT, 'GE', 5, 7, ex=1, ep=(2, 0)),
                                  s.p2r(12, 0xf)], [12]),
    ]
    for mode in range(7):
        vs.append(v(f'PRMT mode {mode}', s.prmt(12, 4, 5, 6, mode), [12]))
    return ('int', rows, vs)


def fam_shf(r):
    rows = []
    for _ in range(500):
        a, b, c = int_pool(r, 3)
        rows.append((a, b if r.randrange(3) else b & 0x7f, c))
    vs = []
    for left in (0, 1):
        for ty in range(4):
            for wrap in (0, 1):
                for hi in (0, 1):
                    nm = f'SHF.{"L" if left else "R"}{".W" if wrap else ""}.{["S64", "U64", "S32", "U32"][ty]}{".HI" if hi else ""}'
                    vs.append(v(nm, s.shf(12, 4, 5, 6, left, wrap, ty, hi), [12]))
    return ('shf', rows, vs)


def mufu_rows(r, fn, n):
    rows = [x for x in F32_EDGE]
    if fn in ('RCP', 'RSQ', 'SQRT'):
        rows += [(127 << 23) | (k << 16) | r.getrandbits(16) for k in range(128)]   # every table index
        rows += [(126 << 23) | (k << 17) | r.getrandbits(17) for k in range(64)]
        rows += [(e << 23) | r.getrandbits(23) for e in range(256)]                  # every exponent
        while len(rows) < n:
            rows.append(rnd_f32(r) if r.randrange(2) else rnd_mant(r, r.choice([0, 1, 2, 125, 126, 127, 128, 252, 253, 254])))
    elif fn == 'EX2':
        rows += [f32(x / 8) for x in range(-1200, 1210, 7)]
        rows += [f32(k) for k in (-150, -149, -127, -126, -125, 127, 128, 129)]
        rows += [u32(f32(k) + d) for k in (-150, -149, -126, 127, 128, 1, -1) for d in (-1, 1)]
        while len(rows) < n:
            rows.append(rnd_mant(r, r.randint(100, 134)) if r.randrange(3) else rnd_mant(r, r.randint(100, 127), 0))
    elif fn == 'LG2':
        rows += [(127 << 23) | (k << 17) | r.getrandbits(17) for k in range(64)]
        rows += [0x3f800000 + d for d in range(-40, 41)]
        rows += [(e << 23) | r.getrandbits(23) for e in range(256)]
        while len(rows) < n:
            rows.append(rnd_f32(r) if r.randrange(2) else rnd_mant(r, r.randint(120, 134), 0))
    else:  # SIN, COS take the operand in turns: every quarter, near the ends
        rows += [f32(k / 4) for k in range(-40, 41)]
        rows += [u32(f32(k / 4) + d) for k in range(-8, 9) for d in (-2, -1, 1, 2) if k]
        rows += [f32(k / 64) for k in range(256)]
        while len(rows) < n:
            k = r.randrange(4)
            rows.append(rnd_mant(r, r.randint(100, 127)) if k < 2 else rnd_mant(r, r.randint(127, 160)) if k == 2
                        else rnd_f32(r))
    return [(x & 0xffffffff, 0, 0, 0) for x in rows[:n]]


def fam_mufu(fn):
    def f(r):
        rows = mufu_rows(r, fn, 2600)
        return (f'mufu_{fn.lower()}', rows, [v(f'MUFU.{fn}', s.mufu(12, 4, fn), [12])])
    f.__name__ = f'fam_mufu_{fn.lower()}'
    return f


def fam_mufu16(r):
    rows = [((a << 16) | b, 0, 0, 0) for a in H16_EDGE for b in H16_EDGE[:8]]
    while len(rows) < 1200:
        rows.append((h2(r), 0, 0, 0))
    vs = []
    for fn in ('RCP', 'RSQ', 'SQRT', 'EX2', 'LG2', 'SIN', 'COS'):
        for h1 in (0, 1):
            vs.append(v(f'MUFU.{fn}.F16 H{h1}', s.mufu(12, 4, fn, f16=1, h1=h1), [12]))
    return ('mufu_f16', rows, vs)


def fam_mufu64(r):
    rows = [(0, x, 0, 0) for x in [0, 0x80000000, 1, 0x000fffff, 0x00100000, 0x3ff00000, 0xbff00000, 0x3fefffff,
                                   0x7fefffff, 0x7ff00000, 0xfff00000, 0x7ff80000, 0x7ff00001, 0xfff80000]]
    rows += [(0, (1023 << 20) | (k << 13) | r.getrandbits(13), 0, 0) for k in range(128)]
    rows += [(0, (1022 << 20) | (k << 14) | r.getrandbits(14), 0, 0) for k in range(64)]
    while len(rows) < 1200:
        rows.append((r.getrandbits(32), r.getrandbits(32), 0, 0))
    return ('mufu_64h', rows, [v('MUFU.RCP64H', s.mufu(12, 5, 'RCP64H'), [12]),
                               v('MUFU.RSQ64H', s.mufu(12, 5, 'RSQ64H'), [12])])


def fam_ftz_chain(r):
    """A double to float, FMUL by an immediate 1 (with and without FTZ), back to double: float(x) denormal for
    many rows, where only the FTZ multiply flushes."""
    rows = []
    for e in list(range(1023 - 160, 1023 - 120)) + [1023 - 126, 1023 - 127, 1023 - 149, 1023 - 150, 1023, 0, 2047]:
        for k in range(8):
            hi = (r.getrandbits(1) << 31) | (e << 20) | r.getrandbits(20)
            rows.append((r.getrandbits(32), hi, 0, 0))
    rows += [(0, 0, 0, 0), (0, 0x80000000, 0, 0), (0, 0x3ff00000, 0, 0), (0, 0x36a00000, 0, 0),
             (0, 0x37f00000, 0, 0), (0, 0x380fffff, 0, 0), (0xffffffff, 0x380fffff, 0, 0)]
    while len(rows) < 600:
        rows.append((r.getrandbits(32), r.getrandbits(32), 0, 0))
    vs = []
    for ftz in (0, 1):
        body = [s.f2f(6, 4, 2, 3), s.fmul_imm(7, 6, 0x3f800000, ftz=ftz), s.f2f(12, 7, 3, 2)]
        vs.append(v(f'F2F.F32.F64, FMUL{".FTZ" if ftz else ""} 1, F2F.F64.F32', body, [12, 13, 6, 7]))
    return ('ftz_chain', rows, vs)


FAMILIES = [fam_fadd, fam_fmul, fam_ffma, fam_fcmp, fam_f2i, fam_f2i_f16, fam_i2f, fam_f2f, fam_frnd, fam_f2fp,
            fam_half, fam_int, fam_shf, fam_mufu16, fam_mufu64, fam_ftz_chain] + \
    [fam_mufu(fn) for fn in ('RCP', 'RSQ', 'SQRT', 'EX2', 'LG2', 'SIN', 'COS')]


# --- recording -------------------------------------------------------------

def build(work):
    cub = os.path.join(work, 'skeleton.cubin')
    run = os.path.join(work, 'runner')
    subprocess.run(['nvcc', '-arch=sm_86', '-cubin', '-o', cub, os.path.join(HERE, 'skeleton.cu')], check=True)
    subprocess.run(['nvcc', '-O2', '-o', run, os.path.join(HERE, 'runner.cpp'), '-lcuda'], check=True)
    return cub, run


def record(name, rows, variants, cub, run, work):
    jobs = [struct.pack('<I', len(variants))]
    progs = []
    for label, body, outs in variants:
        ws = s.wrapper(body, outs)
        progs.append(ws)
        jobs.append(struct.pack('<I', len(ws)) + s.words_bytes(ws) + struct.pack('<3I', len(rows), s.NIN, s.NOUT))
        for row in rows:
            jobs.append(struct.pack('<4I', *(list(row) + [0] * (4 - len(row)))))
    jp = os.path.join(work, 'jobs.bin')
    rp = os.path.join(work, 'res.bin')
    open(jp, 'wb').write(b''.join(jobs))
    subprocess.run([run, cub, jp, rp], check=True)
    res = open(rp, 'rb').read()
    nin = max(max((k + 1 for k, x in enumerate(row) if x), default=1) for row in rows)
    out = [MAGIC, struct.pack('<4I', nin, len(rows), len(variants), len(PREFIX)), s.words_bytes(PREFIX)]
    for (label, body, outs), ws in zip(variants, progs):
        assert ws[:len(PREFIX)] == PREFIX
        lb = label.encode()
        tail = ws[len(PREFIX):]
        out.append(struct.pack('<H', len(lb)) + lb + struct.pack('<II', len(tail), len(outs)) + s.words_bytes(tail))
    for row in rows:
        out.append(struct.pack(f'<{nin}I', *(list(row[:nin]) + [0] * (nin - len(row[:nin])))))
    # Outputs, a column per variant and output register: the index of an
    # earlier column (the inputs, then the outputs in order) and the values
    # XORed with it, run-length coded (encode_col).
    cols = [[row[k] if k < len(row) else 0 for row in rows] for k in range(nin)]
    for j, (label, body, outs) in enumerate(variants):
        base = j * len(rows) * 64
        for k in range(len(outs)):
            col = [struct.unpack_from('<I', res, base + 64 * i + 4 * k)[0] for i in range(len(rows))]
            best = min(range(len(cols)), key=lambda c: len(encode_col(col, cols[c])))
            out.append(struct.pack('<H', best) + encode_col(col, cols[best]))
            cols.append(col)
    return b''.join(out)


def varint(x):
    b = bytearray()
    while True:
        if x < 0x80:
            b.append(x)
            return bytes(b)
        b.append((x & 0x7f) | 0x80)
        x >>= 7


def encode_col(col, prev):
    """Each value XOR the one before it in `prev`; a run of n zeros is the
    varint 2n+1, any other value d the varint 2d."""
    b = bytearray()
    zeros = 0
    for x, p in zip(col, prev):
        d = x ^ p
        if d == 0:
            zeros += 1
            continue
        if zeros:
            b += varint(2 * zeros + 1)
            zeros = 0
        b += varint(2 * d)
    if zeros:
        b += varint(2 * zeros + 1)
    return bytes(b)


def main():
    dest = sys.argv[1] if len(sys.argv) > 1 else ''
    if not os.path.isabs(dest):
        raise SystemExit('usage: gen.py <absolute out dir> [family ...]')
    want = set(sys.argv[2:])
    with tempfile.TemporaryDirectory() as work:
        cub, run = build(work)
        for fam in FAMILIES:
            name, rows, variants = fam(random.Random(fam.__name__))
            if want and name not in want:
                continue
            data = record(name, rows, variants, cub, run, work)
            path = os.path.join(dest, name + '.kat')
            open(path, 'wb').write(data)
            print(f'{name}: {len(variants)} variants x {len(rows)} rows, {len(data)} bytes')


if __name__ == '__main__':
    main()
