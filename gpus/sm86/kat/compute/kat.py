"""Known answers for compute programs, from the NVIDIA driver.

    python3 -I kat/compute/kat.py <absolute work dir> [--record <kat file>] [case ...]

For each case under cases/ (k.comp, GLSL, and spec, a krun spec): compile the GLSL, run it on the
NVIDIA device (KAT_REF, default "NVIDIA") for the reference outputs, carve the SASS the driver built
from its pipeline cache with relift-shaders, translate it for every host preset in KAT_HOSTS,
validate each translation with spirv-val, run it on every device in KAT_DEVS (default "llvmpipe
NVIDIA") and compare every output with the reference, word for word. --record writes the SASS,
the inputs and the reference outputs to the kat file. Needs glslangValidator, spirv-val, zstd, a C
compiler with the Vulkan headers, and `cargo build --release -p relift-switch-shaders -p
relift-gpu-sm86 --example xlate` in CARGO_TARGET_DIR (default target/).

    python3 -I kat/compute/kat.py <absolute work dir> --replay tests/kat/compute.txt [case ...]

translates the recorded programs instead and compares their outputs with the recorded ones on the
KAT_DEVS devices; it needs no NVIDIA device, glslangValidator or zstd.

A spec names its files with placeholders: SPV (the module), OUT (the prefix of the output files)
and IN<n> (input file n). `input <n> <bytes> <kind> <seed>` lines make input n: kind u32 for
random words, f32:<lo>:<hi> for floats drawn between lo and hi, words:<a>,<b>,... for those words
then zeros, zero for zeros. The translated
program finds its buffer addresses in constant bank 0, so a spec places them there with `put`
lines as well as in the push constants the GLSL reads. A run that needs VK_KHR_shader_fma or
VK_KHR_cooperative_matrix on a device without it is skipped, and so is a translation that runs warp
operations on 32-wide host subgroups (or 32-lane clusters of them) anywhere but on the reference
device. A translation the translator reports inexact may differ; any other difference fails.
"""
import os, random, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '../../../..'))
TARGET = os.environ.get('CARGO_TARGET_DIR', os.path.join(ROOT, 'target'))
XLATE = os.path.join(TARGET, 'release/examples/xlate')
DIS = os.path.join(TARGET, 'release/examples/dis')
SHADERS = os.path.join(TARGET, 'release/relift-shaders')
HOSTS = os.environ.get('KAT_HOSTS', 'lavapipe mali amd_pro anv nvidia adreno').split()
DEVS = os.environ.get('KAT_DEVS', 'llvmpipe NVIDIA').split()
REF = os.environ.get('KAT_REF', 'NVIDIA')


