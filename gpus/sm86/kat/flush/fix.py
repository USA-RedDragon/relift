"""The translated denormal corrections, run on a device.

    python3 -I kat/flush/fix.py <absolute work dir> run
    python3 -I kat/flush/fix.py <absolute work dir> prepare
    python3 -I kat/flush/fix.py <absolute work dir> compare <host> <fix>

A program that multiplies and adds two floats per invocation with FMUL.FTZ and FADD.FTZ reads its
operands from two buffers and writes both results. It is translated for the hosts the device
probes can produce (a flushing host with no correction, the Apple and Adreno presets as probed,
and lavapipe, Apple and Adreno with each correction of `HostProfile::flush_fix` forced on with
xlate's --flush-fix) and run on 20,000 rows at the edge of
the normal range (zeros, denormals, the smallest normals, products just below the smallest normal
number, random bits). Every result must equal what SM86 gives: the operands' denormals read as
zero, the exact result rounded to 24 bits with an unbounded exponent, and a result below the
normal range a zero of its sign (any NaN equals any NaN). `run` does that on the devices of
KAT_DEVS (default "llvmpipe") with krun from kat/compute; `prepare` writes each host's module, the
inputs, the spec and the expected words under <work dir>/prepared for a device that is run by hand
(krun on the device, then `compare` with its output files). Needs spirv-val, a C compiler with the
Vulkan headers, and `cargo build --release -p relift-gpu-sm86 --example xlate` in CARGO_TARGET_DIR
(default target/).
"""
import os, struct, subprocess, sys
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '../../../..'))
TARGET = os.environ.get('CARGO_TARGET_DIR', os.path.join(ROOT, 'target'))
XLATE = os.path.join(TARGET, 'release/examples/xlate')
DEVS = os.environ.get('KAT_DEVS', 'llvmpipe').split()
ROWS = 20000
HOSTS = [('nvidia', None), ('moltenvk', None), ('adreno', None)] + [
    (h, f) for h in ('lavapipe', 'moltenvk', 'adreno') for f in range(4)]


def put(w, lo, n, v):
    m = ((1 << n) - 1) << lo
    return (w & ~m) | ((v << lo) & m)


def alu(op, d, a, b):
    """FMUL (0x020) or FADD (0x021) with .FTZ: R<d> = R<a> op R<b>."""
    w = op | (1 << 9) | (7 << 12) | (7 << 110) | (7 << 113)
    for lo, n, v in [(16, 8, d), (24, 8, a), (32, 8, b), (80, 1, 1)] + ([(84, 3, 4)] if op == 0x020 else []):
        w = put(w, lo, n, v)
    return w


def program():
    """S2R, ULDC, LDG and STG words recorded from the NVIDIA driver, the operation words built."""
    w = [int(h, 16) for h in (
        '000e2200000021000000000000057919',  # S2R R5, SR_TID.X
        '000fc60000000a000000080000047ab9',  # ULDC.64 UR4, c[0x0][0x20]
        '000e2400000025000000000000007919',  # S2R R0, SR_CTAID.X
        '001fc800078e02050000004000007824',  # IMAD R0, R0, 0x40, R5
        '000fca00078e00ff0000000400017824',  # IMAD.SHL.U32 R1, R0, 0x4, RZ
        '000ea200081ea9000000000401007981',  # LDG.E.STRONG.SM R0, [R1.U32+UR4]
        '000fe2000810a9040000000001007986',  # STG.E.STRONG.SM [R1.U32+UR4], R0
        '000fea0003800000000000000000794d',  # EXIT
    )]
    s2r_t, uldc, s2r_c, imad, shl, ldg, stg, exit_ = w
    out = [s2r_t, uldc, s2r_c, imad, shl, ldg]
    out.append(put(put(uldc, 16, 8, 6), 38, 10, 0x28))
    out.append(put(put(ldg, 16, 8, 2), 32, 8, 6))
    out.append(alu(0x020, 3, 0, 2))
    out.append(alu(0x021, 4, 0, 2))
    out.append(put(put(uldc, 16, 8, 8), 38, 10, 0x30))
    out.append(put(put(stg, 32, 8, 3), 64, 8, 8))
    out.append(put(put(uldc, 16, 8, 10), 38, 10, 0x38))
    out.append(put(put(stg, 32, 8, 4), 64, 8, 10))
    out.append(exit_)
    return b''.join(x.to_bytes(16, 'little') for x in out)


