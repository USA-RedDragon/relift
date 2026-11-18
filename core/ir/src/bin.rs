//! A compact binary form of a [`Block`]. The emitter's reuse code
//! (`core/emit/src/reuse.rs`) writes it as part of the input it hashes into a
//! function's emit-cache key; no IR is stored. Integers are LEB128; the
//! layout is private to this crate's version ([`FORMAT`]).

use crate::{
    AtomicOp, BinOp, Block, CmpOp, CryptoOp, Endian, Exit, ExtKind, Flow, FpArgs, FpOp, FpRound,
    Inst, IoKind, Konst, LaneKind, LaneOp, LaneWidth, Next, Op, Order, Permute, Reg, RegFile, UnOp,
    Value, Width,
};

/// The version of the byte layout of [`write_block`] and [`Reader`].
pub const FORMAT: u32 = 2;

/// A byte string that [`Reader`] could not take.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BinError(pub &'static str);

impl std::fmt::Display for BinError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.0)
    }
}

impl std::error::Error for BinError {}

type Res<T> = Result<T, BinError>;

fn bad<T>(what: &'static str) -> Res<T> {
    Err(BinError(what))
}

/// Appends `v` as LEB128.
pub fn put_uvar(out: &mut Vec<u8>, mut v: u64) {
    while v >= 0x80 {
        out.push((v as u8) | 0x80);
        v >>= 7;
    }
    out.push(v as u8);
}

/// Appends `s` as a length and its bytes.
pub fn put_str(out: &mut Vec<u8>, s: &str) {
    put_uvar(out, s.len() as u64);
    out.extend_from_slice(s.as_bytes());
}

/// A cursor over bytes written by the `put_` functions and [`write_block`].
pub struct Reader<'a> {
    buf: &'a [u8],
}

impl<'a> Reader<'a> {
    /// A reader at the start of `buf`.
    pub fn new(buf: &'a [u8]) -> Reader<'a> {
        Reader { buf }
    }

    /// Whether every byte has been read.
    pub fn is_empty(&self) -> bool {
        self.buf.is_empty()
    }

    /// The next byte.
    ///
    /// # Errors
    ///
    /// The bytes ended.
    pub fn u8(&mut self) -> Res<u8> {
        let (&b, rest) = self.buf.split_first().ok_or(BinError("truncated"))?;
        self.buf = rest;
        Ok(b)
    }

    /// The next `n` bytes.
    ///
    /// # Errors
    ///
    /// The bytes ended.
    pub fn bytes(&mut self, n: usize) -> Res<&'a [u8]> {
        if self.buf.len() < n {
            return bad("truncated");
        }
        let (h, t) = self.buf.split_at(n);
        self.buf = t;
        Ok(h)
    }

    /// The next LEB128 integer.
    ///
    /// # Errors
    ///
    /// The bytes ended or the integer is longer than 64 bits.
    pub fn uvar(&mut self) -> Res<u64> {
        let mut v = 0u64;
        let mut shift = 0;
        loop {
            let b = self.u8()?;
            if shift == 63 && b > 1 {
                return bad("integer too long");
            }
            v |= u64::from(b & 0x7f) << shift;
            if b & 0x80 == 0 {
                return Ok(v);
            }
            shift += 7;
        }
    }

    /// The next LEB128 integer, as a `u32`.
    ///
    /// # Errors
    ///
    /// As [`Reader::uvar`], or the integer does not fit.
    pub fn u32(&mut self) -> Res<u32> {
        u32::try_from(self.uvar()?).or_else(|_| bad("integer out of range"))
    }

    /// The next length or count, checked against the bytes left so that a
    /// bad count cannot ask for a huge allocation.
    ///
    /// # Errors
    ///
    /// As [`Reader::uvar`], or the count exceeds the bytes left.
    pub fn count(&mut self) -> Res<usize> {
        let n = self.uvar()?;
        if n > self.buf.len() as u64 {
            return bad("count past the end");
        }
        Ok(n as usize)
    }

    /// The next string written by [`put_str`].
    ///
    /// # Errors
    ///
    /// The bytes ended or are not UTF-8.
    pub fn str(&mut self) -> Res<&'a str> {
        let n = self.count()?;
        std::str::from_utf8(self.bytes(n)?).or_else(|_| bad("not UTF-8"))
    }
}

trait Coded: Copy + 'static {
    const ALL: &'static [Self];

    fn code(self) -> u8;

    fn decode(c: u8) -> Res<Self> {
        Self::ALL
            .get(usize::from(c))
            .copied()
            .ok_or(BinError("bad code"))
    }
}

