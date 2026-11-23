"""SM86 instruction words for the known-answer programs.

Field positions follow the decoder in ../src/decode (and Mesa NAK's SM70+
encoder). Every word is a Python int of 128 bits. The scheduling bits are
set by `sched` when a program is laid out, not here.
"""

RZ = 255
PT = 7


def setf(w, lo, ln, v):
    m = ((1 << ln) - 1) << lo
    return (w & ~m) | ((v << lo) & m)


def W(op, form, fields=(), guard=PT, gnot=0):
    w = op | (form << 9) | (guard << 12) | (gnot << 15)
    for lo, ln, v in fields:
        w = setf(w, lo, ln, v)
    return w


def cbuf(bank, off):
    """A c[bank][off] operand in bits 32..63 (offset in bytes, a multiple of 4)."""
    return (bank << 22) | ((off >> 2) << 8)


# Scheduling: stall cycles 105..108, yield 109, write barrier 110..112, read
# barrier 113..115, wait mask 116..121. Every instruction stalls the
# maximum and waits for every barrier; variable-latency instructions set
# barrier 0. Slow, but never wrong.
VARLAT = {0x100, 0x101, 0x104, 0x105, 0x106, 0x107, 0x108, 0x109, 0x110, 0x111, 0x112, 0x119, 0x181}


def sched(w):
    w = setf(w, 105, 4, 15)
    w = setf(w, 109, 1, 0)
    var = (w & 0x1ff) in VARLAT
    w = setf(w, 110, 3, 0 if var else 7)
    w = setf(w, 113, 3, 7)
    w = setf(w, 116, 6, 0x3f)
    w = setf(w, 122, 4, 0)
    return w


# --- wrapper ---------------------------------------------------------------

def s2r(dst, sr):
    return W(0x119, 4, [(16, 8, dst), (72, 8, sr)])


def uldc64(udst, off):
    return W(0x0b9, 5, [(16, 6, udst), (32, 32, cbuf(0, off)), (73, 3, 5)])


def mov_imm(dst, v):
    return W(0x002, 4, [(16, 8, dst), (32, 32, v), (72, 4, 15)])


def imad_rcr(dst, a, cb_off, c):
    """IMAD dst, a, c[0][cb_off], c"""
    return W(0x024, 5, [(16, 8, dst), (24, 8, a), (32, 32, cbuf(0, cb_off)), (64, 8, c), (73, 1, 1),
                        (81, 3, PT), (87, 3, PT), (90, 1, 1)])


def imad_wide_rrc(dst, a, b, cb_off):
    """IMAD.WIDE dst, a, b, c[0][cb_off]"""
    return W(0x025, 3, [(16, 8, dst), (24, 8, a), (64, 8, b), (32, 32, cbuf(0, cb_off)), (73, 1, 1),
                        (81, 3, PT), (87, 3, PT), (90, 1, 1)])


def ldg(dst, addr, off=0, ureg=4):
    """LDG.E dst, [addr.64+off] (32 bits, memory descriptor in ureg)"""
    return W(0x181, 4, [(16, 8, dst), (24, 8, addr), (32, 6, ureg), (40, 24, off), (72, 1, 1), (73, 3, 4),
                        (76, 1, 1), (81, 3, PT), (84, 3, 1), (90, 1, 1), (91, 1, 1)])


def stg(addr, off, data, ureg=4):
    """STG.E [addr.64+off], data"""
    return W(0x186, 4, [(24, 8, addr), (32, 8, data), (40, 24, off), (64, 6, ureg), (72, 1, 1), (73, 3, 4),
                        (76, 1, 1), (84, 3, 1), (90, 1, 1), (91, 1, 1)])


def exit_():
    return W(0x14d, 4, [(87, 3, PT)])


def bra_self():
    return W(0x147, 4, [(32, 32, 0xfffffff0), (64, 8, RZ), (72, 10, 0x3ff), (87, 3, PT)])


SR_TID_X = 0x21
SR_CTAID_X = 0x25
NIN = 4    # input words per vector, in R4..R7
NOUT = 16  # output words per vector


def wrapper(body, outs):
    """The kernel: R4..R7 = in[4i..4i+3], body, out[16i+k] = outs[k]."""
    assert len(outs) <= NOUT
    pre = [
        s2r(0, SR_TID_X),
        s2r(1, SR_CTAID_X),
        uldc64(4, 0x118),
        mov_imm(10, 4 * NIN),
        mov_imm(11, 4 * NOUT),
        imad_rcr(0, 1, 0x0, 0),
        imad_wide_rrc(2, 0, 10, 0x160),
        imad_wide_rrc(8, 0, 11, 0x168),
    ] + [ldg(4 + k, 2, 4 * k) for k in range(NIN)]
    post = [stg(8, 4 * k, r) for k, r in enumerate(outs)] + [exit_(), bra_self()]
    return [sched(w) for w in pre + list(body) + post]


def words_bytes(ws):
    return b''.join(w.to_bytes(16, 'little') for w in ws)


# --- float ALU -------------------------------------------------------------

