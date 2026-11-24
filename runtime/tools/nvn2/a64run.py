#!/usr/bin/env python3
"""a64run.py: run one A64 function of an uncompressed NSO over a sparse memory and record its stores.

A small interpreter for the integer, load/store and few NEON forms that NVN driver pushbuffer writers use. Calls
(bl/blr) are not followed: a call returns 0 in x0 unless --call gives its result. Memory reads of module addresses come
from the NSO's segments; other reads return what the run stored or 0. Every branch whose condition was computed from a
value read from unset memory is reported, so a run that took an input dependent path says so.

usage: a64run.py <nso> <entry hex> [--reg x0=0x...] [--mem addr=u64] [--cmd field_off] [--out stream.bin]
"""
import argparse
import re
import struct
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

M64 = (1 << 64) - 1
M128 = (1 << 128) - 1


class Nso:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        self.segs = [struct.unpack_from("<III", self.d, o) for o in (0x10, 0x20, 0x30)]
        flags = struct.unpack_from("<I", self.d, 0xC)[0]
        assert flags & 7 == 0, "compressed NSO"

    def byte(self, a):
        for f, m, s in self.segs:
            if m <= a < m + s:
                return self.d[f + a - m]
        return None

    def text(self, a, n):
        f, m, s = self.segs[0]
        return self.d[f + a - m: f + a - m + n]


class Mem:
    """Sparse little-endian memory; reads of untouched bytes fall back to the module image, then to 0."""

    def __init__(self, nso):
        self.nso = nso
        self.b = {}
        self.unset_reads = 0

    def read(self, a, n):
        v = 0
        unset = False
        for i in range(n):
            x = self.b.get(a + i)
            if x is None:
                x = self.nso.byte(a + i)
                if x is None:
                    x = 0
                    unset = True
            v |= x << (8 * i)
        return v, unset

    def write(self, a, n, v):
        for i in range(n):
            self.b[a + i] = (v >> (8 * i)) & 0xFF


SIZES = {"x": 8, "w": 4, "q": 16, "d": 8, "s": 4, "h": 2, "b": 1}