macro_rules! coded {
    ($($t:ident: $($v:ident),+;)+) => {$(
        impl Coded for $t {
            const ALL: &'static [$t] = &[$($t::$v),+];

            fn code(self) -> u8 {
                self as u8
            }
        }
    )+};
}

coded! {
    UnOp: Not, Neg, Clz, Ctz, Popcount, BitReverse, ByteSwap;
    BinOp: Add, Sub, Mul, UDiv, SDiv, URem, SRem, And, Or, Xor, Shl, LShr, AShr, RotR;
    CmpOp: Eq, Ne, Ult, Ule, Slt, Sle;
    ExtKind: Zero, Sign;
    Endian: Little, Big;
    Order: Plain, Acquire, Release, AcqRel, SeqCst;
    FpRound: TiesEven, TiesAway, Floor, Ceil, Zero;
    IoKind: Any, Device;
    AtomicOp: Add, Clear, Xor, Or, SMax, SMin, UMax, UMin, Swap;
    LaneWidth: L8, L16, L32, L64;
    LaneKind: Add, Sub, AddSatS, AddSatU, SubSatS, SubSatU, MulLo, MulHiS, MulHiU, MulHiSU, And, Or, Xor, AndNot, Not, MinS, MinU, MaxS, MaxU, Abs, Neg, Eq, Ne, Slt, Sle, Ult, Ule, ShlImm, LShrImm, AShrImm, Shl, LShr, AShr, Select, WidenLoS, WidenLoU, WidenHiS, WidenHiU, NarrowTrunc, NarrowSS, NarrowUU, NarrowSU, MulWideLoSS, MulWideLoUU, MulWideLoSU, MulWideHiSS, MulWideHiUU, MulWideHiSU, Broadcast, Extract, Insert, Splat;
    CryptoOp: AesSubBytes, AesInvSubBytes, AesMixColumns, AesInvMixColumns, Sm4Sbox;
}

fn put_val(out: &mut Vec<u8>, v: Value) {
    put_uvar(out, u64::from(v.0));
}

fn put_eo(out: &mut Vec<u8>, e: Endian, o: Order) {
    out.push(e.code() | o.code() << 1);
}

fn fp_code(f: FpOp) -> (u8, u8) {
    match f {
        FpOp::Add => (0, 0),
        FpOp::Sub => (1, 0),
        FpOp::Mul => (2, 0),
        FpOp::Div => (3, 0),
        FpOp::Sqrt => (4, 0),
        FpOp::MulAdd => (5, 0),
        FpOp::Neg => (6, 0),
        FpOp::Abs => (7, 0),
        FpOp::RoundInt(r) => (8, r.code()),
        FpOp::ToSInt(r) => (9, r.code()),
        FpOp::ToUInt(r) => (10, r.code()),
        FpOp::FromSInt => (11, 0),
        FpOp::FromUInt => (12, 0),
        FpOp::Convert => (13, 0),
    }
}

fn fp_decode(c: u8, r: u8) -> Res<FpOp> {
    Ok(match c {
        0 => FpOp::Add,
        1 => FpOp::Sub,
        2 => FpOp::Mul,
        3 => FpOp::Div,
        4 => FpOp::Sqrt,
        5 => FpOp::MulAdd,
        6 => FpOp::Neg,
        7 => FpOp::Abs,
        8 => FpOp::RoundInt(FpRound::decode(r)?),
        9 => FpOp::ToSInt(FpRound::decode(r)?),
        10 => FpOp::ToUInt(FpRound::decode(r)?),
        11 => FpOp::FromSInt,
        12 => FpOp::FromUInt,
        13 => FpOp::Convert,
        _ => return bad("bad floating-point code"),
    })
}