RND = {'RN': 0, 'RM': 1, 'RP': 2, 'RZ': 3}


def fadd(dst, a, b, rnd=0, ftz=0, sat=0, nega=0, negb=0, absa=0, absb=0):
    # Form 1 takes the second source from the R32 slot (modifiers 63/62).
    return W(0x021, 1, [(16, 8, dst), (24, 8, a), (32, 8, b), (78, 2, rnd), (80, 1, ftz), (77, 1, sat),
                        (72, 1, nega), (73, 1, absa), (63, 1, negb), (62, 1, absb)])


def fmul(dst, a, b, rnd=0, ftz=0, sat=0, scale=4, nega=0, negb=0):
    return W(0x020, 1, [(16, 8, dst), (24, 8, a), (32, 8, b), (78, 2, rnd), (80, 1, ftz), (77, 1, sat),
                        (84, 3, scale), (72, 1, nega), (63, 1, negb)])


def fmul_imm(dst, a, imm, rnd=0, ftz=0, sat=0, scale=4, nega=0):
    return W(0x020, 4, [(16, 8, dst), (24, 8, a), (32, 32, imm), (78, 2, rnd), (80, 1, ftz), (77, 1, sat),
                        (84, 3, scale), (72, 1, nega)])


def ffma(dst, a, b, c, rnd=0, ftz=0, sat=0, nega=0, negb=0, negc=0):
    return W(0x023, 1, [(16, 8, dst), (24, 8, a), (32, 8, b), (64, 8, c), (78, 2, rnd), (80, 1, ftz),
                        (77, 1, sat), (72, 1, nega), (63, 1, negb), (75, 1, negc)])


def fmnmx(dst, a, b, p=PT, pnot=0, ftz=0):
    return W(0x009, 1, [(16, 8, dst), (24, 8, a), (32, 8, b), (87, 3, p), (90, 1, pnot), (80, 1, ftz)])


FCMP = ['F', 'LT', 'EQ', 'LE', 'GT', 'NE', 'GE', 'NUM', 'NAN', 'LTU', 'EQU', 'LEU', 'GTU', 'NEU', 'GEU', 'T']


def fsetp(pd, pq, cmp, a, b, ftz=0, bop=0, ps=PT, psnot=0):
    return W(0x00b, 1, [(81, 3, pd), (84, 3, pq), (76, 4, FCMP.index(cmp)), (80, 1, ftz), (74, 2, bop),
                        (24, 8, a), (32, 8, b), (87, 3, ps), (90, 1, psnot)])


def p2r(dst, mask=0x7f):
    return W(0x003, 4, [(16, 8, dst), (24, 8, RZ), (32, 32, mask)])


# --- conversions -----------------------------------------------------------

def f2i(dst, src, signed=1, size=2, rnd=0, ntz=0, ftz=0, sty=2, sel=0):
    op = 0x111 if sty == 3 else 0x105
    return W(op, 1, [(16, 8, dst), (32, 8, src), (72, 1, signed), (75, 2, size), (78, 2, rnd), (77, 1, ntz),
                     (80, 1, ftz), (84, 3, sty), (60, 2, sel)])


def i2f(dst, src, signed=1, size=2, rnd=0, dty=2, sel=0):
    op = 0x112 if size == 3 or dty == 3 else 0x106
    return W(op, 1, [(16, 8, dst), (32, 8, src), (74, 1, signed), (84, 2, size), (78, 2, rnd), (75, 3, dty),
                     (60, 2, sel)])


def i2fp(dst, src, signed=1, rnd=0):
    return W(0x045, 1, [(16, 8, dst), (32, 8, src), (74, 1, signed), (75, 3, 2), (84, 2, 2), (78, 2, rnd)])


def f2f(dst, src, dty, sty, rnd=0, ftz=0, sel=0):
    op = 0x110 if 3 in (dty, sty) else 0x104
    return W(op, 1, [(16, 8, dst), (32, 8, src), (75, 3, dty), (84, 3, sty), (78, 2, rnd), (80, 1, ftz),
                     (60, 2, sel)])


def frnd(dst, src, ty=2, rnd=0, ftz=0, sel=0):
    return W(0x107, 1, [(16, 8, dst), (32, 8, src), (75, 3, ty), (84, 3, ty), (78, 2, rnd), (80, 1, ftz),
                        (60, 2, sel)])


def f2fp(dst, a, b, relu=0, bf16=0, sat=0, rz=0):
    return W(0x03e, 1, [(16, 8, dst), (24, 8, a), (32, 8, b), (64, 8, RZ), (75, 1, relu), (76, 1, bf16),
                        (77, 1, sat), (79, 2, 3 if rz else 0)])


# --- half precision --------------------------------------------------------

