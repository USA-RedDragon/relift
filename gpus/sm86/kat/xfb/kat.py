"""Known answers for transform feedback, from the NVIDIA driver.

    python3 -I kat/xfb/kat.py <absolute work dir> [case ...]

For each case under cases/ (a GLSL capture program c.vert, a geometry program c.geom when there is one, and
case.json: the stride, the point count and the layout select bytes the GLSL's xfb decorations give, as the NVN2
driver writes them for a GLSLC header in raw form): compile the GLSL, capture it on the NVIDIA device and draw what it
captured (xrun.c, the reference), carve the SASS the driver built from its pipeline cache with relift-shaders,
translate it with the case's layout, validate it with spirv-val, capture and draw the translation the same way, and
compare the captured bytes, the byte count and the drawn image with the reference, word for word. Then the same
for the memory path (a host without VK_EXT_transform_feedback): the program translated for it writes its records on
the NVIDIA device, and the records are packed into the buffer as the runtime does (nvn_xfb.c, nvn_xfb_draw_finish);
the packed bytes and the count must be the reference's too.

Needs glslangValidator, spirv-val, zstd, a C compiler with the Vulkan headers, and
`cargo build --release -p relift-switch-shaders -p relift-gpu-sm86 --example xlate` in CARGO_TARGET_DIR (default
target/).     python3 -I kat/xfb/kat.py <absolute work dir> --record tests/kat/xfb.txt [case ...]

also writes, for each case that passes, its programs and the NVIDIA capture (bytes and count) to the record.

    python3 -I kat/xfb/kat.py <absolute work dir> --replay tests/kat/xfb.txt [--export <dir>]

with KAT_DEV and KAT_HOST translates the recorded programs for the host (no NVIDIA device, glslangValidator or zstd
needed), validates them, captures on KAT_DEV through the path the translation takes (KAT_XFB_MEMORY=1 takes the
memory path on a host that has transform feedback too) (the host's transform feedback,
or the memory path packed as nvn_xfb_pack packs it) and requires the recorded NVIDIA bytes and count word for word.
With --export it only translates, into <dir> with run.json, for a device without the build (the Mac):

    python3 -I kat.py <absolute work dir> --run <dir>      (beside xrun.c, draw.vert.spv and draw.frag.spv in <dir>)

KAT_DEV=<device name part> with KAT_HOST=<preset> runs the translation, made for that host preset, on another device;
the reference stays NVIDIA's unless KAT_REF_DEV names another (CI: KAT_DEV=llvmpipe KAT_HOST=lavapipe
KAT_REF_DEV=llvmpipe, the capture of the GLSL on lavapipe as the reference).

    python3 -I kat/xfb/kat.py <absolute work dir> --game <module or file>

reads every capturing program of a game's file (a GLSLC header with a transform feedback section,
runtime/tools/nvn2/glslc.py), translates it with the layout the driver builds from the header, validates it with
spirv-val and checks that the translation captures the buffers and strides the header names. The game's own bytes
stay in its import; no answer of the console's is recorded, so this checks the translation, not the values.
"""
import json, os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '../../../..'))
TARGET = os.environ.get('CARGO_TARGET_DIR', os.path.join(ROOT, 'target'))
XLATE = os.path.join(TARGET, 'release/examples/xlate')
SHADERS = os.path.join(TARGET, 'release/relift-shaders')


def run(args, **kw):
    r = subprocess.run(args, capture_output=True, text=True, **kw)
    if r.returncode:
        how = 'signal %d' % -r.returncode if r.returncode < 0 else 'exit status %d' % r.returncode
        sys.exit('%s failed (%s): %s\n%s %s' % (os.path.basename(args[0]), how, ' '.join(args), r.stdout[-2000:],
                                               r.stderr[-2000:]))
    return r.stdout


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


def gs_flag(case):
    """xlate's flag for the case's geometry input primitive (default points); none for triangles, its default."""
    i = case.get('input', 'points')
    return [] if i == 'triangles' else ['--gs-input', i]