def xorshift(x):
    x ^= (x << 13) & 0xffffffff
    x ^= x >> 17
    x ^= (x << 5) & 0xffffffff
    return x


def rows():
    """Operand pairs: the fixed edge cases, then random ones at the edge of the normal range."""
    r = [(0x00ffffff, 0x3f000000), (0x3f000000, 0x00ffffff), (0, 0xbf800000), (0x80000000, 0x3f800000),
         (1, 0xbf800000), (0x00800000, 0xbf000000)]
    x = 0x2545f491
    for _ in range(ROWS - len(r)):
        v = []
        for _ in range(4):
            x = xorshift(x)
            v.append(x)
        kind, a, b, r3 = v[0] % 4, v[1], v[2], v[3]
        if kind == 1:
            a = (a & 0x807fffff) | (r3 % 4) << 23
            b = (b & 0x807fffff) | ((r3 >> 4) % 4) << 23
        elif kind >= 2:
            ea = 1 + r3 % 127
            eb = max(0, 128 - ea + (r3 >> 8) % 5 - 2)
            a = (a & 0x807fffff) | ea << 23
            b = (b & 0x807fffff) | eb << 23
            if kind == 3:
                a |= 0x7fffff
                b &= 0xff800000
            if (r3 >> 12) & 1:
                a, b = b, a
        r.append((a, b))
    return r + [(0, 0)] * (-len(r) % 64)


def flt(bits):
    return struct.unpack('<f', struct.pack('<I', bits))[0]


def flush(bits):
    return bits & 0x80000000 if bits & 0x7f800000 == 0 else bits


def r24(v):
    """The Fraction v rounded to 24 bits with an unbounded exponent, to nearest even."""
    if v == 0:
        return v
    a = abs(v)
    e = a.numerator.bit_length() - a.denominator.bit_length()
    if Fraction(2) ** e > a:
        e -= 1
    scale = Fraction(2) ** (e - 23)
    q = a / scale
    n = q.numerator // q.denominator
    rem = q - n
    if rem > Fraction(1, 2) or (rem == Fraction(1, 2) and n % 2):
        n += 1
    return n * scale * (1 if v > 0 else -1)


def expect(op, a, b):
    """SM86's FMUL.FTZ (op 'mul') or FADD.FTZ (op 'add') result bits."""
    a, b = flush(a), flush(b)
    fa, fb = flt(a), flt(b)
    if fa != fa or fb != fb:
        return 0x7fffffff
    sign = ((a ^ b) & 0x80000000) if op == 'mul' else 0
    if fa in (float('inf'), float('-inf')) or fb in (float('inf'), float('-inf')):
        r = fa * fb if op == 'mul' else fa + fb
        return 0x7fffffff if r != r else struct.unpack('<I', struct.pack('<f', r))[0]
    exact = Fraction(fa) * Fraction(fb) if op == 'mul' else Fraction(fa) + Fraction(fb)
    if exact == 0:
        if op == 'mul':
            return sign
        return 0x80000000 if a == 0x80000000 and b == 0x80000000 else 0
    r = r24(exact)
    s = 0x80000000 if r < 0 else 0
    if abs(r) < Fraction(1, 2 ** 126):
        return s
    if abs(r) >= Fraction(2) ** 128:
        return s | 0x7f800000
    return struct.unpack('<I', struct.pack('<f', float(r)))[0]


def same(g, w):
    nan = lambda v: v & 0x7fffffff > 0x7f800000
    return g == w or (nan(g) and nan(w))


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit('%s failed: %s %s' % (os.path.basename(cmd[0]), r.stdout[-2000:], r.stderr[-2000:]))
    return r.stdout