def hop(op, dst, a, b, c=RZ, swa=0, swb=0, swc=0, nega=0, negb=0, negc=0, absa=0, absb=0, ftz=0, sat=0,
        f32=0, fmz=0, relu=0, bf=0):
    """HADD2 0x030, HMUL2 0x032, HFMA2 0x031 in form 1 (A R24, B R32, C R64)."""
    f = [(16, 8, dst), (24, 8, a), (32, 8, b), (74, 2, swa), (60, 2, swb & 3), (86, 1, swb >> 2),
         (72, 1, nega), (63, 1, negb), (73, 1, absa), (62, 1, absb), (80, 1, ftz), (77, 1, sat), (78, 1, f32),
         (85, 1, bf)]
    if op == 0x032:
        f += [(76, 1, fmz)]
    if op == 0x031:
        f += [(64, 8, c), (81, 2, swc), (84, 1, negc), (76, 1, fmz), (79, 1, relu), (87, 3, PT)]
    return W(op, 1, f)


def hmnmx2(dst, a, b, p=PT, pnot=0, ftz=0, nan=0):
    return W(0x040, 1, [(16, 8, dst), (24, 8, a), (32, 8, b), (87, 3, p), (90, 1, pnot), (81, 1, nan),
                        (80, 1, ftz)])


# --- integer ---------------------------------------------------------------

def iadd3(dst, a, b, c, p0=PT, p1=PT, neg=(0, 0, 0), x=0, cin0=(PT, 0), cin1=(PT, 1)):
    f = [(16, 8, dst), (24, 8, a), (32, 8, b), (64, 8, c), (81, 3, p0), (84, 3, p1), (72, 1, neg[0]),
         (63, 1, neg[1]), (75, 1, neg[2]), (74, 1, x)]
    if x:
        f += [(87, 3, cin0[0]), (90, 1, cin0[1]), (77, 3, cin1[0]), (80, 1, cin1[1])]
    return W(0x010, 1, f)


def imad(kind, dst, a, b, c, p=PT, signed=0, x=0, cin=(PT, 0), cneg=0):
    op = {'lo': 0x024, 'wide': 0x025, 'hi': 0x027}[kind]
    f = [(16, 8, dst), (24, 8, a), (32, 8, b), (64, 8, c), (81, 3, p), (73, 1, signed), (74, 1, x),
         (75, 1, cneg)]
    f += [(87, 3, cin[0]), (90, 1, cin[1])] if x else [(87, 3, PT), (90, 1, 1)]
    return W(op, 1, f)


def lea(dst, a, b, sh, c=RZ, hi=0, x=0, sx=0, pd=PT, ps=(PT, 0), nega=0):
    return W(0x011, 1, [(16, 8, dst), (24, 8, a), (32, 8, b), (64, 8, c), (75, 5, sh), (80, 1, hi), (74, 1, x),
                        (73, 1, sx), (81, 3, pd), (87, 3, ps[0]), (90, 1, ps[1]), (72, 1, nega)])


def shf(dst, a, s, c, left=0, wrap=0, ty=3, hi=0):
    """SHF.{L,R}[.W].{S64,U64,S32,U32}[.HI] dst, a, s, c"""
    return W(0x019, 1, [(16, 8, dst), (24, 8, a), (32, 8, s), (64, 8, c), (76, 1, 0 if left else 1),
                        (75, 1, wrap), (73, 2, ty), (80, 1, hi)])


def isetp(pd, pq, cmp, a, b, signed=1, ex=0, ep=(PT, 0), bop=0):
    c = ['F', 'LT', 'EQ', 'LE', 'GT', 'NE', 'GE', 'T'].index(cmp)
    return W(0x00c, 1, [(81, 3, pd), (84, 3, pq), (76, 3, c), (73, 1, signed), (74, 2, bop), (72, 1, ex),
                        (24, 8, a), (32, 8, b), (87, 3, PT), (90, 1, 0), (68, 3, ep[0]), (71, 1, ep[1])])


def flo(dst, src, signed=0, sh=0, inv=0):
    return W(0x100, 1, [(16, 8, dst), (32, 8, src), (73, 1, signed), (74, 1, sh), (63, 1, inv), (81, 3, PT)])


def popc(dst, src, inv=0):
    return W(0x109, 1, [(16, 8, dst), (32, 8, src), (63, 1, inv)])


def brev(dst, src):
    return W(0x101, 1, [(16, 8, dst), (32, 8, src)])


def prmt(dst, a, sel, c, mode=0):
    return W(0x016, 1, [(16, 8, dst), (24, 8, a), (32, 8, sel), (64, 8, c), (72, 3, mode)])


def imnmx(dst, a, b, signed=1, p=PT, pnot=0):
    return W(0x017, 1, [(16, 8, dst), (24, 8, a), (32, 8, b), (73, 1, signed), (87, 3, p), (90, 1, pnot)])


def iabs(dst, src):
    return W(0x013, 1, [(16, 8, dst), (32, 8, src)])


# --- MUFU ------------------------------------------------------------------

MUFU = ['COS', 'SIN', 'EX2', 'LG2', 'RCP', 'RSQ', 'RCP64H', 'RSQ64H', 'SQRT', 'TANH']


def mufu(dst, src, fn, f16=0, h1=0):
    return W(0x108, 1, [(16, 8, dst), (32, 8, src), (74, 6, MUFU.index(fn)), (73, 1, f16), (60, 1, h1)])