class Cpu:
    def __init__(self, nso, mem):
        self.nso = nso
        self.mem = mem
        self.x = [0] * 32
        self.taint = [False] * 32
        self.v = [0] * 32
        self.n = self.z = self.c = self.vf = False
        self.ftaint = False
        self.md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        self.cache = {}
        self.calls = {}
        self.stores = []
        self.tainted_branches = []
        self.steps = 0
        self.follow = False
        self.null_calls = []
        self.imports = []
        self.natives = {}
        self.last = 0
        self.stubs = {}

    def reg(self, r):
        r = r.strip()
        if r in ("xzr", "wzr"):
            return 0, False
        if r == "sp":
            return self.x[31], False
        if r == "wsp":
            return self.x[31] & 0xFFFFFFFF, False
        k, i = r[0], int(r[1:])
        if k == "x":
            return self.x[i], self.taint[i]
        if k == "w":
            return self.x[i] & 0xFFFFFFFF, self.taint[i]
        raise ValueError("reg " + r)

    def setreg(self, r, val, t=False):
        r = r.strip()
        if r in ("xzr", "wzr"):
            return
        if r == "sp":
            self.x[31] = val & M64
            return
        k, i = r[0], int(r[1:])
        if k == "x":
            self.x[i] = val & M64
        elif k == "w":
            self.x[i] = val & 0xFFFFFFFF
        else:
            raise ValueError("setreg " + r)
        self.taint[i] = t

    def is64(self, r):
        return r.strip()[0] in "x" or r.strip() == "sp"

    def vreg(self, r):
        r = r.strip()
        k, i = r[0], int(re.match(r"[a-z](\d+)", r).group(1))
        return k, i

    def imm(self, s):
        s = s.strip().lstrip("#")
        return int(s, 0)

    def shifted(self, ops, i, width):
        """Value of operand ops[i] plus an optional shift/extend in ops[i+1]."""
        o = ops[i].strip()
        if o.startswith("#"):
            v, t = self.imm(o), False
        else:
            v, t = self.reg(o)
        if i + 1 < len(ops):
            sh = ops[i + 1].strip()
            m = re.match(r"(lsl|lsr|asr|ror|uxtw|sxtw|uxtx|sxtx|uxtb|uxth)\s*(?:#(\S+))?", sh)
            if m:
                kind, amt = m.group(1), int(m.group(2), 0) if m.group(2) else 0
                if kind == "lsl":
                    v = v << amt
                elif kind == "lsr":
                    v = v >> amt
                elif kind == "asr":
                    if v >> (width - 1):
                        v -= 1 << width
                    v = v >> amt
                elif kind == "uxtw":
                    v = (v & 0xFFFFFFFF) << amt
                elif kind == "sxtw":
                    v &= 0xFFFFFFFF
                    if v >> 31:
                        v -= 1 << 32
                    v = v << amt
                elif kind == "uxtb":
                    v = (v & 0xFF) << amt
                elif kind == "uxth":
                    v = (v & 0xFFFF) << amt
                else:
                    v = v << amt
        return v & ((1 << width) - 1), t

    def addr(self, memop):
        """Address of '[base, off]' or '[base, reg, lsl #n]'."""
        inner = memop.strip()[1:].rstrip("!").rstrip("]")
        parts = [p.strip() for p in inner.split(",")]
        base, t = self.reg(parts[0])
        if len(parts) == 1:
            return base, t
        if parts[1].startswith("#"):
            return (base + self.imm(parts[1])) & M64, t
        off, t2 = self.shifted(parts, 1, 64)
        return (base + off) & M64, t or t2

    def flags_add(self, a, b, width, carry=0):
        mask = (1 << width) - 1
        r = a + b + carry
        res = r & mask
        sa, sb, sr = a >> (width - 1), b >> (width - 1), res >> (width - 1)
        self.n = bool(sr)
        self.z = res == 0
        self.c = r > mask
        self.vf = sa == sb and sr != sa
        return res

    def cond(self, cc):
        n, z, c, v = self.n, self.z, self.c, self.vf
        return {
            "eq": z, "ne": not z, "hs": c, "cs": c, "lo": not c, "cc": not c, "mi": n, "pl": not n,
            "vs": v, "vc": not v, "hi": c and not z, "ls": not c or z, "ge": n == v, "lt": n != v,
            "gt": not z and n == v, "le": z or n != v, "al": True,
        }[cc]

    def decode(self, pc):
        ins = self.cache.get(pc)
        if ins is None:
            i = next(self.md.disasm(self.nso.text(pc, 4), pc), None)
            ins = (i.mnemonic, i.op_str) if i else (".word", "")
            self.cache[pc] = ins
        return ins

    def note_branch(self, pc, t):
        if t:
            self.tainted_branches.append(pc)

    def run(self, pc, lr_stop=0xDEAD0000, max_steps=2_000_000):
        self.x[30] = lr_stop
        while pc != lr_stop:
            self.steps += 1
            if self.steps > max_steps:
                raise RuntimeError("step limit at %x" % pc)
            if pc == 0 or self.nso.byte(pc) is None:
                raise RuntimeError("jump to %x (not in the module) from %x" % (pc, self.last))
            self.last = pc
            mn, op = self.decode(pc)
            npc = self.step(pc, mn, op)
            pc = npc if npc is not None else pc + 4
        return self.x[0]

    def split(self, op):
        out, depth, cur = [], 0, ""
        for ch in op:
            if ch == "[":
                depth += 1
            elif ch == "]":
                depth -= 1
            if ch == "," and depth == 0:
                out.append(cur)
                cur = ""
            else:
                cur += ch
        if cur:
            out.append(cur)
        return [o.strip() for o in out]

    def ldst(self, mn, ops, pc):
        load = mn.startswith("ld")
        pair = mn in ("ldp", "stp", "ldnp", "stnp")
        regs = ops[:2] if pair else ops[:1]
        memi = 2 if pair else 1
        memop = ops[memi]
        post = memi + 1 < len(ops)
        pre = memop.endswith("!")
        base_reg = memop.strip()[1:].split(",")[0].rstrip("]").strip()
        if post:
            a, t = self.reg(base_reg)
        else:
            a, t = self.addr(memop)
        r0 = regs[0]
        size = SIZES[r0[0]] if r0[0] in SIZES else 8
        if mn in ("ldrb", "strb", "ldrsb"):
            size = 1
        elif mn in ("ldrh", "strh", "ldrsh"):
            size = 2
        elif mn == "ldrsw":
            size = 4
        elif mn == "ldrsb":
            size = 1
        elif mn == "ldrsh":
            size = 2
        for k, r in enumerate(regs):
            ea = (a + k * size) & M64
            if load:
                v, unset = self.mem.read(ea, size)
                if mn in ("ldrsw", "ldrsb", "ldrsh") and v >> (8 * size - 1):
                    v = (v - (1 << (8 * size))) & (M64 if r[0] == "x" else 0xFFFFFFFF)
                if r[0] in "qdsh" or (r[0] == "b" and r[1:].isdigit()):
                    self.v[int(r[1:])] = v
                else:
                    self.setreg(r, v, unset or t)
            else:
                if r[0] in "qdsh" or (r[0] == "b" and r[1:].isdigit() and not r.startswith("b.")):
                    v = self.v[int(r[1:])] & ((1 << (8 * size)) - 1)
                else:
                    v, _ = self.reg(r)
                self.mem.write(ea, size, v)
                self.stores.append((pc, ea, size, v))
        if post:
            self.setreg(base_reg, a + self.imm(ops[memi + 1]))
        elif pre:
            self.setreg(base_reg, a)

    def step(self, pc, mn, op):
        ops = self.split(op)
        if ops and re.match(r"v\d+\.", ops[0]) and mn in ("add", "orr", "and", "cmhs", "movi", "dup", "cmgt", "cmeq",
                                                            "cmhi", "bsl", "bif", "bit"):
            return self.neon(mn, ops, pc)
        if mn == "movi":
            return self.neon(mn, ops, pc)
        if mn == "fcsel":
            k, i = self.vreg(ops[0])
            _, j = self.vreg(ops[1])
            _, l = self.vreg(ops[2])
            self.v[i] = self.v[j] if self.cond(ops[3]) else self.v[l]
            return None
        if mn in ("nop", "pacibsp", "autibsp", "hint", "bti", "dmb", "dsb", "isb", "prfm"):
            return None
        if mn in ("ret", "retab", "retaa"):
            return self.x[30]
        if mn == "b":
            return self.imm(ops[0])
        if mn.startswith("b.") and len(mn) > 2:
            self.note_branch(pc, self.ftaint)
            return self.imm(ops[0]) if self.cond(mn[2:]) else None
        if mn in ("cbz", "cbnz"):
            v, t = self.reg(ops[0])
            self.note_branch(pc, t)
            return self.imm(ops[1]) if (v == 0) == (mn == "cbz") else None
        if mn in ("tbz", "tbnz"):
            v, t = self.reg(ops[0])
            self.note_branch(pc, t)
            bit = (v >> self.imm(ops[1])) & 1
            return self.imm(ops[2]) if (bit == 0) == (mn == "tbz") else None
        if mn in ("bl", "blr", "blraa", "blrab", "blraaz", "blrabz"):
            target = self.imm(ops[0]) if mn == "bl" else self.reg(ops[0])[0]
            self.calls[target] = self.calls.get(target, 0) + 1
            if target in self.natives:
                self.natives[target](self)
            elif target == 0:
                self.null_calls.append(pc)
            elif self.decode(target) == ("adrp", "x16, " + self.decode(target)[1].split(", ")[-1]) and \
                    self.decode(target + 4)[0] == "ldr" and self.decode(target + 12)[0] == "br":
                self.imports.append((pc, target))
                self.x[0] = self.stubs.get(target, 0)
            elif self.follow and target not in self.stubs and self.nso.byte(target) is not None:
                self.x[30] = pc + 4
                return target
            self.x[0] = self.stubs.get(target, 0)
            return None
        if mn in ("br", "braa", "brab", "braaz", "brabz"):
            return self.reg(ops[0])[0]
        if mn in ("ldr", "str", "ldur", "stur", "ldp", "stp", "ldrb", "strb", "ldrh", "strh", "ldurb", "sturb",
                  "ldrsw", "ldursw", "ldurh", "sturh", "ldrsb", "ldrsh", "ldursb", "ldursh"):
            m2 = {"ldur": "ldr", "stur": "str", "ldurb": "ldrb", "sturb": "strb", "ldursw": "ldrsw", "ldurh": "ldrh", "ldursb": "ldrsb", "ldursh": "ldrsh",
                  "sturh": "strh"}.get(mn, mn)
            if m2 == "ldr" and ops[1].startswith("#"):
                raise RuntimeError("literal load at %x" % pc)
            self.ldst(m2, ops, pc)
            return None
        if mn == "adrp" or mn == "adr":
            self.setreg(ops[0], self.imm(ops[1]))
            return None
        dst = ops[0] if ops else ""
        w = 64 if dst and dst[0] == "x" or dst == "sp" else 32
        mask = (1 << w) - 1
        if mn in ("add", "sub", "adds", "subs", "cmp", "cmn"):
            if mn in ("cmp", "cmn"):
                a, ta = self.reg(ops[0])
                w = 64 if ops[0][0] == "x" else 32
                b, tb = self.shifted(ops, 1, w)
            else:
                a, ta = self.reg(ops[1])
                b, tb = self.shifted(ops, 2, w)
            mask = (1 << w) - 1
            if mn in ("sub", "subs", "cmp"):
                r = self.flags_add(a, (~b) & mask, w, 1) if mn != "sub" else (a - b) & mask
            else:
                r = self.flags_add(a, b, w) if mn != "add" else (a + b) & mask
            if mn in ("adds", "subs", "cmp", "cmn"):
                self.ftaint = ta or tb
            if mn not in ("cmp", "cmn"):
                self.setreg(dst, r, ta or tb)
            return None
        if mn in ("tst",):
            a, ta = self.reg(ops[0])
            w = 64 if ops[0][0] == "x" else 32
            b, tb = self.shifted(ops, 1, w)
            r = a & b
            self.n, self.z, self.c, self.vf = bool(r >> (w - 1)), r == 0, False, False
            self.ftaint = ta or tb
            return None
        if mn in ("and", "orr", "eor", "ands", "bic", "orn"):
            a, ta = self.reg(ops[1])
            b, tb = self.shifted(ops, 2, w)
            if mn in ("bic", "orn"):
                b = (~b) & mask
            r = {"and": a & b, "ands": a & b, "bic": a & b, "orr": a | b, "orn": a | b, "eor": a ^ b}[mn] & mask
            if mn == "ands":
                self.n, self.z, self.c, self.vf = bool(r >> (w - 1)), r == 0, False, False
                self.ftaint = ta or tb
            self.setreg(dst, r, ta or tb)
            return None
        if mn in ("neg", "negs"):
            b, tb = self.shifted(ops, 1, w)
            r = self.flags_add(0, (~b) & mask, w, 1) if mn == "negs" else (-b) & mask
            if mn == "negs":
                self.ftaint = tb
            self.setreg(dst, r, tb)
            return None
        if mn in ("clz", "rbit"):
            a, ta = self.reg(ops[1])
            if mn == "clz":
                r = w - a.bit_length()
            else:
                r = int(format(a, "0%db" % w)[::-1], 2)
            self.setreg(dst, r, ta)
            return None
        if mn == "mvn":
            b, tb = self.shifted(ops, 1, w)
            self.setreg(dst, (~b) & mask, tb)
            return None
        lane = re.compile(r"v(\d+)\.([bhsd])\[(\d+)\]")
        if mn in ("mov", "umov", "ins") and (lane.match(ops[0]) or lane.match(ops[1]) or ops[0].startswith("v")):
            lb = {"b": 8, "h": 16, "s": 32, "d": 64}
            md, ms = lane.match(ops[0]), lane.match(ops[1])
            if ms:
                j, k, idx = int(ms.group(1)), ms.group(2), int(ms.group(3))
                val = (self.v[j] >> (idx * lb[k])) & ((1 << lb[k]) - 1)
            elif ops[1].startswith("v"):
                val = self.v[int(re.match(r"v(\d+)", ops[1]).group(1))]
            else:
                val = self.reg(ops[1])[0]
            if md:
                i, k, idx = int(md.group(1)), md.group(2), int(md.group(3))
                m = ((1 << lb[k]) - 1) << (idx * lb[k])
                self.v[i] = (self.v[i] & ~m) | ((val << (idx * lb[k])) & m)
            elif ops[0].startswith("v"):
                self.v[int(re.match(r"v(\d+)", ops[0]).group(1))] = val
            else:
                self.setreg(ops[0], val & mask)
            return None
        if mn == "rev64":
            i, n, bits = self.lanes(ops[0])
            j, _, _ = self.lanes(ops[1])
            src, out, per = self.v[j], 0, 64 // bits
            for k in range(n):
                g, e = divmod(k, per)
                v = (src >> (k * bits)) & ((1 << bits) - 1)
                out |= v << ((g * per + (per - 1 - e)) * bits)
            self.v[i] = out
            return None
        if mn in ("mov", "movz", "movn"):
            src = ops[1]
            if src.startswith("#"):
                v, t = self.shifted(ops, 1, w)
                if mn == "movn":
                    v = ~v
                self.setreg(dst, v & mask)
            else:
                v, t = self.reg(src)
                self.setreg(dst, v & mask, t)
            return None
        if mn == "movk":
            v, t = self.reg(dst)
            imm = self.imm(ops[1])
            sh = int(ops[2].split("#")[1], 0) if len(ops) > 2 else 0
            v = (v & ~(0xFFFF << sh)) | (imm << sh)
            self.setreg(dst, v & mask, t)
            return None
        if mn in ("lsl", "lsr", "asr"):
            a, ta = self.reg(ops[1])
            if ops[2].startswith("#"):
                s, ts = self.imm(ops[2]), False
            else:
                s, ts = self.reg(ops[2])
                s &= w - 1
            if mn == "lsl":
                r = a << s
            elif mn == "lsr":
                r = a >> s
            else:
                if a >> (w - 1):
                    a -= 1 << w
                r = a >> s
            self.setreg(dst, r & mask, ta or ts)
            return None
        if mn in ("ubfiz", "ubfx", "sbfx", "sbfiz", "bfi", "bfxil"):
            a, ta = self.reg(ops[1])
            lsb, width = self.imm(ops[2]), self.imm(ops[3])
            f = (1 << width) - 1
            if mn == "ubfiz":
                r = (a & f) << lsb
            elif mn == "ubfx":
                r = (a >> lsb) & f
            elif mn == "sbfx":
                r = (a >> lsb) & f
                if r >> (width - 1):
                    r -= 1 << width
            elif mn == "sbfiz":
                r = a & f
                if r >> (width - 1):
                    r -= 1 << width
                r <<= lsb
            elif mn == "bfi":
                d0, td = self.reg(dst)
                r = (d0 & ~(f << lsb)) | ((a & f) << lsb)
                ta = ta or td
            else:
                d0, td = self.reg(dst)
                r = (d0 & ~f) | ((a >> lsb) & f)
                ta = ta or td
            self.setreg(dst, r & mask, ta)
            return None
        if mn in ("uxtw", "sxtw", "uxtb", "uxth", "sxtb", "sxth"):
            a, ta = self.reg(ops[1])
            bits = {"uxtw": 32, "sxtw": 32, "uxtb": 8, "uxth": 16, "sxtb": 8, "sxth": 16}[mn]
            r = a & ((1 << bits) - 1)
            if mn[0] == "s" and r >> (bits - 1):
                r -= 1 << bits
            self.setreg(dst, r & mask, ta)
            return None
        if mn in ("umull", "smull", "mul", "madd", "msub", "umulh"):
            a, ta = self.reg(ops[1])
            b, tb = self.reg(ops[2])
            if mn == "smull":
                a = a - (1 << 32) if a >> 31 else a
                b = b - (1 << 32) if b >> 31 else b
            r = a * b
            if mn == "umulh":
                r >>= 64
            if mn in ("madd", "msub"):
                c, tc = self.reg(ops[3])
                r = c + r if mn == "madd" else c - r
                tb = tb or tc
            self.setreg(dst, r & mask, ta or tb)
            return None
        if mn in ("udiv", "sdiv"):
            a, ta = self.reg(ops[1])
            b, tb = self.reg(ops[2])
            self.setreg(dst, (a // b if b else 0) & mask, ta or tb)
            return None
        if mn in ("csel", "csinc", "csinv", "csneg", "cset", "csetm", "cinc"):
            if mn in ("cset", "csetm"):
                r = (mask if mn == "csetm" else 1) if self.cond(ops[1]) else 0
                self.setreg(dst, r, self.ftaint)
                return None
            if mn == "cinc":
                a, ta = self.reg(ops[1])
                self.setreg(dst, (a + 1 if self.cond(ops[2]) else a) & mask, ta or self.ftaint)
                return None
            a, ta = self.reg(ops[1])
            b, tb = self.reg(ops[2])
            if not self.cond(ops[3]):
                b = {"csel": b, "csinc": b + 1, "csinv": ~b, "csneg": -b}[mn]
                r, t = b, tb
            else:
                r, t = a, ta
            self.setreg(dst, r & mask, t or self.ftaint)
            return None
        if mn in ("ccmp", "ccmn"):
            if self.cond(ops[3]):
                a, ta = self.reg(ops[0])
                w2 = 64 if ops[0][0] == "x" else 32
                b, tb = self.shifted(ops, 1, w2)
                m2 = (1 << w2) - 1
                if mn == "ccmp":
                    self.flags_add(a, (~b) & m2, w2, 1)
                else:
                    self.flags_add(a, b, w2)
                self.ftaint = self.ftaint or ta or tb
            else:
                nzcv = self.imm(ops[2])
                self.n, self.z, self.c, self.vf = bool(nzcv & 8), bool(nzcv & 4), bool(nzcv & 2), bool(nzcv & 1)
            return None
        if mn == "fmov":
            k, i = self.vreg(ops[0])
            if ops[1][0] in "wx":
                self.v[i], _ = self.reg(ops[1])
                return None
            if ops[1].startswith("#"):
                f = float(ops[1][1:])
                if k == "d":
                    self.v[i] = struct.unpack("<Q", struct.pack("<d", f))[0]
                else:
                    self.v[i] = struct.unpack("<I", struct.pack("<f", f))[0]
                return None
            k2, j = self.vreg(ops[1])
            if ops[0][0] in "wx":
                self.setreg(ops[0], self.v[j] & mask)
            else:
                self.v[i] = self.v[j] & ((1 << (8 * SIZES[k])) - 1)
            return None
        if mn in ("dup", "cmgt", "cmeq", "cmhi", "bsl", "bif", "bit", "movi", "and", "orr", "mov", "ins", "umov"):
            return self.neon(mn, ops, pc)
        raise RuntimeError("unhandled %x: %s %s" % (pc, mn, op))

    def lanes(self, r):
        m = re.match(r"v(\d+)\.(\d+)([bhsd])", r)
        i, n, k = int(m.group(1)), int(m.group(2)), m.group(3)
        bits = {"b": 8, "h": 16, "s": 32, "d": 64}[k]
        return i, n, bits

    def neon(self, mn, ops, pc):
        if mn == "dup":
            i, n, bits = self.lanes(ops[0])
            src = ops[1]
            if src[0] in "wx":
                e, _ = self.reg(src)
            else:
                m = re.match(r"v(\d+)\.([bhsd])\[(\d+)\]", src)
                j, idx = int(m.group(1)), int(m.group(3))
                e = (self.v[j] >> (idx * bits)) & ((1 << bits) - 1)
            v = 0
            for k in range(n):
                v |= (e & ((1 << bits) - 1)) << (k * bits)
            self.v[i] = v
            return None
        if mn in ("cmgt", "cmeq", "cmhi"):
            i, n, bits = self.lanes(ops[0])
            j, _, _ = self.lanes(ops[1])
            k2, _, _ = self.lanes(ops[2])
            m = (1 << bits) - 1
            v = 0
            for k in range(n):
                a = (self.v[j] >> (k * bits)) & m
                b = (self.v[k2] >> (k * bits)) & m
                if mn == "cmgt":
                    sa = a - (1 << bits) if a >> (bits - 1) else a
                    sb = b - (1 << bits) if b >> (bits - 1) else b
                    r = sa > sb
                elif mn == "cmhi":
                    r = a > b
                else:
                    r = a == b
                if r:
                    v |= m << (k * bits)
            self.v[i] = v
            return None
        if mn in ("bsl", "bif", "bit"):
            i, n, bits = self.lanes(ops[0])
            j, _, _ = self.lanes(ops[1])
            k2, _, _ = self.lanes(ops[2])
            full = (1 << (n * bits)) - 1
            d, a, b = self.v[i], self.v[j], self.v[k2]
            if mn == "bsl":
                r = (a & d) | (b & ~d)
            elif mn == "bif":
                r = (d & b) | (a & ~b)
            else:
                r = (a & b) | (d & ~b)
            self.v[i] = r & full
            return None
        if mn == "movi":
            if re.match(r"[ds]\d+$", ops[0]):
                self.v[int(ops[0][1:])] = self.imm(ops[1].split()[0]) if not ops[1].startswith("#0000") else 0
                return None
            i, n, bits = self.lanes(ops[0])
            imm = int(ops[1].lstrip("#"), 16) if ops[1].startswith("#0000") else self.imm(ops[1])
            if len(ops) > 2:
                imm <<= int(ops[2].split("#")[1], 0)
            v = 0
            for k in range(n):
                v |= (imm & ((1 << bits) - 1)) << (k * bits)
            self.v[i] = v
            return None
        if mn in ("add", "orr", "and", "cmhs"):
            i, n, bits = self.lanes(ops[0])
            j, _, _ = self.lanes(ops[1])
            k2, _, _ = self.lanes(ops[2])
            m = (1 << bits) - 1
            v = 0
            for k in range(n):
                a = (self.v[j] >> (k * bits)) & m
                b = (self.v[k2] >> (k * bits)) & m
                r = {"add": (a + b) & m, "orr": a | b, "and": a & b, "cmhs": m if a >= b else 0}[mn]
                v |= r << (k * bits)
            self.v[i] = v
            return None
        raise RuntimeError("unhandled neon %x: %s %s" % (pc, mn, ops))


def memcpy(cpu):
    """A native memcpy(x0, x1, x2) for --native."""
    d, s, n = cpu.x[0], cpu.x[1], cpu.x[2]
    for i in range(n):
        cpu.mem.write(d + i, 1, cpu.mem.read(s + i, 1)[0])


def memset(cpu):
    """A native memset(x0, w1, x2) for --native."""
    for i in range(cpu.x[2]):
        cpu.mem.write(cpu.x[0] + i, 1, cpu.x[1] & 0xFF)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("nso")
    ap.add_argument("entry")
    ap.add_argument("--reg", action="append", default=[], help="xN=value")
    ap.add_argument("--mem", action="append", default=[], help="addr=u64 (8-byte store before the run)")
    ap.add_argument("--mem32", action="append", default=[], help="addr=u32")
    ap.add_argument("--dump", help="addr:len, write those bytes after the run to --out")
    ap.add_argument("--out")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--follow", action="store_true", help="run into calls instead of returning 0 from them")
    ap.add_argument("--stub", action="append", default=[], help="addr=x0: a call to addr returns x0 without running")
    ap.add_argument("--str", action="append", default=[], help="addr=text: a NUL-terminated string in memory")
    a = ap.parse_args()
    nso = Nso(a.nso)
    mem = Mem(nso)
    cpu = Cpu(nso, mem)
    cpu.follow = a.follow
    for s in a.stub:
        k, v = s.split("=")
        cpu.stubs[int(k, 0)] = int(v, 0)
    for s in a.str:
        k, v = s.split("=", 1)
        b = v.encode() + b"\0"
        for i, ch in enumerate(b):
            mem.b[int(k, 0) + i] = ch
    cpu.x[31] = 0x7FFF_0000_0000
    for r in a.reg:
        k, v = r.split("=")
        cpu.setreg(k, int(v, 0))
    for m in a.mem:
        k, v = m.split("=")
        mem.write(int(k, 0), 8, int(v, 0))
    for m in a.mem32:
        k, v = m.split("=")
        mem.write(int(k, 0), 4, int(v, 0))
    x0 = cpu.run(int(a.entry, 16))
    print("x0 %#x  steps %d  stores %d  calls %s" % (x0, cpu.steps, len(cpu.stores),
                                              {hex(k): v for k, v in cpu.calls.items()}), file=sys.stderr)
    for pc, t in sorted(set(cpu.imports)):
        print("import call at %08x to PLT %08x (returned its stub or 0)" % (pc, t), file=sys.stderr)
    for pc in sorted(set(cpu.null_calls)):
        print("call through a null pointer at %08x (returned 0)" % pc, file=sys.stderr)
    for pc in sorted(set(cpu.tainted_branches)):
        print("input-dependent branch at %08x" % pc, file=sys.stderr)
    if a.dump:
        base, n = (int(x, 0) for x in a.dump.split(":"))
        data = bytes(mem.b.get(base + i, 0) for i in range(n))
        open(a.out, "wb").write(data)


if __name__ == "__main__":
    main()