def topo_env(env, case):
    """The draw's primitive for xrun: the geometry input of the case."""
    return dict(env or os.environ, XRUN_TOPO=case.get('input', 'points'))


def capture(work, o, sub, vs, gs, case, env=None):
    d = os.path.join(o, sub)
    os.makedirs(d, exist_ok=True)
    run([os.path.join(work, 'xrun'), vs, gs or '-', str(case['stride']), str(case['count']),
         os.path.join(work, 'draw.vert.spv'), os.path.join(work, 'draw.frag.spv'), d], env=topo_env(env, case))
    return [open(os.path.join(d, f), 'rb').read() for f in ('capture.bin', 'count.txt', 'image.raw')]


def one(work, name):
    d = os.path.join(HERE, 'cases', name)
    case = json.load(open(os.path.join(d, 'case.json')))
    o = os.path.join(work, name)
    subprocess.run(['rm', '-rf', o])
    os.makedirs(os.path.join(o, 'tree/romfs/Lib'))
    stages = [s for s in ('c.vert', 'c.geom') if os.path.exists(os.path.join(d, s))]
    for s in stages:
        run(['glslangValidator', '-V', '--target-env', 'vulkan1.3', '-o', os.path.join(o, s + '.spv'), os.path.join(d, s)])
    gs = os.path.join(o, 'c.geom.spv') if 'c.geom' in stages else None
    env = dict(os.environ, XRUN_CACHE=os.path.join(o, 'c.bin'))
    if os.environ.get('KAT_REF_DEV'):
        env['XRUN_DEV'] = os.environ['KAT_REF_DEV']
    ref = capture(work, o, 'ref', os.path.join(o, 'c.vert.spv'), gs, case, env)
    frames(os.path.join(o, 'c.bin'), os.path.join(o, 'tree/romfs/Lib/c.bin'))
    run([SHADERS, 'scan', os.path.join(o, 'tree'), '--out', os.path.join(o, 'scan'), '-j', '2'])
    progs = {}
    for sha in json.load(open(os.path.join(o, 'scan/manifest.json'))):
        path = os.path.join(o, 'scan', sha + '.bin')
        stage = (struct.unpack('<I', open(path, 'rb').read(4))[0] >> 10) & 0xf
        progs.setdefault({1: 'vert', 4: 'geom'}.get(stage, 'other'), []).append(path)
    want = ['vert'] + (['geom'] if gs else [])
    if any(len(progs.get(k, [])) != 1 for k in want):
        sys.exit('%s: the carve found %s' % (name, {k: len(v) for k, v in progs.items()}))
    sel = '%d:%s' % (case['stride'], ','.join(str(b) for b in case['select']))
    host = os.environ.get('KAT_HOST', 'nvidia')
    spv = {}
    for k in want:
        spv[k] = os.path.join(o, k + '.spv')
        flags = (['--xfb', sel] if k == want[-1] else []) + (gs_flag(case) if k == 'geom' else [])
        out = run([XLATE, progs[k][0], spv[k], host] + flags)
        if not out.startswith('ok'):
            sys.exit('%s: the %s program does not translate: %s' % (name, k, out))
        r = subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', spv[k]], capture_output=True, text=True)
        if r.returncode:
            sys.exit('%s: spirv-val %s: %s' % (name, k, r.stdout + r.stderr))
    tenv = dict(os.environ)
    if os.environ.get('KAT_DEV'):
        tenv['XRUN_DEV'] = os.environ['KAT_DEV']
    got = capture(work, o, 'got', spv['vert'], spv.get('geom'), case, tenv)
    words = lambda b: struct.unpack('<%dI' % (len(b) // 4), b)
    dc = sum(1 for x, y in zip(words(ref[0]), words(got[0])) if x != y)
    di = sum(1 for x, y in zip(words(ref[2]), words(got[2])) if x != y)
    lit = sum(1 for i in range(0, len(ref[2]), 16) if any(ref[2][i:i + 16]))
    same_count = ref[1] == got[1]
    print('%s: %s bytes captured (reference %s); %d capture words and %d image words differ; %d pixels lit' % (
        name, got[1].decode().strip(), ref[1].decode().strip(), dc, di, lit))
    mem_ok = memory(work, o, name, case, progs, want, sel, host, tenv, ref)
    ok = dc == 0 and di == 0 and same_count and lit > 0 and mem_ok
    if ok and RECORD is not None:
        RECORD.append('case %s %s' % (name, json.dumps(case, separators=(',', ':'))))
        for k in want:
            RECORD.append('program %s %s' % (k, open(progs[k][0], 'rb').read().hex()))
        RECORD.append('answer %s %s' % (ref[1].decode().strip(), ref[0].hex()))
    subprocess.run(['rm', '-rf', os.path.join(o, 'tree'), os.path.join(o, 'c.bin')])
    return ok


RECORD = None


def recorded(path):
    """The record's cases: name, case, programs by stage, answer bytes and count."""
    out, cur = [], None
    for line in open(path):
        kind, rest = line.rstrip('\n').split(' ', 1)
        if kind == 'case':
            name, js = rest.split(' ', 1)
            cur = {'name': name, 'case': json.loads(js), 'progs': {}}
            out.append(cur)
        elif kind == 'program':
            k, hx = rest.split(' ')
            cur['progs'][k] = bytes.fromhex(hx)
        elif kind == 'answer':
            n, hx = rest.split(' ')
            cur['count'], cur['answer'] = int(n), bytes.fromhex(hx)
    return out


def translate_recorded(work, rec, dest):
    """Translates a recorded case for KAT_HOST into dest; returns its run entry."""
    host = os.environ['KAT_HOST']
    case = rec['case']
    sel = '%d:%s' % (case['stride'], ','.join(str(b) for b in case['select']))
    entry = {'name': rec['name'], 'case': case, 'count': rec['count'], 'answer': rec['answer'].hex(), 'mem': False}
    for k in ('vert', 'geom'):
        if k not in rec['progs']:
            continue
        b = os.path.join(work, '%s-%s.bin' % (rec['name'], k))
        open(b, 'wb').write(rec['progs'][k])
        spv = os.path.join(dest, '%s-%s.spv' % (rec['name'], k))
        flags = ['--xfb', sel] if k == ('geom' if 'geom' in rec['progs'] else 'vert') else []
        if flags and os.environ.get('KAT_XFB_MEMORY'):
            flags.append('--xfb-memory')
        if k == 'geom':
            flags += gs_flag(case)
        out = run([XLATE, b, spv, host] + flags)
        r = subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', spv], capture_output=True, text=True)
        if not out.startswith('ok') or r.returncode:
            sys.exit('%s: %s does not translate for %s: %s %s' % (rec['name'], k, host, out[:300], r.stdout + r.stderr))
        entry[k] = os.path.basename(spv)
        if flags:
            entry['mem'] = 'xfb_memory: Some' in out
            if 'xfb: [(%d, 0)]' % case['stride'] not in out:
                sys.exit('%s: the translation for %s captures nothing: %s' % (rec['name'], host, out[:300]))
    return entry


def run_entry(work, d, e):
    """Captures entry e on XRUN_DEV through its path; whether it matches the recorded answer."""
    case = e['case']
    stride, count = case['stride'], case['count']
    o = os.path.join(work, e['name'] + '-run')
    os.makedirs(o, exist_ok=True)
    env = dict(os.environ)
    if os.environ.get('KAT_DEV'):
        env['XRUN_DEV'] = os.environ['KAT_DEV']
    env = topo_env(env, case)
    if e['mem']:
        env['XRUN_MEM'] = '%d:%d' % (stride + 4, case['slots'])
    gs = os.path.join(d, e['geom']) if 'geom' in e else '-'
    run([os.path.join(work, 'xrun'), os.path.join(d, e['vert']), gs, str(stride), str(count),
         os.path.join(work, 'draw.vert.spv'), os.path.join(work, 'draw.frag.spv'), o], env=env)
    answer = bytes.fromhex(e['answer'])
    if e['mem']:
        got, n = pack(open(os.path.join(o, 'scratch.bin'), 'rb').read(), stride + 4, stride, case['select'],
                      len(answer))
    else:
        got, n = open(os.path.join(o, 'capture.bin'), 'rb').read(), int(open(os.path.join(o, 'count.txt')).read())
    words = lambda b: struct.unpack('<%dI' % (len(b) // 4), b)
    dw = sum(1 for x, y in zip(words(answer), words(got)) if x != y)
    print('%s: %s path: %d bytes (recorded %d); %d words differ from the NVIDIA capture' % (
        e['name'], 'memory' if e['mem'] else 'transform feedback', n, e['count'], dw))
    return dw == 0 and n == e['count'] and len(got) == len(answer)


def prepare(work, d=None):
    d = d or HERE
    inc = ['-I' + i for i in os.environ.get('KAT_VK_INCLUDE', '').split()]
    run(['cc', '-O1'] + inc + ['-o', os.path.join(work, 'xrun'), os.path.join(d, 'xrun.c')] + XRUN_LIBS)
    for s in ('draw.vert', 'draw.frag'):
        if os.path.exists(os.path.join(d, s + '.spv')):
            subprocess.run(['cp', os.path.join(d, s + '.spv'), work])
        else:
            run(['glslangValidator', '-V', '--target-env', 'vulkan1.3', '-o', os.path.join(work, s + '.spv'),
                 os.path.join(d, s)])


XRUN_LIBS = os.environ.get('XRUN_LIBS', '-lvulkan').split()


def pack(scratch, record, stride, select, size):
    """The records packed into a zeroed buffer of size bytes as nvn_xfb_draw_finish packs them; and the count."""
    out, pos = bytearray(size), 0
    for s in range(len(scratch) // record):
        rec = scratch[s * record:(s + 1) * record]
        if struct.unpack_from('<I', rec, stride)[0] != 1 or pos + stride > size:
            continue
        for w in range(stride // 4):
            if w >= len(select) or select[w] != 0xff:
                out[pos + 4 * w:pos + 4 * w + 4] = rec[4 * w:4 * w + 4]
        pos += stride
    return bytes(out), pos


def memory(work, o, name, case, progs, want, sel, host, tenv, ref):
    if want != ['vert', 'geom']:
        print('%s: memory path: only a geometry program takes it' % name)
        return True
    spv = {}
    for k in want:
        spv[k] = os.path.join(o, k + '-mem.spv')
        flags = ['--xfb', sel, '--xfb-memory'] + gs_flag(case) if k == 'geom' else []
        out = run([XLATE, progs[k][0], spv[k], host] + flags)
        r = subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', spv[k]], capture_output=True, text=True)
        if not out.startswith('ok') or r.returncode:
            sys.exit('%s: memory path %s: %s %s' % (name, k, out[:300], r.stdout + r.stderr))
    stride = case['stride']
    record = stride + 4
    env = dict(topo_env(tenv, case), XRUN_MEM='%d:%d' % (record, case['slots']))
    d = os.path.join(o, 'mem')
    os.makedirs(d, exist_ok=True)
    run([os.path.join(work, 'xrun'), spv['vert'], spv['geom'], str(stride), str(case['count']),
         os.path.join(work, 'draw.vert.spv'), os.path.join(work, 'draw.frag.spv'), d], env=env)
    scratch = open(os.path.join(d, 'scratch.bin'), 'rb').read()
    packed, count = pack(scratch, record, stride, case['select'], len(ref[0]))
    words = lambda b: struct.unpack('<%dI' % (len(b) // 4), b)
    dm = sum(1 for x, y in zip(words(ref[0]), words(packed)) if x != y)
    ok = dm == 0 and str(count) == ref[1].decode().strip()
    print('%s: memory path: %d bytes packed (reference %s); %d words differ' % (name, count, ref[1].decode().strip(), dm))
    return ok


def game(work, path):
    sys.path.insert(0, os.path.join(ROOT, 'runtime/tools/nvn2'))
    import glslc
    b = open(path, 'rb').read()
    n = 0
    for h in glslc.headers(b):
        x = glslc.section(b, h)
        if not x or 'layout' not in x:
            continue
        code, size = struct.unpack_from('<I', b, h + 0x30)[0], struct.unpack_from('<I', b, h + 0x38)[0]
        prog, spv = os.path.join(work, 'game-%x.bin' % h), os.path.join(work, 'game-%x.spv' % h)
        open(prog, 'wb').write(b[h + code:h + code + size])
        lay = x['layout']
        if len(lay) != 1 or not x['raw']:
            sys.exit('%#x: layout %s needs more than the one raw buffer xlate takes' % (h, lay))
        sel = '%d:%s' % (lay[0]['stride'], ','.join(str(a & 0xff) for a in lay[0]['attr']))
        out = run([XLATE, prog, spv, os.environ.get('KAT_HOST', 'nvidia'), '--xfb', sel, '--gs-input', 'points'])
        r = subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', spv], capture_output=True, text=True)
        want = 'xfb: [(%d, %d)]' % (lay[0]['stride'], lay[0]['stream'])
        if not out.startswith('ok') or r.returncode or want not in out:
            sys.exit('%#x: %s %s' % (h, out[:300], r.stdout + r.stderr))
        print('%#x: stride %d, select %s: translates, validates and captures %s' % (h, lay[0]['stride'], sel, want))
        n += 1
    if not n:
        sys.exit('no capturing program in %s' % path)


def main():
    if os.environ.get('KAT_DEV') and not os.environ.get('KAT_HOST'):
        sys.exit('KAT_DEV=%s needs KAT_HOST=<preset of that device>: a translation runs only on the host it was made '
                 'for' % os.environ['KAT_DEV'])
    work = sys.argv[1]
    os.makedirs(work, exist_ok=True)
    args = sys.argv[2:]
    if args[:1] == ['--game']:
        return game(work, args[1])
    if args[:1] == ['--run']:
        d = os.path.abspath(args[1])
        prepare(work, d)
        entries = json.load(open(os.path.join(d, 'run.json')))
        bad = [e['name'] for e in entries if not run_entry(work, d, e)]
        if bad:
            sys.exit('differ: %s' % ' '.join(bad))
        return
    if args[:1] == ['--replay']:
        if not os.environ.get('KAT_HOST'):
            sys.exit('--replay needs KAT_HOST=<preset of the device>')
        recs = recorded(args[1])
        export = args[3] if args[2:3] == ['--export'] else None
        d = os.path.abspath(export) if export else os.path.join(work, 'spv')
        os.makedirs(d, exist_ok=True)
        entries = [translate_recorded(work, rec, d) for rec in recs]
        if not entries:
            sys.exit('no cases in %s' % args[1])
        json.dump(entries, open(os.path.join(d, 'run.json'), 'w'))
        if export:
            prepare(work)
            for f in ('xrun.c',):
                subprocess.run(['cp', os.path.join(HERE, f), d])
            for f in ('draw.vert.spv', 'draw.frag.spv'):
                subprocess.run(['cp', os.path.join(work, f), d])
            subprocess.run(['cp', os.path.abspath(__file__), d])
            print('%d cases translated for %s into %s' % (len(entries), os.environ['KAT_HOST'], d))
            return
        prepare(work)
        bad = [e['name'] for e in entries if not run_entry(work, d, e)]
        if bad:
            sys.exit('differ: %s' % ' '.join(bad))
        return
    global RECORD
    record = None
    if args[:1] == ['--record']:
        record, args = args[1], args[2:]
        RECORD = []
    prepare(work)
    names = args or sorted(os.listdir(os.path.join(HERE, 'cases')))
    bad = [n for n in names if not one(work, n)]
    if bad:
        sys.exit('differ: %s' % ' '.join(bad))
    if record:
        open(record, 'w').write('\n'.join(RECORD) + '\n')


if __name__ == '__main__':
    main()
