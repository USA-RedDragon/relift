"""Known answers for task and mesh programs, from the NVIDIA driver.

    python3 -I kat/mesh/kat.py <absolute work dir> [--record <kat file>] [case ...]

For each case under cases/ (GLSL task, mesh and fragment shaders and case.json): compile the GLSL,
draw it on the NVIDIA device (the reference image), carve the SASS the driver built from its pipeline
cache with relift-shaders, translate the task and mesh SASS for both back ends and the fragment SASS
for each, validate everything with spirv-val, draw the native translation and the compute back end
(task pass, scan, mesh pass, scan, indirect draw) and compare both images with the reference, word for
word. --record writes the SASS, the pipeline and the reference image's digest to the kat file
tests/mesh.rs reads. Needs glslangValidator, spirv-val, zstd, a C compiler with the Vulkan headers, and
`cargo build --release -p relift-switch-shaders -p relift-gpu-sm86 --example mesh` in CARGO_TARGET_DIR
(default target/). KAT_DEV=<device name part> and KAT_HOST=<preset> draw the translations on another
device; its rasterizer may differ from NVIDIA's, so compare the two back ends with each other there.

    python3 -I kat/mesh/kat.py <absolute work dir> --replay tests/kat/mesh.txt [case ...]

with KAT_DEV and KAT_HOST translates the recorded programs instead (no NVIDIA device, glslangValidator
or zstd needed) and requires the two back ends to draw the same image on that device, word for word.
"""
import array, hashlib, json, os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '../../../..'))
TARGET = os.environ.get('CARGO_TARGET_DIR', os.path.join(ROOT, 'target'))
X = os.path.join(TARGET, 'release/examples/mesh')
SHADERS = os.path.join(TARGET, 'release/relift-shaders')
TOPO = {'point': 1, 'line': 2, 'tri': 3}


