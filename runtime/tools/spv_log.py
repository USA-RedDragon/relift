#!/usr/bin/env python3
"""Value log of one invocation: rewrites a compute SPIR-V module so that one chosen invocation writes every scalar it
computes, in order, to a log buffer, for comparing a GPU run with the SPIR-V interpreter's instruction by instruction.

  python3 -I spv_log.py IN.spv OUT.spv OUT.map.json [--cap RECORDS]

Every instruction in every function whose result is a boolean, integer or float scalar or vector (up to 4
components) is followed by a record of two 32-bit words: the result id (bits 0-23) with the component (bits 24-31),
and the value (32-bit values as they are, 16-bit floats as their bits in the low half, 16-bit integers widened,
64-bit floats converted to single precision, booleans as 0 or 1). Records go to the storage buffer at set 3, binding 7 (the translator's trace binding;
krun's `trace` line and wgrun's `--buffer 3,7=` give it), from word 0, up to --cap records (default 1,000,000);
later ones land in one trash record past the cap. Only the invocation whose LocalInvocationIndex and WorkgroupId
equal the specialization constants 0x7000 (index) and 0x7001 to 0x7003 (x, y, z) writes; every other one writes the
trash record.

The map lists each recorded id with its opcode and, for a module translated with debug info (xlate --debug-info),
the guest instruction it belongs to: the PC and the disassembly line of the last DebugLine before it. The
NonSemantic debug instructions are removed from the output, so a device needs no extension for them.

Standard library only.
"""

import json
import struct
import sys

OP_NAMES = {
    12: "ExtInst", 57: "FunctionCall", 61: "Load", 65: "AccessChain", 79: "VectorShuffle", 80: "CompositeConstruct",
    81: "CompositeExtract", 82: "CompositeInsert", 95: "ImageFetch", 98: "ImageRead", 103: "ImageQuerySizeLod",
    109: "ConvertFToU", 110: "ConvertFToS", 111: "ConvertSToF", 112: "ConvertUToF", 113: "UConvert",
    114: "SConvert", 115: "FConvert", 124: "Bitcast", 126: "SNegate", 127: "FNegate", 128: "IAdd", 129: "FAdd",
    130: "ISub", 131: "FSub", 132: "IMul", 133: "FMul", 134: "UDiv", 135: "SDiv", 136: "FDiv", 137: "UMod",
    149: "IAddCarry", 151: "UMulExtended", 156: "IsNan", 157: "IsInf", 164: "LogicalEqual", 166: "LogicalOr",
    167: "LogicalAnd", 168: "LogicalNot", 169: "Select", 170: "IEqual", 171: "INotEqual", 172: "UGreaterThan",
    173: "SGreaterThan", 176: "ULessThan", 177: "SLessThan", 194: "ShiftRightLogical", 195: "ShiftRightArithmetic",
    196: "ShiftLeftLogical", 197: "BitwiseOr", 198: "BitwiseXor", 199: "BitwiseAnd", 200: "Not",
    201: "BitFieldInsert", 202: "BitFieldSExtract", 203: "BitFieldUExtract", 245: "Phi", 345: "GroupNonUniformShuffle",
    4446: "FmaKHR", 227: "AtomicLoad", 234: "AtomicIAdd",
}

SPEC_INDEX = 0x7000


class Module:
    def __init__(self, words):
        assert words[0] == 0x07230203, "not SPIR-V"
        self.header = list(words[:5])
        self.insts = []
        i = 5
        while i < len(words):
            n = words[i] >> 16
            self.insts.append([words[i] & 0xFFFF] + list(words[i + 1:i + n]))
            i += n
        self.bound = words[3]

    def new_id(self):
        self.bound += 1
        return self.bound - 1

    def words(self):
        out = list(self.header)
        out[3] = self.bound
        for ins in self.insts:
            out.append((len(ins) << 16) | ins[0])
            out.extend(ins[1:])
        return out


def string_of(ws):
    b = b"".join(struct.pack("<I", w) for w in ws)
    return b.split(b"\0", 1)[0].decode("utf-8", "replace")