fn put_op(out: &mut Vec<u8>, op: &Op) {
    match op {
        Op::Const(k) => {
            out.push(0);
            let v = k.get();
            put_uvar(out, v as u64);
            put_uvar(out, (v >> 64) as u64);
        }
        Op::Get(r) => {
            out.push(1);
            put_uvar(out, u64::from(r.0));
        }
        Op::Set(r, v) => {
            out.push(2);
            put_uvar(out, u64::from(r.0));
            put_val(out, *v);
        }
        Op::Un(u, v) => {
            out.extend_from_slice(&[3, u.code()]);
            put_val(out, *v);
        }
        Op::Bin(b, x, y) => {
            out.extend_from_slice(&[4, b.code()]);
            put_val(out, *x);
            put_val(out, *y);
        }
        Op::Cmp(c, x, y) => {
            out.extend_from_slice(&[5, c.code()]);
            put_val(out, *x);
            put_val(out, *y);
        }
        Op::Select(c, x, y) => {
            out.push(6);
            put_val(out, *c);
            put_val(out, *x);
            put_val(out, *y);
        }
        Op::Ext(k, v) => {
            out.extend_from_slice(&[7, k.code()]);
            put_val(out, *v);
        }
        Op::Extract(v, lo) => {
            out.extend_from_slice(&[8, *lo]);
            put_val(out, *v);
        }
        Op::Concat(h, l) => {
            out.push(9);
            put_val(out, *h);
            put_val(out, *l);
        }
        Op::Load {
            addr,
            endian,
            order,
        } => {
            out.push(10);
            put_eo(out, *endian, *order);
            put_val(out, *addr);
        }
        Op::Store {
            addr,
            value,
            endian,
            order,
        } => {
            out.push(11);
            put_eo(out, *endian, *order);
            put_val(out, *addr);
            put_val(out, *value);
        }
        Op::LoadExclusive {
            addr,
            endian,
            order,
        } => {
            out.push(12);
            put_eo(out, *endian, *order);
            put_val(out, *addr);
        }
        Op::StoreExclusive {
            addr,
            value,
            endian,
            order,
        } => {
            out.push(13);
            put_eo(out, *endian, *order);
            put_val(out, *addr);
            put_val(out, *value);
        }
        Op::ClearExclusive => out.push(14),
        Op::Fence(o) => out.extend_from_slice(&[15, o.code()]),
        Op::Fp(f, args) => {
            let (c, r) = fp_code(*f);
            out.extend_from_slice(&[16, c, r, args.len() as u8]);
            for a in args {
                put_val(out, *a);
            }
        }
        Op::Guard(v, code) => {
            out.push(17);
            put_val(out, *v);
            put_uvar(out, u64::from(*code));
        }
        Op::Intrinsic(call) => {
            out.push(18);
            put_str(out, call.name);
            put_uvar(out, call.args.len() as u64);
            for a in &call.args {
                put_val(out, *a);
            }
        }
        Op::Tick(n) => {
            out.push(19);
            put_uvar(out, u64::from(*n));
        }
        Op::TickIf(v, n) => {
            out.push(20);
            put_val(out, *v);
            put_uvar(out, u64::from(*n));
        }
        Op::IoLoad { addr, endian, kind } => {
            out.extend_from_slice(&[21, endian.code(), kind.code()]);
            put_val(out, *addr);
        }
        Op::IoStore {
            addr,
            value,
            endian,
            kind,
        } => {
            out.extend_from_slice(&[22, endian.code(), kind.code()]);
            put_val(out, *addr);
            put_val(out, *value);
        }
        Op::Sync => out.push(23),
        Op::Crypto(c, v) => {
            out.extend_from_slice(&[24, c.code()]);
            put_val(out, *v);
        }
        Op::AtomicRmw {
            op,
            addr,
            value,
            endian,
            order,
        } => {
            out.extend_from_slice(&[25, op.code()]);
            put_eo(out, *endian, *order);
            put_val(out, *addr);
            put_val(out, *value);
        }
        Op::CompareSwap {
            addr,
            expected,
            value,
            endian,
            order,
        } => {
            out.push(26);
            put_eo(out, *endian, *order);
            put_val(out, *addr);
            put_val(out, *expected);
            put_val(out, *value);
        }
        Op::Lane(l, args) => {
            out.extend_from_slice(&[27, l.kind.code(), l.lane.code(), l.imm]);
            for a in args {
                put_val(out, *a);
            }
        }
        Op::Permute(p) => {
            out.push(28 + u8::from(p.b.is_some()));
            out.extend_from_slice(&p.pattern);
            put_val(out, p.a);
            if let Some(b) = p.b {
                put_val(out, b);
            }
        }
        Op::Pac {
            data,
            modifier,
            key0,
            key1,
        } => {
            out.push(30);
            for v in [data, modifier, key0, key1] {
                put_val(out, *v);
            }
        }
    }
}

fn val(r: &mut Reader<'_>) -> Res<Value> {
    r.u32().map(Value)
}

fn reg(r: &mut Reader<'_>) -> Res<Reg> {
    u16::try_from(r.uvar()?)
        .map(Reg)
        .or_else(|_| bad("slot out of range"))
}

fn eo(r: &mut Reader<'_>) -> Res<(Endian, Order)> {
    let b = r.u8()?;
    Ok((Endian::decode(b & 1)?, Order::decode(b >> 1)?))
}