def run(args, **kw):
    r = subprocess.run(args, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit('%s failed: %s %s' % (os.path.basename(args[0]), r.stdout[-2000:], r.stderr[-2000:]))
    return r.stdout


def make_input(n, kind, seed):
    rng = random.Random(seed)
    if kind == 'u32':
        return struct.pack('<%dI' % (n // 4), *[rng.getrandbits(32) for _ in range(n // 4)])
    if kind.startswith('f32:'):
        lo, hi = (float(x) for x in kind.split(':')[1:])
        return struct.pack('<%df' % (n // 4), *[rng.uniform(lo, hi) for _ in range(n // 4)])
    if kind == 'zero':
        return bytes(n)
    if kind.startswith('words:'):
        w = [int(x, 0) for x in kind[6:].split(',')]
        return struct.pack('<%dI' % (n // 4), *(w + [0] * (n // 4 - len(w))))
    sys.exit('unknown input kind %s' % kind)


def parse_spec(name):
    """The spec's krun lines and its inputs as {n: bytes}."""
    lines, inputs = [], {}
    for line in open(os.path.join(HERE, 'cases', name, 'spec')):
        w = line.split()
        if w and w[0] == 'input':
            inputs[int(w[1])] = make_input(int(w[2]), w[3], int(w[4]))
        elif w:
            lines.append(line.rstrip('\n'))
    return lines, inputs


def outputs(lines):
    return sorted({t[3:] for line in lines for t in line.split() if t.startswith('OUT')})


def spec_for(lines, o, spv, out, inputs):
    """Writes the inputs and the spec with its placeholders filled; gives the spec's path."""
    s = []
    for line in lines:
        w = []
        for t in line.split():
            if t == 'SPV':
                t = spv
            elif t.startswith('OUT'):
                t = out + t[3:]
            elif t.startswith('IN') and t[2:].isdigit():
                t = os.path.join(o, 'in%s.bin' % t[2:])
            w.append(t)
        s.append(' '.join(w))
    for k, b in inputs.items():
        open(os.path.join(o, 'in%d.bin' % k), 'wb').write(b)
    path = os.path.join(o, 'spec')
    open(path, 'w').write('\n'.join(s) + '\n')
    return path


def frames(cache, out):
    """Every zstd frame of the NVIDIA pipeline cache, decompressed, behind a GLSLC magic for the carve."""
    b = open(cache, 'rb').read()
    blob = bytearray(16) + bytes([0x34, 0x12, 0x76, 0x98]) + bytearray(12)
    i = b.find(b'\x28\xb5\x2f\xfd')
    while i >= 0:
        r = subprocess.run(['zstd', '-dcq'], input=b[i:], capture_output=True)
        blob += r.stdout + bytearray(256)
        i = b.find(b'\x28\xb5\x2f\xfd', i + 4)
    open(out, 'wb').write(blob)


def carve(o, cache):
    """The compute program the driver left in its pipeline cache, without the carve's header."""
    subprocess.run(['rm', '-rf', os.path.join(o, 'tree'), os.path.join(o, 'scan')])
    os.makedirs(os.path.join(o, 'tree/romfs/Lib'))
    frames(cache, os.path.join(o, 'tree/romfs/Lib/c.bin'))
    run([SHADERS, 'scan', os.path.join(o, 'tree'), '--out', os.path.join(o, 'scan'), '-j', '2'])
    found = [f for f in os.listdir(os.path.join(o, 'scan')) if f.endswith('.bin')]
    if len(found) != 1:
        sys.exit('%s: the carve found %s' % (o, found))
    b = open(os.path.join(o, 'scan', found[0]), 'rb').read()
    subprocess.run(['rm', '-rf', os.path.join(o, 'tree'), os.path.join(o, 'scan')])
    open(os.path.join(o, 'k.bin'), 'wb').write(b)
    skip = 0
    for line in run([DIS, os.path.join(o, 'k.bin')]).splitlines():
        if line.startswith('/*') and '*/ ??' in line:
            skip = int(line[2:line.index('*/')], 16) + 16
    return b[skip:]


def check(work, name, prog, lines, inputs, answers):
    """Translates prog for every host, validates it and runs it on every device; True if every
    output equals its answer."""
    o = os.path.join(work, name)
    os.makedirs(o, exist_ok=True)
    open(os.path.join(o, 'k.bin'), 'wb').write(prog)
    krun = os.path.join(work, 'krun')
    ok = True
    for host in HOSTS:
        spv = os.path.join(o, host + '.spv')
        x = run([XLATE, os.path.join(o, 'k.bin'), spv, host])
        if not x.startswith('ok'):
            print('%s %s: %s' % (name, host, x.strip()))
            ok = False
            continue
        inexact = x.split('inexact: [', 1)[1].split(']', 1)[0] if 'inexact: [' in x else ''
        native = ('warp: Subgroup' in x or 'check_warp32: true' in x) and 'warp_ops: 0,' not in x
        v = subprocess.run(['spirv-val', '--target-env', 'vulkan1.2', spv], capture_output=True, text=True)
        if v.returncode:
            print('%s %s: spirv-val: %s' % (name, host, (v.stdout + v.stderr).strip()[-1500:]))
            ok = False
            continue
        for dev in DEVS:
            if native and dev != REF:
                print('%-10s %-8s %-8s skipped: warps on 32-wide subgroups' % (name, host, dev))
                continue
            out = os.path.join(o, 'out')
            spec = spec_for(lines, o, spv, out, inputs)
            r = subprocess.run([krun, spec, dev], capture_output=True, text=True, timeout=120)
            if r.returncode == 77:
                print('%-10s %-8s %-8s skipped: %s' % (name, host, dev, r.stderr.strip().splitlines()[-1]))
                continue
            if r.returncode:
                print('%-10s %-8s %-8s run failed: %s' % (name, host, dev, r.stderr.strip()[-800:]))
                ok = False
                continue
            msgs = []
            for k, ref in answers.items():
                got = open(out + k, 'rb').read()[:len(ref)]
                os.remove(out + k)
                n = len(ref) // 4
                a, g = struct.unpack('<%dI' % n, ref), struct.unpack('<%dI' % n, got)
                diff = [i for i in range(n) if a[i] != g[i]]
                if diff:
                    ok &= bool(inexact)
                    msgs.append('%s: %d of %d words differ, first %d: %08x, not %08x' % (
                        k or 'out', len(diff), n, diff[0], g[diff[0]], a[diff[0]]))
            print('%-10s %-8s %-8s %s%s' % (name, host, dev, '; '.join(msgs) or 'exact',
                                            ' (reported inexact: %s)' % inexact if inexact else ''))
    return ok


def record_one(work, name, record):
    lines, inputs = parse_spec(name)
    o = os.path.join(work, name)
    os.makedirs(o, exist_ok=True)
    ref_spv = os.path.join(o, 'ref.spv')
    run(['glslangValidator', '-V', '--target-env', 'vulkan1.3', '-o', ref_spv, os.path.join(HERE, 'cases', name, 'k.comp')])
    ref = os.path.join(o, 'ref')
    spec = spec_for(lines, o, ref_spv, ref, inputs)
    cache = os.path.join(o, 'cache.bin')
    run([os.path.join(work, 'krun'), spec, REF], env=dict(os.environ, KRUN_CACHE=cache))
    answers = {k: open(ref + k, 'rb').read() for k in outputs(lines)}
    prog = carve(o, cache)
    os.remove(cache)
    ok = check(work, name, prog, lines, inputs, answers)
    if record is not None:
        record.append('case %s' % name)
        record.append('program %s' % prog.hex())
        for k, b in sorted(inputs.items()):
            record.append('input %d %s' % (k, b.hex()))
        for k, b in sorted(answers.items()):
            record.append('answer %s %s' % (k or '-', b.hex()))
    return ok


def read_kat(path):
    out, cur = {}, None
    for line in open(path):
        kind, rest = line.rstrip('\n').split(' ', 1)
        if kind == 'case':
            cur = out.setdefault(rest, {'inputs': {}, 'answers': {}})
        elif kind == 'program':
            cur['program'] = bytes.fromhex(rest)
        elif kind == 'input':
            k, h = rest.split(' ')
            cur['inputs'][int(k)] = bytes.fromhex(h)
        elif kind == 'answer':
            k, h = rest.split(' ')
            cur['answers']['' if k == '-' else k] = bytes.fromhex(h)
    return out


def main():
    args = sys.argv[1:]
    work = args.pop(0)
    record_to = replay_from = None
    for flag in ('--record', '--replay'):
        if flag in args:
            k = args.index(flag)
            if flag == '--record':
                record_to = args[k + 1]
            else:
                replay_from = args[k + 1]
            del args[k:k + 2]
    os.makedirs(work, exist_ok=True)
    subprocess.run(['cc', '-O1'] + ['-I' + i for i in os.environ.get('KAT_VK_INCLUDE', '').split()] + ['-o', os.path.join(work, 'krun'), os.path.join(HERE, 'krun.c'), '-lvulkan'], check=True)
    names = args or sorted(os.listdir(os.path.join(HERE, 'cases')))
    if replay_from:
        kat = read_kat(replay_from)
        if not args and sorted(kat) != names:
            sys.exit('%s has %s, the cases are %s' % (replay_from, sorted(kat), names))
        ok = True
        for n in names:
            lines, _ = parse_spec(n)
            ok &= check(work, n, kat[n]['program'], lines, kat[n]['inputs'], kat[n]['answers'])
    else:
        record = [] if record_to else None
        ok = all([record_one(work, n, record) for n in names])
        if record_to:
            if not ok:
                sys.exit('not recorded: a translation differs from the reference')
            open(record_to, 'w').write('\n'.join(record) + '\n')
    print('PASS' if ok else 'FAIL')
    sys.exit(0 if ok else 1)


main()