def run(args, **kw):
    r = subprocess.run(args, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit('%s failed: %s %s' % (os.path.basename(args[0]), r.stdout[-2000:], r.stderr[-2000:]))
    return r.stdout


def validate(spv):
    r = subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', spv], capture_output=True, text=True)
    if r.returncode:
        sys.exit('spirv-val %s: %s' % (spv, r.stdout + r.stderr))


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
    return bytes(blob)


def one(work, name, record):
    d = os.path.join(HERE, 'cases', name)
    case = json.load(open(os.path.join(d, 'case.json')))
    o = os.path.join(work, name)
    subprocess.run(['rm', '-rf', o])
    os.makedirs(os.path.join(o, 'tree/romfs/Lib'))
    task = os.path.exists(os.path.join(d, 't.task'))
    for s in (['t.task'] if task else []) + ['m.mesh', 'f.frag']:
        run(['glslangValidator', '-V', '--target-env', 'vulkan1.3', '-o', os.path.join(o, s[0] + '.spv'), os.path.join(d, s)])
    ext = case.get('ext', False)
    env = dict(os.environ)
    if not ext:
        env['MRUN_NV'] = '1'
    count, first = case['draw']
    blend = 'on' if case.get('blend') else 'off'
    t_ref = os.path.join(o, 't.spv') if task else '-'
    common = 'buf 0 65536\nbind 1 0 0 ubo\n'
    grid = '%d %d 1' % (count, first) if not ext else '%d 1 1' % count
    open(os.path.join(o, 'ref.s'), 'w').write(common + 'blend %s\nmesh %s %s/m.spv %s/f.spv %s\nout %s/ref.raw\ncache %s/c.bin\n' % (
        blend, t_ref, o, o, grid, o, o))
    mrun = os.path.join(work, 'mrun')
    run([mrun, os.path.join(o, 'ref.s')], env=env)
    blob = frames(os.path.join(o, 'c.bin'), os.path.join(o, 'tree/romfs/Lib/c.bin'))
    run([SHADERS, 'scan', os.path.join(o, 'tree'), '--out', os.path.join(o, 'scan'), '-j', '2'])
    progs = {}
    for sha in json.load(open(os.path.join(o, 'scan/manifest.json'))):
        path = os.path.join(o, 'scan', sha + '.bin')
        w = struct.unpack('<32I', open(path, 'rb').read(128))
        if w[4] == 0xff000000:
            progs['task' if (w[0] >> 10) & 0xf == 1 and task else 'mesh'] = path
        elif (w[0] >> 10) & 0xf == 5:
            progs['frag'] = path
    if sorted(progs) != sorted((['task'] if task else []) + ['mesh', 'frag']):
        sys.exit('%s: the carve found %s' % (name, sorted(progs)))
    prim_sph = None
    for k in range(0, len(blob) - 128, 4):
        if struct.unpack_from('<I', blob, k)[0] & 0x01003fff == 0x01001081:
            prim_sph = blob[k:k + 128]
            break
    ref = open(os.path.join(o, 'ref.raw'), 'rb').read()
    nat, fb = draw(work, o, name, case, progs, prim_sph, tolerate_native=True)
    if not nat:
        nat = bytes(len(ref))
    words = [array.array('I', x) for x in (ref, nat, fb)]
    lit = lit_pixels(words[0])
    diff = lambda a, b: sum(1 for x, y in zip(a, b) if x != y)
    dn, df, dnf = diff(words[0], words[1]), diff(words[0], words[2]), diff(words[1], words[2])
    print('%s: %d pixels lit; native %d, compute %d words differ from the reference, %d from each other' % (name, lit, dn, df, dnf))
    if record is not None:
        if dn or df or lit == 0:
            sys.exit('%s: not recorded: the translations differ from the reference' % name)
        p = case['pipeline']
        record.append('case %s' % name)
        record.append('pipeline ' + ' '.join('%s=%s' % kv for kv in sorted(p.items())) +
                      ''.join(' %s=%d' % (k, case[k]) for k in ('primitive_id_location', 'fragment_per_primitive') if k in case) +
                      (' primitive_sph=' + prim_sph.hex() if prim_sph else ''))
        for k in ('task', 'mesh', 'frag'):
            if k in progs:
                record.append('program %s %s' % (k, open(progs[k], 'rb').read().hex()))
        record.append('answer lit=%d sha256=%s' % (lit, hashlib.sha256(ref).hexdigest()))
    for f in ('c.bin', 'ref.raw', 'cb0.bin'):
        os.remove(os.path.join(o, f))
    subprocess.run(['rm', '-rf', os.path.join(o, 'tree')])
    return dn == 0 and df == 0 and lit > 0


def lit_pixels(words):
    return sum(1 for i in range(0, len(words), 4) if any(words[i:i + 4]))


def draw(work, o, name, case, progs, prim_sph, tolerate_native):
    """Translates the programs for KAT_HOST, draws the native translation and the compute back end on
    KAT_DEV, and returns both images (the native one empty when its draw failed and tolerate_native)."""
    task = 'task' in progs
    count, first = case['draw']
    blend = 'on' if case.get('blend') else 'off'
    mrun = os.path.join(work, 'mrun')
    p = case['pipeline']
    pipe = ['--verts', str(p['max_vertices']), '--prims', str(p['max_primitives']), '--topo', p['topology'],
            '--task-lanes', str(p['task_lanes']), '--mesh-lanes', str(p['mesh_lanes']), '--payload', str(p['payload_bytes'])]
    if prim_sph:
        open(os.path.join(o, 'prim.sph'), 'wb').write(prim_sph)
        pipe += ['--prim-sph', os.path.join(o, 'prim.sph')]
    native = ['--frag-per-prim', str(case['fragment_per_primitive'])] if 'fragment_per_primitive' in case else []
    compute = ['--prim-id-loc', str(case['primitive_id_location'])] if 'primitive_id_location' in case else []
    host = os.environ.get('KAT_HOST', 'nvidia')
    for k, path in progs.items():
        for back, extra in (('native', native), ('compute', compute + ['--compute'])):
            spv = os.path.join(o, '%s-%s.spv' % (k, back))
            r = run([X, path, spv, host] + pipe + extra)
            if not r.startswith('ok') or 'inexact []' not in r:
                sys.exit('%s %s %s: %s' % (name, k, back, r))
            validate(spv)
    r = run([X, 'helpers', progs['mesh'], o] + pipe + compute).split()
    for h in ('scan-tasks', 'scan-meshes', 'vertex'):
        validate(os.path.join(o, h + '.spv'))
    record_words = int(r[3])
    cb = bytearray(65536)
    struct.pack_into('<I', cb, 0x1f4, first)
    struct.pack_into('<3I', cb, 0x20, count, 1, 1)
    open(os.path.join(o, 'cb0.bin'), 'wb').write(cb)
    common = 'bufi 0 %s/cb0.bin\nbind 1 0 0 ubo\n' % o
    tenv = {k: v for k, v in os.environ.items() if k != 'MRUN_NV'}
    if os.environ.get('KAT_DEV'):
        tenv['MRUN_DEV'] = os.environ['KAT_DEV']
    t_nat = os.path.join(o, 'task-native.spv') if task else '-'
    open(os.path.join(o, 'nat.s'), 'w').write(common + 'blend %s\nmesh %s %s/mesh-native.spv %s/frag-native.spv %d 1 1\nout %s/nat.raw\n' % (
        blend, t_nat, o, o, count, o))
    r = subprocess.run([mrun, os.path.join(o, 'nat.s')], env=tenv, capture_output=True, text=True)
    if r.returncode:
        if not tolerate_native or not os.environ.get('KAT_DEV'):
            sys.exit('%s: native draw failed: %s' % (name, r.stderr[-500:]))
        print('%s: native draw failed on %s: %s' % (name, os.environ['KAT_DEV'], r.stderr.strip()[-200:]))
        open(os.path.join(o, 'nat.raw'), 'wb').write(b'')
    cap = 4096
    hdr = [0] * 32
    hdr[0], hdr[12], hdr[13] = count, cap, 32
    hdr[14] = 32 + count * max(p['payload_bytes'] // 4, 2)
    hdr[15] = hdr[14] + count + 1
    hdr[16] = hdr[15] + cap * record_words
    if not task:
        hdr[1] = count
        hdr[4:7] = [min(count, 65535), (count + 65534) // 65535, 1]
    open(os.path.join(o, 'fb.bin'), 'wb').write(struct.pack('<32I', *hdr))
    s = common + 'bufi 1 %s/fb.bin %d\nbind 3 2 1 ssbo\n' % (o, (hdr[16] + cap + 1) * 4)
    if task:
        s += 'comp %s/task-compute.spv %d 1 1\ncomp %s/scan-tasks.spv 1 1 1\n' % (o, count, o)
    s += 'compi %s/mesh-compute.spv 1 16\ncomp %s/scan-meshes.spv 1 1 1\n' % (o, o)
    s += 'blend %s\ndrawi %s/vertex.spv %s/frag-compute.spv %s 1 32\nout %s/fb.raw\n' % (blend, o, o, p['topology'], o)
    open(os.path.join(o, 'fb.s'), 'w').write(s)
    run([mrun, os.path.join(o, 'fb.s')], env=tenv)
    nat, fb = (open(os.path.join(o, f), 'rb').read() for f in ('nat.raw', 'fb.raw'))
    for f in ('nat.raw', 'fb.raw'):
        os.remove(os.path.join(o, f))
    return nat, fb


def replay(work, name, kat):
    """Draws a recorded case's programs on KAT_DEV: both back ends must give the same image."""
    case = json.load(open(os.path.join(HERE, 'cases', name, 'case.json')))
    o = os.path.join(work, name)
    subprocess.run(['rm', '-rf', o])
    os.makedirs(o)
    progs, prim_sph = {}, None
    for k, h in kat[name]['programs'].items():
        progs[k] = os.path.join(o, k + '.bin')
        open(progs[k], 'wb').write(bytes.fromhex(h))
    if kat[name].get('primitive_sph'):
        prim_sph = bytes.fromhex(kat[name]['primitive_sph'])
    nat, fb = draw(work, o, name, case, progs, prim_sph, tolerate_native=False)
    words = [array.array('I', x) for x in (nat, fb)]
    lit = lit_pixels(words[0])
    d = sum(1 for x, y in zip(*words) if x != y)
    print('%s: %d pixels lit; the back ends differ in %d words' % (name, lit, d))
    return d == 0 and lit > 0 and len(nat) == len(fb)


def read_kat(path):
    out, cur = {}, None
    for line in open(path):
        kind, rest = line.rstrip('\n').split(' ', 1)
        if kind == 'case':
            cur = out.setdefault(rest, {'programs': {}})
        elif kind == 'program':
            k, h = rest.split(' ')
            cur['programs'][k] = h
        elif kind == 'pipeline':
            for kv in rest.split(' '):
                k, v = kv.split('=')
                if k == 'primitive_sph':
                    cur['primitive_sph'] = v
    return out


def main():
    args = sys.argv[1:]
    work = args.pop(0)
    record_to = None
    if '--record' in args:
        k = args.index('--record')
        record_to = args[k + 1]
        del args[k:k + 2]
    replay_from = None
    if '--replay' in args:
        k = args.index('--replay')
        replay_from = args[k + 1]
        del args[k:k + 2]
    if os.environ.get('KAT_DEV') and not os.environ.get('KAT_HOST'):
        sys.exit('KAT_DEV is set without KAT_HOST: name the host preset the translations are for')
    if replay_from and not os.environ.get('KAT_DEV'):
        sys.exit('--replay needs KAT_DEV and KAT_HOST')
    os.makedirs(work, exist_ok=True)
    subprocess.run(['cc', '-O1', '-o', os.path.join(work, 'mrun'), os.path.join(HERE, 'mrun.c'), '-lvulkan'], check=True)
    names = args or sorted(os.listdir(os.path.join(HERE, 'cases')))
    record = [] if record_to else None
    if replay_from:
        kat = read_kat(replay_from)
        if sorted(kat) != sorted(names) and not args:
            sys.exit('%s has %s, the cases are %s' % (replay_from, sorted(kat), names))
        ok = all([replay(work, n, kat) for n in names])
    else:
        ok = all([one(work, n, record) for n in names])
    if record_to:
        open(record_to, 'w').write('\n'.join(record) + '\n')
    sys.exit(0 if ok else 1)


main()
