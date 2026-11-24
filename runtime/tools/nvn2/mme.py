#!/usr/bin/env python3
"""mme.py: Turing/Ampere-class (TU104 encoding) MME macro tool over a captured method stream.

  mme.py stream <stream.bin>                  list the methods of a pushbuffer stream
  mme.py macros <stream.bin>                  the macro start table and instruction RAM the stream loads
  mme.py dis <stream.bin> <macro>             disassemble one macro (index, or call method such as 0xe3a)
  mme.py run <stream.bin> <macro> p0 p1 ...   run a macro on parameters; prints every method it writes

  mme.py seq <stream.bin> m:p0,p1 ...       run macros in order, sharing shadow state and data RAM

Encoding and semantics follow Mesa's nouveau mme_tu104 (MIT): 3 words per instruction, two ALUs, two outputs.
A stream comes from a64run.py: the driver function that uploads the macros (methods 0x45-0x48), run with --dump.
Method names come from clsnames.py when $RELIFT_CLASS_HEADER names a class header.
"""
import struct
import sys

ALU = ["ADD", "ADDC", "SUB", "SUBB", "MUL", "MULH", "MULU", "EXTENDED", "CLZ", "SLL", "SRL", "SRA", "AND", "NAND",
       "OR", "XOR", "MERGE", "SLT", "SLTU", "SLE", "SLEU", "SEQ", "STATE", "LOOP", "JAL", "BLT", "BLTU", "BLE", "BLEU",
       "BEQ", "DREAD", "DWRITE"]
PRED = ["UUUU", "TTTT", "FFFF", "TTUU", "FFUU", "TFUU", "TUUU", "FUUU", "UUTT", "UUTF", "UUTU", "UUFT", "UUFF",
        "UUFU", "UUUT", "UUUF"]
OUT = ["NONE", "ALU0", "ALU1", "LOAD0", "LOAD1", "IMM0", "IMM1", "RES", "IMMHIGH0", "IMMHIGH1", "IMM32"]
ZERO, IMM, IMMPAIR, IMM32, LOAD0, LOAD1 = 24, 25, 26, 27, 28, 29


