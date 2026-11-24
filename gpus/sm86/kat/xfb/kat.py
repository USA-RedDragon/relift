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
target/). KAT_DEV=<device name part> runs the translation on another device (the reference stays NVIDIA's).

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
        sys.exit('%s failed: %s %s' % (os.path.basename(args[0]), r.stdout[-2000:], r.stderr[-2000:]))
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


def capture(work, o, sub, vs, gs, case, env=None):
    d = os.path.join(o, sub)
    os.makedirs(d, exist_ok=True)
    run([os.path.join(work, 'xrun'), vs, gs or '-', str(case['stride']), str(case['count']),
         os.path.join(work, 'draw.vert.spv'), os.path.join(work, 'draw.frag.spv'), d], env=env)
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
        flags = ['--xfb', sel] if k == want[-1] else []
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
    subprocess.run(['rm', '-rf', os.path.join(o, 'tree'), os.path.join(o, 'c.bin')])
    return dc == 0 and di == 0 and same_count and lit > 0 and mem_ok


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
        flags = ['--xfb', sel, '--xfb-memory'] if k == 'geom' else []
        out = run([XLATE, progs[k][0], spv[k], host] + flags)
        r = subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', spv[k]], capture_output=True, text=True)
        if not out.startswith('ok') or r.returncode:
            sys.exit('%s: memory path %s: %s %s' % (name, k, out[:300], r.stdout + r.stderr))
    stride = case['stride']
    record = stride + 4
    env = dict(tenv, XRUN_MEM='%d:%d' % (record, case['count']))
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
        out = run([XLATE, prog, spv, os.environ.get('KAT_HOST', 'nvidia'), '--xfb', sel])
        r = subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', spv], capture_output=True, text=True)
        want = 'xfb: [(%d, %d)]' % (lay[0]['stride'], lay[0]['stream'])
        if not out.startswith('ok') or r.returncode or want not in out:
            sys.exit('%#x: %s %s' % (h, out[:300], r.stdout + r.stderr))
        print('%#x: stride %d, select %s: translates, validates and captures %s' % (h, lay[0]['stride'], sel, want))
        n += 1
    if not n:
        sys.exit('no capturing program in %s' % path)


def main():
    work = sys.argv[1]
    os.makedirs(work, exist_ok=True)
    if sys.argv[2:3] == ['--game']:
        return game(work, sys.argv[3])
    run(['cc', '-O1', '-o', os.path.join(work, 'xrun'), os.path.join(HERE, 'xrun.c'), '-lvulkan'])
    for s in ('draw.vert', 'draw.frag'):
        run(['glslangValidator', '-V', '--target-env', 'vulkan1.3', '-o', os.path.join(work, s + '.spv'),
             os.path.join(HERE, s)])
    names = sys.argv[2:] or sorted(os.listdir(os.path.join(HERE, 'cases')))
    bad = [n for n in names if not one(work, n)]
    if bad:
        sys.exit('differ: %s' % ' '.join(bad))


if __name__ == '__main__':
    main()