fn get_op(r: &mut Reader<'_>) -> Res<Op> {
    Ok(match r.u8()? {
        0 => {
            let lo = r.uvar()?;
            let hi = r.uvar()?;
            Op::Const(Konst::new(u128::from(hi) << 64 | u128::from(lo)))
        }
        1 => Op::Get(reg(r)?),
        2 => {
            let g = reg(r)?;
            Op::Set(g, val(r)?)
        }
        3 => {
            let u = UnOp::decode(r.u8()?)?;
            Op::Un(u, val(r)?)
        }
        4 => {
            let b = BinOp::decode(r.u8()?)?;
            Op::Bin(b, val(r)?, val(r)?)
        }
        5 => {
            let c = CmpOp::decode(r.u8()?)?;
            Op::Cmp(c, val(r)?, val(r)?)
        }
        6 => Op::Select(val(r)?, val(r)?, val(r)?),
        7 => {
            let k = ExtKind::decode(r.u8()?)?;
            Op::Ext(k, val(r)?)
        }
        8 => {
            let lo = r.u8()?;
            Op::Extract(val(r)?, lo)
        }
        9 => Op::Concat(val(r)?, val(r)?),
        10 => {
            let (endian, order) = eo(r)?;
            Op::Load {
                addr: val(r)?,
                endian,
                order,
            }
        }
        11 => {
            let (endian, order) = eo(r)?;
            Op::Store {
                addr: val(r)?,
                value: val(r)?,
                endian,
                order,
            }
        }
        12 => {
            let (endian, order) = eo(r)?;
            Op::LoadExclusive {
                addr: val(r)?,
                endian,
                order,
            }
        }
        13 => {
            let (endian, order) = eo(r)?;
            Op::StoreExclusive {
                addr: val(r)?,
                value: val(r)?,
                endian,
                order,
            }
        }
        14 => Op::ClearExclusive,
        15 => Op::Fence(Order::decode(r.u8()?)?),
        16 => {
            let c = r.u8()?;
            let rd = r.u8()?;
            let n = usize::from(r.u8()?);
            if n > 3 {
                return bad("too many floating-point operands");
            }
            let mut args = [Value(0); 3];
            for a in &mut args[..n] {
                *a = val(r)?;
            }
            Op::Fp(fp_decode(c, rd)?, FpArgs::new(&args[..n]))
        }
        17 => Op::Guard(val(r)?, r.u32()?),
        18 => {
            let name = crate::text::intern(r.str()?);
            let n = r.count()?;
            let mut args = Vec::with_capacity(n);
            for _ in 0..n {
                args.push(val(r)?);
            }
            Op::intrinsic(name, args)
        }
        19 => Op::Tick(r.u32()?),
        20 => Op::TickIf(val(r)?, r.u32()?),
        21 => {
            let endian = Endian::decode(r.u8()?)?;
            let kind = IoKind::decode(r.u8()?)?;
            Op::IoLoad {
                addr: val(r)?,
                endian,
                kind,
            }
        }
        22 => {
            let endian = Endian::decode(r.u8()?)?;
            let kind = IoKind::decode(r.u8()?)?;
            Op::IoStore {
                addr: val(r)?,
                value: val(r)?,
                endian,
                kind,
            }
        }
        23 => Op::Sync,
        24 => {
            let c = CryptoOp::decode(r.u8()?)?;
            Op::Crypto(c, val(r)?)
        }
        25 => {
            let op = AtomicOp::decode(r.u8()?)?;
            let (endian, order) = eo(r)?;
            Op::AtomicRmw {
                op,
                addr: val(r)?,
                value: val(r)?,
                endian,
                order,
            }
        }
        26 => {
            let (endian, order) = eo(r)?;
            Op::CompareSwap {
                addr: val(r)?,
                expected: val(r)?,
                value: val(r)?,
                endian,
                order,
            }
        }
        27 => {
            let kind = LaneKind::decode(r.u8()?)?;
            let lane = LaneWidth::decode(r.u8()?)?;
            let imm = r.u8()?;
            let mut args = [Value(0); 3];
            for a in &mut args[..kind.arity()] {
                *a = val(r)?;
            }
            Op::Lane(
                LaneOp::with_imm(kind, lane, imm),
                FpArgs::new(&args[..kind.arity()]),
            )
        }
        c @ (28 | 29) => {
            let mut pattern = [0u8; 16];
            pattern.copy_from_slice(r.bytes(16)?);
            let a = val(r)?;
            let b = if c == 29 { Some(val(r)?) } else { None };
            Op::Permute(Box::new(Permute { pattern, a, b }))
        }
        30 => Op::Pac {
            data: val(r)?,
            modifier: val(r)?,
            key0: val(r)?,
            key1: val(r)?,
        },
        _ => return bad("bad operation code"),
    })
}