def prepare(work):
    d = os.path.join(work, 'prepared')
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, 'k.bin'), 'wb').write(program())
    rs = rows()
    n = len(rs)
    open(os.path.join(d, 'a.bin'), 'wb').write(struct.pack('<%dI' % n, *[a for a, _ in rs]))
    open(os.path.join(d, 'b.bin'), 'wb').write(struct.pack('<%dI' % n, *[b for _, b in rs]))
    open(os.path.join(d, 'want.bin'), 'wb').write(struct.pack(
        '<%dI' % (2 * n), *([expect('mul', a, b) for a, b in rs] + [expect('add', a, b) for a, b in rs])))
    spec = ['local 64 1 1', 'groups %d 1 1' % (n // 64), 'buf a.bin', 'buf b.bin', 'out m.out %d' % (4 * n),
            'out s.out %d' % (4 * n), 'put 0 0x20 addr 0', 'put 0 0x28 addr 1', 'put 0 0x30 addr 2',
            'put 0 0x38 addr 3', 'push 0x0 addr 0', 'push 0x8 addr 1', 'push 0x10 addr 2', 'push 0x18 addr 3']
    for host, fix in HOSTS:
        name = '%s%s' % (host, '' if fix is None else '-fix%d' % fix)
        spv = os.path.join(d, name + '.spv')
        args = [XLATE, os.path.join(d, 'k.bin'), spv, host] + ([] if fix is None else ['--flush-fix', str(fix)])
        x = run(args)
        if not x.startswith('ok'):
            sys.exit('%s: %s' % (name, x))
        run(['spirv-val', '--target-env', 'vulkan1.2', spv])
        open(os.path.join(d, name + '.spec'), 'w').write('spv %s.spv\n' % name + '\n'.join(spec) + '\n')
    return d, n


def compare(d, n, name, m_out, s_out):
    want = struct.unpack('<%dI' % (2 * n), open(os.path.join(d, 'want.bin'), 'rb').read())
    ms = struct.unpack('<%dI' % n, open(m_out, 'rb').read()[:4 * n])
    ss = struct.unpack('<%dI' % n, open(s_out, 'rb').read()[:4 * n])
    rs = rows()
    bad = 0
    for i in range(n):
        for k, g in (('mul', ms[i]), ('add', ss[i])):
            w = want[i] if k == 'mul' else want[n + i]
            if not same(g, w):
                bad += 1
                if bad <= 5:
                    print('  %s %s: %08x %08x got %08x want %08x' % (name, k, rs[i][0], rs[i][1], g, w))
    print('%-16s %s' % (name, 'exact on %d rows' % n if not bad else '%d results differ' % bad))
    return not bad


def main():
    work, mode = sys.argv[1], sys.argv[2]
    if mode == 'compare':
        d = os.path.join(work, 'prepared')
        n = ROWS + (-ROWS % 64)
        ok = compare(d, n, sys.argv[3], os.path.join(work, 'm.out'), os.path.join(work, 's.out'))
        sys.exit(0 if ok else 1)
    d, n = prepare(work)
    if mode == 'prepare':
        return
    krun = os.path.join(work, 'krun')
    subprocess.run(['cc', '-O1'] + ['-I' + i for i in os.environ.get('KAT_VK_INCLUDE', '').split()] +
                   ['-o', krun, os.path.join(HERE, '../compute/krun.c'), '-lvulkan'], check=True)
    ok = True
    for dev in DEVS:
        for host, fix in HOSTS:
            name = '%s%s' % (host, '' if fix is None else '-fix%d' % fix)
            r = subprocess.run([krun, name + '.spec', dev], capture_output=True, text=True, cwd=d, timeout=300)
            if r.returncode == 77:
                print('%-16s %-8s skipped: %s' % (name, dev, r.stderr.strip().splitlines()[-1]))
                continue
            if r.returncode:
                print('%-16s %-8s run failed: %s' % (name, dev, r.stderr.strip()[-800:]))
                ok = False
                continue
            print('%-8s ' % dev, end='')
            ok &= compare(d, n, name, os.path.join(d, 'm.out'), os.path.join(d, 's.out'))
    print('PASS' if ok else 'FAIL')
    sys.exit(0 if ok else 1)


main()