def has_result(op):
    """(has type, has result) of the opcodes that matter here."""
    no = {3, 5, 6, 7, 10, 11, 14, 15, 16, 17, 56, 62, 63, 71, 72, 99, 224, 225, 228, 246, 247, 249, 250, 251, 252,
          253, 254, 255, 4416}
    if op in no:
        return False
    return True


def instrument(words, cap=1_000_000):
    m = Module(words)
    insts = m.insts
    types = {}
    consts = {}
    deco = {}
    nonsem = set()
    strings = {}
    for ins in insts:
        op = ins[0]
        if op == 11 and string_of(ins[2:]).startswith("NonSemantic."):
            nonsem.add(ins[1])
        elif op == 7:
            strings[ins[1]] = string_of(ins[2:])
        elif 19 <= op <= 39 or op == 4456:
            types[ins[1]] = ins
        elif op in (43, 50):
            consts[ins[2]] = (ins[1], ins[3:])
        elif op == 71:
            deco.setdefault(ins[1], []).append(ins[2:])

    def find_type(pred):
        for tid, t in types.items():
            if pred(t):
                return tid
        return None

    new_types = []

    def get_type(ins_tail, opcode):
        tid = find_type(lambda t: t[0] == opcode and t[2:] == ins_tail)
        if tid is None:
            tid = m.new_id()
            t = [opcode, tid] + ins_tail
            types[tid] = t
            new_types.append(t)
        return tid

    t_uint = get_type([32, 0], 21)
    t_bool = get_type([], 20)
    t_f32 = get_type([32], 22)
    t_v3 = get_type([t_uint, 3], 23)
    t_void = get_type([], 19)
    del t_void

    new_vals = []
    const_cache = {}

    def c_uint(v):
        if v in const_cache:
            return const_cache[v]
        for cid, (ty, val) in consts.items():
            if ty == t_uint and val == [v]:
                pass
        cid = m.new_id()
        new_vals.append([43, t_uint, cid, v])
        const_cache[v] = cid
        return cid

    def ptr(cls, pointee):
        return get_type([cls, pointee], 32)

    # Log buffer: struct { uint data[]; } at set 3 binding 7.
    rta = m.new_id()
    new_types.append([29, rta, t_uint])
    blk = m.new_id()
    new_types.append([30, blk, rta])
    p_blk = m.new_id()
    new_types.append([32, p_blk, 12, blk])
    p_sb_uint = ptr(12, t_uint)
    p_priv_uint = ptr(6, t_uint)
    p_priv_bool = ptr(6, t_bool)
    p_in_uint = ptr(1, t_uint)
    p_in_v3 = ptr(1, t_v3)
    logvar = m.new_id()
    counter = m.new_id()
    selvar = m.new_id()
    zero = c_uint(0)
    one = c_uint(1)
    capc = c_uint(cap)
    specs = []
    for k in range(4):
        sid = m.new_id()
        new_vals.append([50, t_uint, sid, 0xFFFFFFFF])
        specs.append(sid)
    new_vals.append([59, p_blk, logvar, 12])
    new_vals.append([59, p_priv_uint, counter, 6, zero])
    new_vals.append([59, p_priv_bool, selvar, 6])
    new_deco = [[71, rta, 6, 4], [71, blk, 2], [72, blk, 0, 35, 0], [71, logvar, 34, 3], [71, logvar, 33, 7]]
    for k, sid in enumerate(specs):
        new_deco.append([71, sid, 1, SPEC_INDEX + k])
    # Types of the existing built-in variables (the module's own may be declared differently: keep them).
    var_type = {ins[2]: ins[1] for ins in insts if ins[0] == 59}
    # Built-ins: LocalInvocationIndex (29) and WorkgroupId (26), reused when the module has them.
    builtin = {}
    for vid, ds in deco.items():
        for d in ds:
            if d[0] == 11:
                builtin[d[1]] = vid
    globals_in = []
    if 29 not in builtin:
        v = m.new_id()
        new_vals.append([59, p_in_uint, v, 1])
        var_type[v] = p_in_uint
        new_deco.append([71, v, 11, 29])
        builtin[29] = v
        globals_in.append(v)
    if 26 not in builtin:
        v = m.new_id()
        new_vals.append([59, p_in_v3, v, 1])
        var_type[v] = p_in_v3
        new_deco.append([71, v, 11, 26])
        builtin[26] = v
        globals_in.append(v)

    def pointee(pid):
        return types[pid][3]

    # The map from ids to guest instructions, from DebugLine.
    source = None
    for sid, s in strings.items():
        if s.startswith("/*0000*/") or "\n/*" in s:
            source = s
    lines = source.splitlines() if source else []

    out = []
    idmap = {}
    cur_line = None
    in_func = False
    entry_fn = None
    for ins in insts:
        if ins[0] == 15:
            entry_fn = ins[2]
    first_block_of = None
    pending_phis = []

    def conv(val, ty, acc):
        """A uint id holding value val of scalar type ty; appends instructions to acc."""
        t = types[ty]
        r = m.new_id()
        if t[0] == 20:
            acc.append([169, t_uint, r, val, one, zero])
        elif t[0] == 21:
            w = t[2]
            if w == 32:
                acc.append([124, t_uint, r, val])
            else:
                acc.append([113, t_uint, r, val])
        elif t[0] == 22:
            if t[2] == 32:
                acc.append([124, t_uint, r, val])
            elif t[2] == 16:
                # The half's bits: a pair (x, 0) as one word, with no conversion a driver could fold away.
                v2 = get_type([ty, 2], 23)
                zh = m.new_id()
                new_vals.append([46, ty, zh])
                pr = m.new_id()
                acc.append([80, v2, pr, val, zh])
                acc.append([124, t_uint, r, pr])
            else:
                f = m.new_id()
                acc.append([115, t_f32, f, val])
                acc.append([124, t_uint, r, f])
        else:
            return None
        return r

    def record(rid, ty, opcode, acc):
        t = types.get(ty)
        if t is None:
            return
        comps = []
        if t[0] in (20, 21, 22):
            comps = [(0, rid, ty)]
        elif t[0] == 23 and t[3] <= 4 and types.get(t[2], [0])[0] in (20, 21, 22):
            for k in range(t[3]):
                e = m.new_id()
                acc.append([81, t[2], e, rid, k])
                comps.append((k, e, t[2]))
        else:
            return
        for k, v, vty in comps:
            u = conv(v, vty, acc)
            if u is None:
                return
            c = m.new_id()
            acc.append([61, t_uint, c, counter])
            sel = m.new_id()
            acc.append([61, t_bool, sel, selvar])
            inb = m.new_id()
            acc.append([176, t_bool, inb, c, capc])
            ok = m.new_id()
            acc.append([167, t_bool, ok, sel, inb])
            slot = m.new_id()
            acc.append([169, t_uint, slot, ok, c, capc])
            w0 = m.new_id()
            acc.append([132, t_uint, w0, slot, c_uint(2)])
            w1 = m.new_id()
            acc.append([128, t_uint, w1, w0, one])
            p0 = m.new_id()
            acc.append([65, p_sb_uint, p0, logvar, zero, w0])
            acc.append([62, p0, c_uint(rid | (k << 24))])
            p1 = m.new_id()
            acc.append([65, p_sb_uint, p1, logvar, zero, w1])
            acc.append([62, p1, u])
            inc = m.new_id()
            acc.append([169, t_uint, inc, ok, one, zero])
            c2 = m.new_id()
            acc.append([128, t_uint, c2, c, inc])
            acc.append([62, counter, c2])
        idmap[rid] = {"op": OP_NAMES.get(opcode, str(opcode)), "line": cur_line}

    def sel_code(acc):
        li = m.new_id()
        lt = pointee(var_type[builtin[29]])
        acc.append([61, lt, li, builtin[29]])
        if lt != t_uint:
            li2 = m.new_id()
            acc.append([124, t_uint, li2, li])
            li = li2
        wg = m.new_id()
        wt = pointee(var_type[builtin[26]])
        acc.append([61, wt, wg, builtin[26]])
        conds = []
        e = m.new_id()
        acc.append([170, t_bool, e, li, specs[0]])
        conds.append(e)
        for k in range(3):
            x = m.new_id()
            acc.append([81, types[wt][2], x, wg, k])
            if types[wt][2] != t_uint:
                x2 = m.new_id()
                acc.append([124, t_uint, x2, x])
                x = x2
            e = m.new_id()
            acc.append([170, t_bool, e, x, specs[k + 1]])
            conds.append(e)
        a = conds[0]
        for cnd in conds[1:]:
            n = m.new_id()
            acc.append([167, t_bool, n, a, cnd])
            a = n
        acc.append([62, selvar, a])

    first_fn_seen = False
    globals_at = 0
    state = None  # None, "vars" (entry block, before the first non-variable), "body"
    fn_is_entry = False
    for ins in insts:
        op = ins[0]
        if op == 12 and ins[3] in nonsem:
            if ins[4] == 103 and len(ins) > 6:
                ln = consts.get(ins[6])
                cur_line = ln[1][0] if ln else None
            continue
        if op == 11 and ins[1] in nonsem:
            continue
        if op == 54:
            if not first_fn_seen:
                first_fn_seen = True
                globals_at = len(out)
            in_func = True
            fn_is_entry = ins[2] == entry_fn
            state = "start"
            out.append(ins)
            continue
        if not in_func:
            if op == 15:
                ins = ins + [logvar, counter, selvar] + globals_in
            if op == 71 and not any(isinstance(x, list) for x in ins):
                pass
            out.append(ins)
            if op == 14:
                pass
            continue
        if op == 56:
            in_func = False
            out.append(ins)
            continue
        if op == 248:
            out.append(ins)
            if state == "start":
                state = "vars"
            pending_phis = []
            continue
        if state == "vars" and op == 59:
            out.append(ins)
            continue
        if state == "vars":
            state = "body"
            if fn_is_entry:
                acc = []
                sel_code(acc)
                out.extend(acc)
        if op == 245:
            out.append(ins)
            pending_phis.append(ins)
            continue
        if pending_phis:
            acc = []
            for ph in pending_phis:
                record(ph[2], ph[1], 245, acc)
            out.extend(acc)
            pending_phis = []
        out.append(ins)
        if op in (59, 55) or not has_result(op) or len(ins) < 3:
            continue
        if op in (246, 247):
            continue
        acc = []
        record(ins[2], ins[1], op, acc)
        out.extend(acc)
    # Types, constants and variables made here (constants also while recording) go before the first function.
    out[globals_at:globals_at] = new_types + new_vals
    # Decorations go before the first type: after the last OpDecorate/OpMemberDecorate/OpDecorationGroup.
    last = max(i for i, x in enumerate(out) if x[0] in (71, 72, 73, 74, 75, 332, 5632, 5633) or x[0] == 5) + 1
    out[last:last] = new_deco
    # Drop names of removed debug strings? OpString stays (harmless). OpExtension SPV_KHR_non_semantic_info too.
    out = [x for x in out if not (x[0] == 10 and string_of(x[1:]) == "SPV_KHR_non_semantic_info")]
    m.insts = out
    info = {"ids": {}, "source": None}
    for rid, v in idmap.items():
        ln = v["line"]
        e = {"op": v["op"]}
        if ln:
            e["pc"] = 16 * (ln - 1)
            if 0 < ln <= len(lines):
                e["sass"] = lines[ln - 1].strip()
        info["ids"][rid] = e
    return m.words(), info


def main():
    args = sys.argv[1:]
    cap = 1_000_000
    if "--cap" in args:
        k = args.index("--cap")
        cap = int(args[k + 1])
        del args[k:k + 2]
    src, dst, mp = args
    with open(src, "rb") as f:
        b = f.read()
    words = list(struct.unpack("<%dI" % (len(b) // 4), b))
    out, info = instrument(words, cap)
    with open(dst, "wb") as f:
        f.write(struct.pack("<%dI" % len(out), *out))
    with open(mp, "w") as f:
        json.dump(info, f)
    print("recorded ids %d" % len(info["ids"]))


if __name__ == "__main__":
    main()