fn put_exit(out: &mut Vec<u8>, e: &Exit) {
    match e.next {
        Next::Const(a) => {
            out.push(0);
            put_uvar(out, a);
        }
        Next::Select {
            cond,
            taken,
            not_taken,
        } => {
            out.push(1);
            put_val(out, cond);
            put_uvar(out, taken);
            put_uvar(out, not_taken);
        }
        Next::Dynamic(v) => {
            out.push(2);
            put_val(out, v);
        }
    }
    match e.flow {
        Flow::Jump => out.push(0),
        Flow::Call { ret } => {
            out.push(1);
            put_uvar(out, ret);
        }
        Flow::Return => out.push(2),
        Flow::Trap(c) => {
            out.push(3);
            put_uvar(out, u64::from(c));
        }
    }
}

fn get_exit(r: &mut Reader<'_>) -> Res<Exit> {
    let next = match r.u8()? {
        0 => Next::Const(r.uvar()?),
        1 => Next::Select {
            cond: val(r)?,
            taken: r.uvar()?,
            not_taken: r.uvar()?,
        },
        2 => Next::Dynamic(val(r)?),
        _ => return bad("bad exit"),
    };
    let flow = match r.u8()? {
        0 => Flow::Jump,
        1 => Flow::Call { ret: r.uvar()? },
        2 => Flow::Return,
        3 => Flow::Trap(r.u32()?),
        _ => return bad("bad flow"),
    };
    Ok(Exit { next, flow })
}

/// Appends `b` to `out`. `files` lists the register files of the caller's
/// frontends; the block's file is written as its index there.
///
/// # Panics
///
/// Panics when the block's register file is not in `files`.
pub fn write_block(out: &mut Vec<u8>, b: &Block, files: &[RegFile]) {
    let file = files
        .iter()
        .position(|f| *f == b.regs)
        .expect("the block's register file is in the list");
    put_uvar(out, file as u64);
    out.push(b.addr_width.bits() as u8);
    put_uvar(out, b.pc);
    put_uvar(out, u64::from(b.len));
    match b.bank {
        None => out.push(0),
        Some(v) => {
            out.push(1);
            put_uvar(out, v);
        }
    }
    put_uvar(out, b.insts.len() as u64);
    for i in &b.insts {
        out.push(i.width.map_or(0, |w| w.bits() as u8));
        put_op(out, &i.op);
    }
    put_exit(out, &b.exit);
}

/// Reads one block written by [`write_block`] against the same `files`.
///
/// # Errors
///
/// The bytes are not a block of `files`.
pub fn read_block(r: &mut Reader<'_>, files: &[RegFile]) -> Res<Block> {
    let regs = *files
        .get(usize::try_from(r.uvar()?).or_else(|_| bad("bad register file"))?)
        .ok_or(BinError("bad register file"))?;
    let addr_width = Width::try_new(u32::from(r.u8()?)).ok_or(BinError("bad width"))?;
    let pc = r.uvar()?;
    let len = r.u32()?;
    let bank = match r.u8()? {
        0 => None,
        1 => Some(r.uvar()?),
        _ => return bad("bad bank"),
    };
    let n = r.count()?;
    let mut insts = Vec::with_capacity(n);
    for _ in 0..n {
        let w = r.u8()?;
        let width = if w == 0 {
            None
        } else {
            Some(Width::try_new(u32::from(w)).ok_or(BinError("bad width"))?)
        };
        insts.push(Inst {
            op: get_op(r)?,
            width,
        });
    }
    let exit = get_exit(r)?;
    Ok(Block {
        regs,
        addr_width,
        pc,
        len,
        insts,
        exit,
        bank,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn codes_follow_the_variant_order() {
        fn check<T: Coded>() {
            for (i, v) in T::ALL.iter().enumerate() {
                assert_eq!(usize::from(v.code()), i);
            }
        }
        check::<UnOp>();
        check::<BinOp>();
        check::<CmpOp>();
        check::<ExtKind>();
        check::<Endian>();
        check::<Order>();
        check::<FpRound>();
        check::<IoKind>();
        check::<AtomicOp>();
        check::<CryptoOp>();
    }

    #[test]
    fn integers_round_trip() {
        let mut v = Vec::new();
        let xs = [0u64, 1, 127, 128, 300, u64::from(u32::MAX), u64::MAX];
        for x in xs {
            put_uvar(&mut v, x);
        }
        let mut r = Reader::new(&v);
        for x in xs {
            assert_eq!(r.uvar(), Ok(x));
        }
        assert!(r.is_empty());
        assert!(Reader::new(&[0x80]).uvar().is_err());
        assert!(Reader::new(&[0xff; 11]).uvar().is_err());
    }
}