def bits(b, lo, hi):
    v = 0
    for i in range(lo, hi + 1):
        v |= ((b[i // 32] >> (i % 32)) & 1) << (i - lo)
    return v


def decode(w3):
    b = [w3[2], w3[1], w3[0]]
    return {
        "end": bits(b, 0, 0), "pmode": bits(b, 1, 4), "pred": bits(b, 5, 9),
        "alu": [(bits(b, 10, 14), bits(b, 15, 19), bits(b, 20, 24), bits(b, 25, 29)),
                (bits(b, 46, 50), bits(b, 51, 55), bits(b, 56, 60), bits(b, 61, 65))],
        "imm": [bits(b, 30, 45), bits(b, 66, 81)],
        "out": [(bits(b, 82, 84), bits(b, 85, 88)), (bits(b, 89, 91), bits(b, 92, 95))],
    }


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def stream(data):
    """Yield (method index, value, subchannel) of a GPFIFO pushbuffer (Fermi+ headers)."""
    w = struct.unpack_from("<%dI" % (len(data) // 4), data)
    i = 0
    while i < len(w):
        h = w[i]
        i += 1
        op, cnt, sub, mth = h >> 29, (h >> 16) & 0x1FFF, (h >> 13) & 7, h & 0xFFF
        if h == 0:
            continue
        if op == 4:
            yield mth, cnt, sub
            continue
        for k in range(cnt):
            if i >= len(w):
                return
            m = mth + k if op == 1 else (mth if op == 3 or (op == 5 and k == 0) else mth + 1)
            yield m, w[i], sub
            i += 1


def macros(data):
    ram = {}
    start = {}
    ip = sp = 0
    for m, v, sub in stream(data):
        if m == 0x45:
            ip = 3 * v
        elif m == 0x46:
            ram[ip] = v
            ip += 1
        elif m == 0x47:
            sp = v
        elif m == 0x48:
            start[sp] = v
            sp += 1
    n = max(ram) + 1 if ram else 0
    return [ram.get(i, 0) for i in range(n)], start


def src(i, a, k):
    r = i["alu"][a][2 + k]
    if r < 24:
        return "$r%d" % r
    return {ZERO: "0", IMM: "%d" % s16(i["imm"][a]), IMMPAIR: "%d" % s16(i["imm"][1 - a]),
            IMM32: "0x%x" % (i["imm"][0] << 16 | i["imm"][1]), LOAD0: "$load0", LOAD1: "$load1"}.get(r, "?%d" % r)


def outsrc(i, o):
    return {1: "$alu0", 2: "$alu1", 3: "$load0", 4: "$load1", 5: "0x%x" % i["imm"][0], 6: "0x%x" % i["imm"][1],
            8: "%d" % (i["imm"][0] >> 12), 9: "%d" % (i["imm"][1] >> 12),
            10: "0x%x" % (i["imm"][0] << 16 | i["imm"][1])}.get(o, "?%d" % o)


def text(i):
    parts = []
    if i["pmode"]:
        parts.append("pred %s $r%d:" % (PRED[i["pmode"]], i["pred"]))
    for a in range(2):
        op, dst, s0, s1 = i["alu"][a]
        used = any(x == 1 + a for oo in i["out"] for x in oo)
        if not used and dst == ZERO and ALU[op] not in ("LOOP", "JAL", "BLT", "BLTU", "BLE", "BLEU", "BEQ",
                                                         "EXTENDED", "DWRITE"):
            continue
        name = ALU[op]
        d = ("$r%d = " % dst) if dst < 24 else ("$alu%d = " % a if used else "")
        if name == "MERGE":
            im = i["imm"][a]
            parts.append("%sMERGE %s %s (dst %d, bits %d, src %d)" % (d, src(i, a, 0), src(i, a, 1), (im >> 10) & 0x3F,
                                                                     (im >> 5) & 0x1F, im & 0x1F))
        elif name == "STATE":
            parts.append("%sSTATE %s + %s" % (d, src(i, a, 0), src(i, a, 1)))
        elif name in ("BLT", "BLTU", "BLE", "BLEU", "BEQ"):
            im = i["imm"][a]
            off = s13(im)
            parts.append("%s%s %s %s -> %s%+d" % (d, name, src(i, a, 0), src(i, a, 1), "T" if im & 0x8000 else "F", off))
        elif name == "LOOP":
            parts.append("LOOP %s (%d)" % (src(i, a, 0), i["imm"][a]))
        elif name == "JAL":
            parts.append("JAL +%d" % (i["imm"][a] & 0x7FFF))
        else:
            parts.append("%s%s %s %s" % (d, name, src(i, a, 0), src(i, a, 1)))
    for o in range(2):
        mt, em = i["out"][o]
        if mt:
            s = outsrc(i, mt)
            if mt in (5, 6):
                v = i["imm"][mt - 5]
                s = "0x%x inc %d" % (v & 0xFFF, v >> 12)
            parts.append("mthd(%s)" % s)
        if em:
            parts.append("emit(%s)" % outsrc(i, em))
    if i["end"]:
        parts.append("END_NEXT")
    return "; ".join(parts)


def s13(im):
    v = im & 0x1FFF
    return v - 0x2000 if v & 0x1000 else v


class Sim:
    def __init__(self, ram, params, state=None, dram=None):
        self.ram = ram
        self.params = list(params)
        self.state = dict(state or {})
        self.dram = dict(dram or {})
        self.writes = []

    def run(self, ip, max_steps=100000):
        regs = [0] * 24
        load = [0, 0]
        res = [0, 0]
        carry = 0
        mthd = inc = 0
        loop_count = loop_start = loop_end = 0
        end_next = False
        steps = 0
        while True:
            steps += 1
            if steps > max_steps:
                raise RuntimeError("step limit")
            w = self.ram[3 * ip: 3 * ip + 3]
            i = decode(w)
            nip = ip + 1
            uses = [i["pred"]] + [x for a in i["alu"] for x in a[2:]]
            outs = [x for o in i["out"] for x in o]
            if LOAD0 in uses or 3 in outs:
                load[0] = self.params.pop(0) if self.params else 0
            if LOAD1 in uses or 4 in outs:
                load[1] = self.params.pop(0) if self.params else 0

            def rd(a, k):
                r = i["alu"][a][2 + k]
                if r < 24:
                    return regs[r]
                return {ZERO: 0, IMM: s16(i["imm"][a]) & 0xFFFFFFFF, IMMPAIR: s16(i["imm"][1 - a]) & 0xFFFFFFFF,
                        IMM32: i["imm"][0] << 16 | i["imm"][1], LOAD0: load[0], LOAD1: load[1]}[r]

            mask = 0xF
            if i["pmode"]:
                pv = regs[i["pred"]] if i["pred"] < 24 else rd(0, 0)
                p = PRED[i["pmode"]]
                mask = 0
                for k in range(4):
                    if p[k] != ("T" if pv else "F"):
                        mask |= 1 << k
            for a in range(2):
                if not mask & (1 << a):
                    continue
                op, dst, _, _ = i["alu"][a]
                x, y = rd(a, 0), rd(a, 1)
                name = ALU[op]
                r = 0
                if name == "ADD":
                    r = (x + y) & 0xFFFFFFFF
                    carry = int(x + y > 0xFFFFFFFF)
                elif name == "ADDC":
                    r = (x + y + carry) & 0xFFFFFFFF
                elif name == "SUB":
                    r = (x - y) & 0xFFFFFFFF
                    carry = int(r > x)
                elif name == "SUBB":
                    r = (x - y - carry) & 0xFFFFFFFF
                elif name in ("MUL", "MULU"):
                    xs, ys = (x - (1 << 32) if x >> 31 else x, y - (1 << 32) if y >> 31 else y) if name == "MUL" else (x, y)
                    p64 = (xs * ys) & 0xFFFFFFFFFFFFFFFF
                    r, carry = p64 & 0xFFFFFFFF, p64 >> 32
                elif name == "MULH":
                    r = carry
                elif name == "CLZ":
                    r = 32 - x.bit_length()
                elif name == "SLL":
                    r = (x << (y & 31)) & 0xFFFFFFFF
                elif name == "SRL":
                    r = x >> (y & 31)
                elif name == "SRA":
                    r = ((x - (1 << 32) if x >> 31 else x) >> (y & 31)) & 0xFFFFFFFF
                elif name == "AND":
                    r = x & y
                elif name == "NAND":
                    r = ~(x & y) & 0xFFFFFFFF
                elif name == "OR":
                    r = x | y
                elif name == "XOR":
                    r = x ^ y
                elif name == "MERGE":
                    im = i["imm"][a]
                    dp, nb, sp_ = (im >> 10) & 0x3F, (im >> 5) & 0x1F, im & 0x1F
                    m = ((1 << nb) - 1) << dp
                    r = ((x & ~m) | (((y >> sp_) & ((1 << nb) - 1)) << dp)) & 0xFFFFFFFF
                elif name in ("SLT", "SLTU", "SLE", "SLEU", "SEQ"):
                    r = 0xFFFFFFFF if cond(name, x, y) else 0
                elif name == "STATE":
                    m = (x + y) & 0xFFFF
                    r = self.state.get(m, 0)
                elif name == "LOOP":
                    loop_count = max(1, x) - 1
                    loop_start = ip
                    loop_end = ip + i["imm"][a] - 1
                elif name == "JAL":
                    nip = ip + (i["imm"][a] & 0x7FFF)
                elif name in ("BLT", "BLTU", "BLE", "BLEU", "BEQ"):
                    im = i["imm"][a]
                    if cond(name, x, y) == bool(im & 0x8000):
                        if s13(im) == -0x1000:
                            return self.writes
                        nip = ip + s13(im)
                elif name == "DREAD":
                    r = self.dram.get(x, 0)
                elif name == "DWRITE":
                    self.dram[x] = y
                elif name == "EXTENDED":
                    self.writes.append(("EXTENDED", x, y))
                res[a] = r
                if dst < 24:
                    regs[dst] = r
            for o in range(2):
                if not mask & (4 << o):
                    continue
                mt, em = i["out"][o]

                def ov(k):
                    return {1: res[0], 2: res[1], 3: load[0], 4: load[1], 5: i["imm"][0], 6: i["imm"][1],
                            8: i["imm"][0] >> 12, 9: i["imm"][1] >> 12, 10: i["imm"][0] << 16 | i["imm"][1]}[k]
                if mt:
                    v = ov(mt)
                    mthd, inc = v & 0xFFF, (v >> 12) & 0xF
                if em:
                    v = ov(em)
                    self.writes.append((mthd, v))
                    self.state[mthd] = v
                    mthd = (mthd + inc) & 0xFFF
            if end_next:
                break
            end_next = bool(i["end"])
            if loop_count > 0 and ip == loop_end:
                loop_count -= 1
                nip = loop_start + 1
            ip = nip
        return self.writes


def cond(name, x, y):
    sx = x - (1 << 32) if x >> 31 else x
    sy = y - (1 << 32) if y >> 31 else y
    return {"SLT": sx < sy, "BLT": sx < sy, "SLTU": x < y, "BLTU": x < y, "SLE": sx <= sy, "BLE": sx <= sy,
            "SLEU": x <= y, "BLEU": x <= y, "SEQ": x == y, "BEQ": x == y}[name]


def replay(data, ram, start, log=None):
    """Runs a whole stream as the class would: plain methods set the shadow state, a write to an even method at
    0xe00 or above starts that macro with its value as the first parameter, the odd method after it adds
    parameters, and the macro runs when its parameters end. Returns the shadow state and the macro data RAM."""
    state, dram = {}, {}
    pend = None

    def flush():
        nonlocal pend
        if pend is None:
            return
        k, params = pend
        pend = None
        if k in start:
            sim = Sim(ram, params, state=state, dram=dram)
            sim.run(start[k])
            if log is not None:
                log.append((k, params, sim.writes))
            state.update(sim.state)
            dram.update(sim.dram)

    for m, v, sub in stream(data):
        if sub != 0:
            continue
        if m >= 0xE00:
            k = (m - 0xE00) // 2
            if m % 2 == 0:
                flush()
                pend = (k, [v])
            elif pend is not None and pend[0] == k:
                pend[1].append(v)
            continue
        flush()
        state[m] = v
    flush()
    return state, dram


def macro_index(s):
    v = int(s, 0)
    return (v - 0xE00) // 2 if v >= 0xE00 else v


def macro_range(ram, start, idx):
    ip = start[idx]
    ends = sorted(v for v in start.values() if v > ip)
    hi = ends[0] if ends else len(ram) // 3
    return ip, hi


def main():
    cmd = sys.argv[1]
    data = open(sys.argv[2], "rb").read()
    if cmd == "stream":
        for m, v, sub in stream(data):
            print("sub %d  0x%03x (byte 0x%04x) = 0x%08x" % (sub, m, m * 4, v))
        return
    ram, start = macros(data)
    if cmd == "macros":
        print("instruction ram: %d words (%d instructions)" % (len(ram), len(ram) // 3))
        for k in sorted(start, key=lambda k: start[k]):
            lo, hi = macro_range(ram, start, k)
            print("macro %2d (call method 0x%03x) at %4d, %d instructions" % (k, 0xE00 + 2 * k, lo, hi - lo))
        return
    if cmd == "seq":
        import os
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import clsnames
        names = clsnames.load()
        state, dram = replay(data, ram, start)
        for arg in sys.argv[3:]:
            c, _, ps = arg.partition(":")
            k = macro_index(c)
            params = [int(x, 0) for x in ps.split(",") if x]
            sim = Sim(ram, params, state=state, dram=dram)
            print("-- 0x%03x %s" % (0xE00 + 2 * k, [hex(x) for x in params]))
            for wr in sim.run(start[k]):
                if wr[0] == "EXTENDED":
                    print("EXTENDED 0x%x 0x%x" % wr[1:])
                else:
                    print("0x%03x (byte 0x%04x) %-40s = 0x%08x" % (wr[0], wr[0] * 4, names.get(wr[0], "?"), wr[1]))
            state, dram = sim.state, sim.dram
        return
    idx = macro_index(sys.argv[3])
    if cmd == "dis":
        lo, hi = macro_range(ram, start, idx)
        for ip in range(lo, hi):
            print("%4d: %s" % (ip, text(decode(ram[3 * ip: 3 * ip + 3]))))
        return
    if cmd == "run":
        import os
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import clsnames
        names = clsnames.load()
        params = [int(x, 0) for x in sys.argv[4:]]
        log = []
        state, dram = replay(data, ram, start, log)
        if os.environ.get("MME_LOG"):
            for k, p, w in log:
                print("init calls macro %d (0x%03x) with %s: %d writes" % (k, 0xE00 + 2 * k, [hex(x) for x in p],
                                                                            len(w)))
        sim = Sim(ram, params, state=state, dram=dram)
        for wr in sim.run(start[idx]):
            if wr[0] == "EXTENDED":
                print("EXTENDED 0x%x 0x%x" % wr[1:])
            else:
                print("0x%03x (byte 0x%04x) %-40s = 0x%08x" % (wr[0], wr[0] * 4, names.get(wr[0], "?"), wr[1]))
        print("unused params: %d" % len(sim.params))


if __name__ == "__main__":
    main()
