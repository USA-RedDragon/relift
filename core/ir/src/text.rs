//! The textual form of a [`Block`]: a printer (`Display`) and a parser
//! ([`parse`]) that round-trip exactly.
//!
//! ```text
//! block 0x1000 len 4 regs arm64
//!   v0:i64 = get x1
//!   v1:i64 = const 0x10
//!   v2:i64 = add v0, v1
//!   v3:i64 = load.le.plain v2
//!   v4:i1 = cmp.eq v3, v1
//!   set x0, v3
//!   store.le.release v2, v3
//!   exit jump select v4, 0x1040, 0x1004
//! ```
//!
//! # Grammar
//!
//! Lines are separated by newlines; blank lines are ignored; tokens are
//! separated by spaces, and `,` is a token of its own. Instructions are
//! printed indented by two spaces, the parser accepts any indentation.
//!
//! ```text
//! block  := header inst* exit
//! header := "block" HEX "len" DEC "regs" NAME [ "addr" TYPE ] [ "bank" HEX ]
//! inst   := [ "v" N ":" TYPE "=" ] op
//! exit   := "exit" flow next
//! flow   := "jump" | "call" HEX | "return" | "trap" HEX
//! next   := HEX | "select" value "," HEX "," HEX | "dynamic" value
//! value  := "v" N
//! TYPE   := "i" WIDTH           (1 to 128 bits)
//! ```
//!
//! `NAME` is the register file's name ([`RegFile::name`]); [`parse`] looks it
//! up in the files it is given. The address width is omitted when it is 64
//! bits, otherwise it is printed as `addr i32` and so on; the bank
//! ([`Block::bank`]) is printed only when the block has one. `HEX` is `0x` and
//! hex digits. The exit puts the flow first so the common case reads
//! `exit jump 0x1004`; `call` names the return address, `trap` the
//! frontend's code, then comes where control goes next. A `select` taken
//! address comes first: `select vC, taken, not_taken`.
//!
//! Values are numbered by instruction index: `v0` is the first instruction,
//! and instructions with no result (`set`, `store`, fences) take an index
//! too, so a result prefix `vN:` must match the instruction's position. An
//! operand names an earlier instruction only.
//!
//! Operations, with operands separated by `, `:
//!
//! | Form | Meaning |
//! |------|---------|
//! | `const HEX` | [`Op::Const`] |
//! | `get REG` / `set REG, v` | [`Op::Get`] / [`Op::Set`], by declared slot name |
//! | `not neg clz ctz popcount bitreverse bswap v` | [`UnOp`] |
//! | `add sub mul udiv sdiv urem srem and or xor shl lshr ashr rotr a, b` | [`BinOp`] |
//! | `cmp.eq cmp.ne cmp.ult cmp.ule cmp.slt cmp.sle a, b` | [`CmpOp`] |
//! | `select c, a, b` | [`Op::Select`] |
//! | `zext v` / `sext v` | [`Op::Ext`] to the result width |
//! | `extract v, LO` | [`Op::Extract`], `LO` decimal |
//! | `concat hi, lo` | [`Op::Concat`] |
//! | `load.E.O addr` / `store.E.O addr, value` | memory, `E` is `le` or `be` |
//! | `load.excl.E.O addr` / `store.excl.E.O addr, value` | exclusive memory |
//! | `atomic.U.E.O addr, value` | [`Op::AtomicRmw`], `U` is `add clr eor set smax smin umax umin swp` |
//! | `cas.E.O addr, expected, value` | [`Op::CompareSwap`] |
//! | `crypto.C v` | [`Op::Crypto`], `C` is `aes.subbytes aes.invsubbytes aes.mixcolumns aes.invmixcolumns sm4.sbox` |
//! | `lane.K.W args` | [`Op::Lane`]: `K` is a [`LaneKind`] mnemonic (`add qadds mulhs shli widenlos narrow mulwloss bcast extract insert splat` and so on, see `LANE` in this file), `W` the lane width 8, 16, 32 or 64 (omitted for the bitwise kinds `and or xor andn not select`); shift, broadcast, extract and insert end in `, N`, a decimal immediate |\n//! | `lane.perm v, PAT` / `lane.perm2 a, b, PAT` | [`Op::Permute`]; `PAT` is the 16 byte indices as a 128-bit hex value, byte `i` of the value giving result byte `i` |\n//! | `fence.O` / `clear.excl` | fence, clear the monitor |
//! | `fp.add fp.sub fp.mul fp.div fp.sqrt fp.muladd fp.neg fp.abs args` | [`FpOp`] |
//! | `fp.rint.R fp.tosint.R fp.touint.R fp.fromsint fp.fromuint fp.convert v` | [`FpOp`] conversions; `R` is `n`, `a`, `m`, `p` or `z` ([`FpRound`]) |
//! | `intrinsic "name" args` | [`Op::Intrinsic`], args may be empty |
//! | `guard c, HEX` | [`Op::Guard`]: leave to slow path `HEX` when `c` is 1 |
//! | `tick DEC` / `tick.if c, DEC` | [`Op::Tick`] / [`Op::TickIf`]: guest cycles, decimal |
//! | `sync` | [`Op::Sync`], the last instruction |
//! | `io.load.E.K addr` / `io.store.E.K addr, value` | [`Op::IoLoad`] / [`Op::IoStore`]; `K` is `any` or `dev` ([`IoKind`]) |
//!
//! The ordering `O` is `plain`, `acquire`, `release`, `acqrel` or `seqcst`.
//! Intrinsic names are quoted with `\"` and `\\` escapes. A parsed
//! intrinsic name is interned in a process-wide table because [`Op`] holds a
//! `&'static str`; the table grows by one entry per distinct name.
//!
//! The parser checks that every reference is to an earlier value, that
//! registers exist, and finally runs [`Block::verify`], mapping its error to
//! the offending line. Errors carry a 1-based line and column.

use std::collections::HashSet;
use std::fmt;
use std::sync::{Mutex, OnceLock};

use crate::{
    AtomicOp, BinOp, Block, CmpOp, CryptoOp, Endian, Exit, ExtKind, Flow, FpArgs, FpOp, FpRound,
    Inst, IoKind, LaneKind, LaneOp, LaneWidth, Next, Op, Order, Permute, RegFile, UnOp, Value,
    Width,
};

const UN: [(UnOp, &str); 7] = [
    (UnOp::Not, "not"),
    (UnOp::Neg, "neg"),
    (UnOp::Clz, "clz"),
    (UnOp::Ctz, "ctz"),
    (UnOp::Popcount, "popcount"),
    (UnOp::BitReverse, "bitreverse"),
    (UnOp::ByteSwap, "bswap"),
];

const BIN: [(BinOp, &str); 14] = [
    (BinOp::Add, "add"),
    (BinOp::Sub, "sub"),
    (BinOp::Mul, "mul"),
    (BinOp::UDiv, "udiv"),
    (BinOp::SDiv, "sdiv"),
    (BinOp::URem, "urem"),
    (BinOp::SRem, "srem"),
    (BinOp::And, "and"),
    (BinOp::Or, "or"),
    (BinOp::Xor, "xor"),
    (BinOp::Shl, "shl"),
    (BinOp::LShr, "lshr"),
    (BinOp::AShr, "ashr"),
    (BinOp::RotR, "rotr"),
];

const CMP: [(CmpOp, &str); 6] = [
    (CmpOp::Eq, "eq"),
    (CmpOp::Ne, "ne"),
    (CmpOp::Ult, "ult"),
    (CmpOp::Ule, "ule"),
    (CmpOp::Slt, "slt"),
    (CmpOp::Sle, "sle"),
];

const FP: [(FpOp, &str); 26] = [
    (FpOp::Add, "add"),
    (FpOp::Sub, "sub"),
    (FpOp::Mul, "mul"),
    (FpOp::Div, "div"),
    (FpOp::Sqrt, "sqrt"),
    (FpOp::MulAdd, "muladd"),
    (FpOp::Neg, "neg"),
    (FpOp::Abs, "abs"),
    (FpOp::RoundInt(FpRound::TiesEven), "rint.n"),
    (FpOp::RoundInt(FpRound::TiesAway), "rint.a"),
    (FpOp::RoundInt(FpRound::Floor), "rint.m"),
    (FpOp::RoundInt(FpRound::Ceil), "rint.p"),
    (FpOp::RoundInt(FpRound::Zero), "rint.z"),
    (FpOp::ToSInt(FpRound::TiesEven), "tosint.n"),
    (FpOp::ToSInt(FpRound::TiesAway), "tosint.a"),
    (FpOp::ToSInt(FpRound::Floor), "tosint.m"),
    (FpOp::ToSInt(FpRound::Ceil), "tosint.p"),
    (FpOp::ToSInt(FpRound::Zero), "tosint.z"),
    (FpOp::ToUInt(FpRound::TiesEven), "touint.n"),
    (FpOp::ToUInt(FpRound::TiesAway), "touint.a"),
    (FpOp::ToUInt(FpRound::Floor), "touint.m"),
    (FpOp::ToUInt(FpRound::Ceil), "touint.p"),
    (FpOp::ToUInt(FpRound::Zero), "touint.z"),
    (FpOp::FromSInt, "fromsint"),
    (FpOp::FromUInt, "fromuint"),
    (FpOp::Convert, "convert"),
];

const ORDER: [(Order, &str); 5] = [
    (Order::Plain, "plain"),
    (Order::Acquire, "acquire"),
    (Order::Release, "release"),
    (Order::AcqRel, "acqrel"),
    (Order::SeqCst, "seqcst"),
];

const ATOMIC: [(AtomicOp, &str); 9] = [
    (AtomicOp::Add, "add"),
    (AtomicOp::Clear, "clr"),
    (AtomicOp::Xor, "eor"),
    (AtomicOp::Or, "set"),
    (AtomicOp::SMax, "smax"),
    (AtomicOp::SMin, "smin"),
    (AtomicOp::UMax, "umax"),
    (AtomicOp::UMin, "umin"),
    (AtomicOp::Swap, "swp"),
];

const CRYPTO: [(CryptoOp, &str); 5] = [
    (CryptoOp::AesSubBytes, "aes.subbytes"),
    (CryptoOp::AesInvSubBytes, "aes.invsubbytes"),
    (CryptoOp::AesMixColumns, "aes.mixcolumns"),
    (CryptoOp::AesInvMixColumns, "aes.invmixcolumns"),
    (CryptoOp::Sm4Sbox, "sm4.sbox"),
];

const LANE: [(LaneKind, &str); 52] = [
    (LaneKind::Add, "add"),
    (LaneKind::Sub, "sub"),
    (LaneKind::AddSatS, "qadds"),
    (LaneKind::AddSatU, "qaddu"),
    (LaneKind::SubSatS, "qsubs"),
    (LaneKind::SubSatU, "qsubu"),
    (LaneKind::MulLo, "mul"),
    (LaneKind::MulHiS, "mulhs"),
    (LaneKind::MulHiU, "mulhu"),
    (LaneKind::MulHiSU, "mulhsu"),
    (LaneKind::And, "and"),
    (LaneKind::Or, "or"),
    (LaneKind::Xor, "xor"),
    (LaneKind::AndNot, "andn"),
    (LaneKind::Not, "not"),
    (LaneKind::MinS, "mins"),
    (LaneKind::MinU, "minu"),
    (LaneKind::MaxS, "maxs"),
    (LaneKind::MaxU, "maxu"),
    (LaneKind::Abs, "abs"),
    (LaneKind::Neg, "neg"),
    (LaneKind::Eq, "eq"),
    (LaneKind::Ne, "ne"),
    (LaneKind::Slt, "slt"),
    (LaneKind::Sle, "sle"),
    (LaneKind::Ult, "ult"),
    (LaneKind::Ule, "ule"),
    (LaneKind::ShlImm, "shli"),
    (LaneKind::LShrImm, "lshri"),
    (LaneKind::AShrImm, "ashri"),
    (LaneKind::Shl, "shl"),
    (LaneKind::LShr, "lshr"),
    (LaneKind::AShr, "ashr"),
    (LaneKind::Select, "select"),
    (LaneKind::WidenLoS, "widenlos"),
    (LaneKind::WidenLoU, "widenlou"),
    (LaneKind::WidenHiS, "widenhis"),
    (LaneKind::WidenHiU, "widenhiu"),
    (LaneKind::NarrowTrunc, "narrow"),
    (LaneKind::NarrowSS, "narrowss"),
    (LaneKind::NarrowUU, "narrowuu"),
    (LaneKind::NarrowSU, "narrowsu"),
    (LaneKind::MulWideLoSS, "mulwloss"),
    (LaneKind::MulWideLoUU, "mulwlouu"),
    (LaneKind::MulWideLoSU, "mulwlosu"),
    (LaneKind::MulWideHiSS, "mulwhiss"),
    (LaneKind::MulWideHiUU, "mulwhiuu"),
    (LaneKind::MulWideHiSU, "mulwhisu"),
    (LaneKind::Broadcast, "bcast"),
    (LaneKind::Extract, "extract"),
    (LaneKind::Insert, "insert"),
    (LaneKind::Splat, "splat"),
];

fn name_of<T: Copy + PartialEq>(table: &[(T, &'static str)], x: T) -> &'static str {
    table
        .iter()
        .find(|(t, _)| *t == x)
        .map(|(_, n)| *n)
        .unwrap_or("?")
}

fn lookup<T: Copy>(table: &[(T, &'static str)], name: &str) -> Option<T> {
    table.iter().find(|(_, n)| *n == name).map(|(t, _)| *t)
}

fn endian_name(e: Endian) -> &'static str {
    match e {
        Endian::Little => "le",
        Endian::Big => "be",
    }
}

fn io_kind_name(k: IoKind) -> &'static str {
    match k {
        IoKind::Any => "any",
        IoKind::Device => "dev",
    }
}

fn write_args(f: &mut fmt::Formatter<'_>, args: &[Value]) -> fmt::Result {
    for (i, a) in args.iter().enumerate() {
        if i > 0 {
            f.write_str(",")?;
        }
        write!(f, " v{}", a.0)?;
    }
    Ok(())
}

fn write_quoted(f: &mut fmt::Formatter<'_>, s: &str) -> fmt::Result {
    f.write_str("\"")?;
    for c in s.chars() {
        if c == '"' || c == '\\' {
            f.write_str("\\")?;
        }
        write!(f, "{c}")?;
    }
    f.write_str("\"")
}

impl fmt::Display for Block {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "block {:#x} len {} regs {}",
            self.pc,
            self.len,
            self.regs.name()
        )?;
        if self.addr_width != Width::W64 {
            write!(f, " addr i{}", self.addr_width.bits())?;
        }
        if let Some(bank) = self.bank {
            write!(f, " bank {bank:#x}")?;
        }
        writeln!(f)?;
        for (i, inst) in self.insts.iter().enumerate() {
            f.write_str("  ")?;
            if let Some(w) = inst.width {
                write!(f, "v{i}:i{} = ", w.bits())?;
            }
            match &inst.op {
                Op::Const(c) => write!(f, "const {:#x}", c.get())?,
                Op::Get(r) => write!(f, "get {}", self.regs.decl(*r).name)?,
                Op::Set(r, v) => write!(f, "set {}, v{}", self.regs.decl(*r).name, v.0)?,
                Op::Un(u, v) => write!(f, "{} v{}", name_of(&UN, *u), v.0)?,
                Op::Bin(b, x, y) => write!(f, "{} v{}, v{}", name_of(&BIN, *b), x.0, y.0)?,
                Op::Cmp(c, x, y) => write!(f, "cmp.{} v{}, v{}", name_of(&CMP, *c), x.0, y.0)?,
                Op::Select(c, x, y) => write!(f, "select v{}, v{}, v{}", c.0, x.0, y.0)?,
                Op::Ext(ExtKind::Zero, v) => write!(f, "zext v{}", v.0)?,
                Op::Ext(ExtKind::Sign, v) => write!(f, "sext v{}", v.0)?,
                Op::Extract(v, lo) => write!(f, "extract v{}, {lo}", v.0)?,
                Op::Concat(h, l) => write!(f, "concat v{}, v{}", h.0, l.0)?,
                Op::Load {
                    addr,
                    endian,
                    order,
                } => write!(
                    f,
                    "load.{}.{} v{}",
                    endian_name(*endian),
                    name_of(&ORDER, *order),
                    addr.0
                )?,
                Op::LoadExclusive {
                    addr,
                    endian,
                    order,
                } => write!(
                    f,
                    "load.excl.{}.{} v{}",
                    endian_name(*endian),
                    name_of(&ORDER, *order),
                    addr.0
                )?,
                Op::Store {
                    addr,
                    value,
                    endian,
                    order,
                } => write!(
                    f,
                    "store.{}.{} v{}, v{}",
                    endian_name(*endian),
                    name_of(&ORDER, *order),
                    addr.0,
                    value.0
                )?,
                Op::StoreExclusive {
                    addr,
                    value,
                    endian,
                    order,
                } => write!(
                    f,
                    "store.excl.{}.{} v{}, v{}",
                    endian_name(*endian),
                    name_of(&ORDER, *order),
                    addr.0,
                    value.0
                )?,
                Op::ClearExclusive => f.write_str("clear.excl")?,
                Op::Guard(v, code) => write!(f, "guard v{}, {code:#x}", v.0)?,
                Op::Fence(o) => write!(f, "fence.{}", name_of(&ORDER, *o))?,
                Op::Fp(op, args) => {
                    write!(f, "fp.{}", name_of(&FP, *op))?;
                    write_args(f, args)?;
                }
                Op::Intrinsic(call) => {
                    f.write_str("intrinsic ")?;
                    write_quoted(f, call.name)?;
                    write_args(f, &call.args)?;
                }
                Op::Tick(n) => write!(f, "tick {n}")?,
                Op::Sync => f.write_str("sync")?,
                Op::TickIf(v, n) => write!(f, "tick.if v{}, {n}", v.0)?,
                Op::IoLoad { addr, endian, kind } => write!(
                    f,
                    "io.load.{}.{} v{}",
                    endian_name(*endian),
                    io_kind_name(*kind),
                    addr.0
                )?,
                Op::IoStore {
                    addr,
                    value,
                    endian,
                    kind,
                } => write!(
                    f,
                    "io.store.{}.{} v{}, v{}",
                    endian_name(*endian),
                    io_kind_name(*kind),
                    addr.0,
                    value.0
                )?,
                Op::Crypto(op, x) => write!(f, "crypto.{} v{}", name_of(&CRYPTO, *op), x.0)?,
                Op::Lane(l, args) => {
                    write!(f, "lane.{}", name_of(&LANE, l.kind))?;
                    if !l.kind.bitwise() {
                        write!(f, ".{}", l.lane.bits())?;
                    }
                    write_args(f, args)?;
                    if l.kind.has_imm() {
                        write!(f, ", {}", l.imm)?;
                    }
                }
                Op::Permute(p) => {
                    match p.b {
                        Some(b) => write!(f, "lane.perm2 v{}, v{}", p.a.0, b.0)?,
                        None => write!(f, "lane.perm v{}", p.a.0)?,
                    }
                    write!(f, ", {:#x}", p.pattern_value())?;
                }
                Op::AtomicRmw {
                    op,
                    addr,
                    value,
                    endian,
                    order,
                } => write!(
                    f,
                    "atomic.{}.{}.{} v{}, v{}",
                    name_of(&ATOMIC, *op),
                    endian_name(*endian),
                    name_of(&ORDER, *order),
                    addr.0,
                    value.0
                )?,
                Op::CompareSwap {
                    addr,
                    expected,
                    value,
                    endian,
                    order,
                } => write!(
                    f,
                    "cas.{}.{} v{}, v{}, v{}",
                    endian_name(*endian),
                    name_of(&ORDER, *order),
                    addr.0,
                    expected.0,
                    value.0
                )?,
            }
            writeln!(f)?;
        }
        f.write_str("  exit ")?;
        match self.exit.flow {
            Flow::Jump => f.write_str("jump")?,
            Flow::Call { ret } => write!(f, "call {ret:#x}")?,
            Flow::Return => f.write_str("return")?,
            Flow::Trap(code) => write!(f, "trap {code:#x}")?,
        }
        match self.exit.next {
            Next::Const(a) => writeln!(f, " {a:#x}"),
            Next::Select {
                cond,
                taken,
                not_taken,
            } => writeln!(f, " select v{}, {taken:#x}, {not_taken:#x}", cond.0),
            Next::Dynamic(v) => writeln!(f, " dynamic v{}", v.0),
        }
    }
}

/// A syntax or type error in IR text, at a 1-based line and column.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ParseError {
    /// The 1-based line.
    pub line: usize,
    /// The 1-based column, counted in characters.
    pub col: usize,
    /// What is wrong.
    pub message: String,
}

impl fmt::Display for ParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}:{}: {}", self.line, self.col, self.message)
    }
}

impl std::error::Error for ParseError {}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Kind {
    Word,
    Comma,
    Str,
}

#[derive(Clone)]
struct Tok {
    text: String,
    col: usize,
    kind: Kind,
}

fn tokenize(line: &str, ln: usize) -> Result<Vec<Tok>, ParseError> {
    let chars: Vec<char> = line.chars().collect();
    let mut toks = Vec::new();
    let mut i = 0;
    while i < chars.len() {
        let c = chars[i];
        if c.is_whitespace() {
            i += 1;
        } else if c == ',' {
            toks.push(Tok {
                text: ",".to_string(),
                col: i + 1,
                kind: Kind::Comma,
            });
            i += 1;
        } else if c == '"' {
            let start = i;
            let mut s = String::new();
            i += 1;
            loop {
                match chars.get(i) {
                    None => {
                        return Err(ParseError {
                            line: ln,
                            col: start + 1,
                            message: "unterminated string".to_string(),
                        });
                    }
                    Some('"') => {
                        i += 1;
                        break;
                    }
                    Some('\\') => match chars.get(i + 1) {
                        Some(&e @ ('"' | '\\')) => {
                            s.push(e);
                            i += 2;
                        }
                        _ => {
                            return Err(ParseError {
                                line: ln,
                                col: i + 1,
                                message: "bad escape in string".to_string(),
                            });
                        }
                    },
                    Some(&ch) => {
                        s.push(ch);
                        i += 1;
                    }
                }
            }
            toks.push(Tok {
                text: s,
                col: start + 1,
                kind: Kind::Str,
            });
        } else {
            let start = i;
            while i < chars.len() && !chars[i].is_whitespace() && chars[i] != ',' && chars[i] != '"'
            {
                i += 1;
            }
            toks.push(Tok {
                text: chars[start..i].iter().collect(),
                col: start + 1,
                kind: Kind::Word,
            });
        }
    }
    Ok(toks)
}

struct Cursor {
    toks: Vec<Tok>,
    pos: usize,
    line: usize,
    end_col: usize,
}

impl Cursor {
    fn err<T>(&self, col: usize, message: impl Into<String>) -> Result<T, ParseError> {
        Err(ParseError {
            line: self.line,
            col,
            message: message.into(),
        })
    }

    fn peek(&self) -> Option<&Tok> {
        self.toks.get(self.pos)
    }

    fn next(&mut self, what: &str) -> Result<Tok, ParseError> {
        match self.toks.get(self.pos) {
            Some(t) => {
                self.pos += 1;
                Ok(t.clone())
            }
            None => self.err(self.end_col, format!("expected {what}")),
        }
    }

    fn word(&mut self, what: &str) -> Result<Tok, ParseError> {
        let t = self.next(what)?;
        if t.kind == Kind::Word {
            Ok(t)
        } else {
            self.err(t.col, format!("expected {what}"))
        }
    }

    fn keyword(&mut self, kw: &str) -> Result<(), ParseError> {
        let t = self.word(&format!("`{kw}`"))?;
        if t.text == kw {
            Ok(())
        } else {
            self.err(t.col, format!("expected `{kw}`, found `{}`", t.text))
        }
    }

    fn comma(&mut self) -> Result<(), ParseError> {
        let t = self.next("`,`")?;
        if t.kind == Kind::Comma {
            Ok(())
        } else {
            self.err(t.col, format!("expected `,`, found `{}`", t.text))
        }
    }

    fn end(&self) -> Result<(), ParseError> {
        match self.peek() {
            None => Ok(()),
            Some(t) => self.err(t.col, format!("unexpected `{}`", t.text)),
        }
    }

    fn hex(&mut self, what: &str) -> Result<u128, ParseError> {
        let t = self.word(what)?;
        self.hex_of(&t, what)
    }

    fn hex_of(&self, t: &Tok, what: &str) -> Result<u128, ParseError> {
        let digits = t.text.strip_prefix("0x").unwrap_or("");
        if digits.is_empty() || !digits.chars().all(|c| c.is_ascii_hexdigit()) {
            return self.err(t.col, format!("expected {what} as 0x and hex digits"));
        }
        match u128::from_str_radix(digits, 16) {
            Ok(v) => Ok(v),
            Err(_) => self.err(t.col, format!("{what} is too large")),
        }
    }

    fn hex64(&mut self, what: &str) -> Result<u64, ParseError> {
        let t = self.word(what)?;
        let v = self.hex_of(&t, what)?;
        u64::try_from(v).or_else(|_| self.err(t.col, format!("{what} does not fit 64 bits")))
    }

    fn width_of(&self, text: &str, col: usize) -> Result<Width, ParseError> {
        text.strip_prefix('i')
            .filter(|d| !d.is_empty() && d.chars().all(|c| c.is_ascii_digit()))
            .and_then(|d| d.parse::<u32>().ok())
            .and_then(Width::try_new)
            .map_or_else(
                || self.err(col, format!("bad type `{text}`, expected i1 to i128")),
                Ok,
            )
    }

    fn ty(&mut self) -> Result<Width, ParseError> {
        let t = self.word("a type")?;
        self.width_of(&t.text, t.col)
    }

    fn value(&mut self, defined: usize) -> Result<Value, ParseError> {
        let t = self.word("a value")?;
        let n = t
            .text
            .strip_prefix('v')
            .filter(|d| !d.is_empty() && d.chars().all(|c| c.is_ascii_digit()))
            .and_then(|d| d.parse::<u32>().ok());
        match n {
            Some(n) if (n as usize) < defined => Ok(Value(n)),
            Some(n) => self.err(t.col, format!("value v{n} is not defined before use")),
            None => self.err(t.col, format!("expected a value, found `{}`", t.text)),
        }
    }

    fn values(&mut self, n: usize, defined: usize) -> Result<Vec<Value>, ParseError> {
        let mut out = Vec::with_capacity(n);
        for i in 0..n {
            if i > 0 {
                self.comma()?;
            }
            out.push(self.value(defined)?);
        }
        Ok(out)
    }

    fn cycles(&mut self) -> Result<u32, ParseError> {
        let t = self.word("a cycle count")?;
        match t.text.parse::<u32>() {
            Ok(n) if t.text.chars().all(|d| d.is_ascii_digit()) => Ok(n),
            _ => self.err(t.col, "expected a cycle count as decimal 0 to 4294967295"),
        }
    }

    fn order(&self, text: &str, col: usize) -> Result<Order, ParseError> {
        lookup(&ORDER, text).map_or_else(|| self.err(col, format!("unknown ordering `{text}`")), Ok)
    }
}

pub(crate) fn intern(name: &str) -> &'static str {
    static TABLE: OnceLock<Mutex<HashSet<&'static str>>> = OnceLock::new();
    let mut table = TABLE
        .get_or_init(|| Mutex::new(HashSet::new()))
        .lock()
        .unwrap_or_else(|e| e.into_inner());
    if let Some(s) = table.get(name) {
        return s;
    }
    let s: &'static str = Box::leak(name.to_string().into_boxed_str());
    table.insert(s);
    s
}

struct Header {
    pc: u64,
    len: u32,
    regs: RegFile,
    addr_width: Width,
    bank: Option<u64>,
}

fn parse_header(c: &mut Cursor, files: &[RegFile]) -> Result<Header, ParseError> {
    c.keyword("block")?;
    let pc = c.hex64("an address")?;
    c.keyword("len")?;
    let lt = c.word("a length")?;
    let len = match lt.text.parse::<u32>() {
        Ok(v) if lt.text.chars().all(|d| d.is_ascii_digit()) => v,
        _ => return c.err(lt.col, "expected a length as decimal"),
    };
    c.keyword("regs")?;
    let nt = c.word("a register file name")?;
    let Some(regs) = files.iter().copied().find(|f| f.name() == nt.text) else {
        return c.err(nt.col, format!("unknown register file `{}`", nt.text));
    };
    let mut addr_width = Width::W64;
    if c.peek().is_some_and(|t| t.text == "addr") {
        c.keyword("addr")?;
        addr_width = c.ty()?;
    }
    let mut bank = None;
    if c.peek().is_some() {
        c.keyword("bank")?;
        bank = Some(c.hex64("a bank")?);
    }
    c.end()?;
    Ok(Header {
        pc,
        len,
        regs,
        addr_width,
        bank,
    })
}

fn parse_order_endian(
    c: &Cursor,
    t: &Tok,
    parts: &[&str],
    base: usize,
) -> Result<(Endian, Order), ParseError> {
    let col_of =
        |i: usize| -> usize { base + parts[..i].iter().map(|p| p.len() + 1).sum::<usize>() };
    let endian = match parts.first().copied() {
        Some("le") => Endian::Little,
        Some("be") => Endian::Big,
        Some(other) => {
            return c.err(col_of(0), format!("unknown byte order `{other}`"));
        }
        None => return c.err(t.col, "missing byte order"),
    };
    let Some(&o) = parts.get(1) else {
        return c.err(t.col, "missing ordering");
    };
    let order = c.order(o, col_of(1))?;
    if parts.len() > 2 {
        return c.err(col_of(2), format!("unexpected `{}`", parts[2]));
    }
    Ok((endian, order))
}

fn parse_io(c: &Cursor, t: &Tok, rest: &str, base: usize) -> Result<(Endian, IoKind), ParseError> {
    let parts: Vec<&str> = rest.split('.').collect();
    let col_of =
        |i: usize| -> usize { base + parts[..i].iter().map(|p| p.len() + 1).sum::<usize>() };
    let endian = match parts[0] {
        "le" => Endian::Little,
        "be" => Endian::Big,
        other => return c.err(col_of(0), format!("unknown byte order `{other}`")),
    };
    let kind = match parts.get(1).copied() {
        Some("any") => IoKind::Any,
        Some("dev") => IoKind::Device,
        Some(other) => return c.err(col_of(1), format!("unknown I/O kind `{other}`")),
        None => return c.err(t.col, "missing I/O kind"),
    };
    if parts.len() > 2 {
        return c.err(col_of(2), format!("unexpected `{}`", parts[2]));
    }
    Ok((endian, kind))
}

fn parse_op(c: &mut Cursor, regs: &RegFile, defined: usize) -> Result<(Op, usize), ParseError> {
    let m = c.word("a mnemonic")?;
    let text = m.text.as_str();
    let at = m.col;
    let reg = |c: &mut Cursor| -> Result<crate::Reg, ParseError> {
        let t = c.word("a register")?;
        match regs.find(&t.text) {
            Some(r) => Ok(r),
            None => c.err(t.col, format!("unknown register `{}`", t.text)),
        }
    };
    let op = if let Some(u) = lookup(&UN, text) {
        Op::Un(u, c.value(defined)?)
    } else if let Some(b) = lookup(&BIN, text) {
        let v = c.values(2, defined)?;
        Op::Bin(b, v[0], v[1])
    } else if let Some(cmp) = text.strip_prefix("cmp.").and_then(|s| lookup(&CMP, s)) {
        let v = c.values(2, defined)?;
        Op::Cmp(cmp, v[0], v[1])
    } else if let Some(f) = text.strip_prefix("fp.").and_then(|s| lookup(&FP, s)) {
        Op::Fp(f, FpArgs::new(&c.values(f.arity(), defined)?))
    } else if let Some(rest) = text
        .strip_prefix("lane.")
        .filter(|r| !r.starts_with("perm"))
    {
        let mut parts = rest.split('.');
        let name = parts.next().unwrap_or("");
        let Some(kind) = lookup(&LANE, name) else {
            return c.err(at, format!("unknown lane operation `{name}`"));
        };
        let lane = if kind.bitwise() {
            LaneWidth::L64
        } else {
            let wt = parts.next().unwrap_or("");
            match wt.parse::<u32>().ok().and_then(LaneWidth::from_bits) {
                Some(l) => l,
                None => {
                    return c.err(
                        at,
                        format!("expected a lane width 8, 16, 32 or 64 in `{text}`"),
                    );
                }
            }
        };
        if let Some(extra) = parts.next() {
            return c.err(at, format!("unexpected `{extra}` in `{text}`"));
        }
        let args = c.values(kind.arity(), defined)?;
        let mut imm = 0;
        if kind.has_imm() {
            c.comma()?;
            let it = c.word("an immediate")?;
            imm = match it.text.parse::<u8>() {
                Ok(n) if it.text.chars().all(|d| d.is_ascii_digit()) => n,
                _ => return c.err(it.col, "expected an immediate as decimal 0 to 255"),
            };
        }
        Op::Lane(LaneOp::with_imm(kind, lane, imm), FpArgs::new(&args))
    } else if let Some(k) = text
        .strip_prefix("crypto.")
        .and_then(|s| lookup(&CRYPTO, s))
    {
        Op::Crypto(k, c.value(defined)?)
    } else {
        match text {
            "lane.perm" | "lane.perm2" => {
                let two = text == "lane.perm2";
                let v = c.values(if two { 2 } else { 1 }, defined)?;
                c.comma()?;
                let pat = c.hex("a byte pattern")?;
                Op::Permute(Box::new(Permute {
                    pattern: pat.to_le_bytes(),
                    a: v[0],
                    b: v.get(1).copied(),
                }))
            }
            "const" => Op::konst(c.hex("a constant")?),
            "get" => Op::Get(reg(c)?),
            "set" => {
                let r = reg(c)?;
                c.comma()?;
                Op::Set(r, c.value(defined)?)
            }
            "select" => {
                let v = c.values(3, defined)?;
                Op::Select(v[0], v[1], v[2])
            }
            "zext" => Op::Ext(ExtKind::Zero, c.value(defined)?),
            "sext" => Op::Ext(ExtKind::Sign, c.value(defined)?),
            "extract" => {
                let v = c.value(defined)?;
                c.comma()?;
                let lt = c.word("a bit offset")?;
                let lo = match lt.text.parse::<u8>() {
                    Ok(n) if lt.text.chars().all(|d| d.is_ascii_digit()) => n,
                    _ => return c.err(lt.col, "expected a bit offset as decimal 0 to 255"),
                };
                Op::Extract(v, lo)
            }
            "concat" => {
                let v = c.values(2, defined)?;
                Op::Concat(v[0], v[1])
            }
            "tick" => Op::Tick(c.cycles()?),
            "sync" => Op::Sync,
            "tick.if" => {
                let v = c.value(defined)?;
                c.comma()?;
                Op::TickIf(v, c.cycles()?)
            }
            "clear.excl" => Op::ClearExclusive,
            "guard" => {
                let v = c.value(defined)?;
                c.comma()?;
                let code = c.hex("a slow-path code")?;
                let Ok(code) = u32::try_from(code) else {
                    return c.err(at, "the slow-path code does not fit 32 bits");
                };
                Op::Guard(v, code)
            }
            "intrinsic" => {
                let n = c.next("an intrinsic name")?;
                if n.kind != Kind::Str {
                    return c.err(n.col, "expected a quoted intrinsic name");
                }
                let mut args = Vec::new();
                if c.peek().is_some() {
                    args.push(c.value(defined)?);
                    while c.peek().is_some() {
                        c.comma()?;
                        args.push(c.value(defined)?);
                    }
                }
                Op::intrinsic(intern(&n.text), args)
            }
            _ => {
                if let Some(rest) = text.strip_prefix("io.load.") {
                    let (endian, kind) = parse_io(c, &m, rest, at + 8)?;
                    Op::IoLoad {
                        addr: c.value(defined)?,
                        endian,
                        kind,
                    }
                } else if let Some(rest) = text.strip_prefix("io.store.") {
                    let (endian, kind) = parse_io(c, &m, rest, at + 9)?;
                    let v = c.values(2, defined)?;
                    Op::IoStore {
                        addr: v[0],
                        value: v[1],
                        endian,
                        kind,
                    }
                } else if let Some(rest) = text.strip_prefix("fence.") {
                    Op::Fence(c.order(rest, at + 6)?)
                } else if let Some(rest) = text.strip_prefix("load.excl.") {
                    let parts: Vec<&str> = rest.split('.').collect();
                    let (endian, order) = parse_order_endian(c, &m, &parts, at + 10)?;
                    Op::LoadExclusive {
                        addr: c.value(defined)?,
                        endian,
                        order,
                    }
                } else if let Some(rest) = text.strip_prefix("load.") {
                    let parts: Vec<&str> = rest.split('.').collect();
                    let (endian, order) = parse_order_endian(c, &m, &parts, at + 5)?;
                    Op::Load {
                        addr: c.value(defined)?,
                        endian,
                        order,
                    }
                } else if let Some(rest) = text.strip_prefix("atomic.") {
                    let parts: Vec<&str> = rest.split('.').collect();
                    let Some(op) = parts.first().and_then(|p| lookup(&ATOMIC, p)) else {
                        return c.err(at + 7, "unknown atomic update");
                    };
                    let skip = 8 + parts[0].len();
                    let (endian, order) = parse_order_endian(c, &m, &parts[1..], at + skip)?;
                    let v = c.values(2, defined)?;
                    Op::AtomicRmw {
                        op,
                        addr: v[0],
                        value: v[1],
                        endian,
                        order,
                    }
                } else if let Some(rest) = text.strip_prefix("cas.") {
                    let parts: Vec<&str> = rest.split('.').collect();
                    let (endian, order) = parse_order_endian(c, &m, &parts, at + 4)?;
                    let v = c.values(3, defined)?;
                    Op::CompareSwap {
                        addr: v[0],
                        expected: v[1],
                        value: v[2],
                        endian,
                        order,
                    }
                } else if let Some(rest) = text.strip_prefix("store.excl.") {
                    let parts: Vec<&str> = rest.split('.').collect();
                    let (endian, order) = parse_order_endian(c, &m, &parts, at + 11)?;
                    let v = c.values(2, defined)?;
                    Op::StoreExclusive {
                        addr: v[0],
                        value: v[1],
                        endian,
                        order,
                    }
                } else if let Some(rest) = text.strip_prefix("store.") {
                    let parts: Vec<&str> = rest.split('.').collect();
                    let (endian, order) = parse_order_endian(c, &m, &parts, at + 6)?;
                    let v = c.values(2, defined)?;
                    Op::Store {
                        addr: v[0],
                        value: v[1],
                        endian,
                        order,
                    }
                } else {
                    return c.err(at, format!("unknown mnemonic `{text}`"));
                }
            }
        }
    };
    c.end()?;
    Ok((op, at))
}

fn parse_inst(c: &mut Cursor, regs: &RegFile, defined: usize) -> Result<(Inst, usize), ParseError> {
    let first = c.peek().cloned();
    let mut width = None;
    if let Some(t) = first
        && t.kind == Kind::Word
        && let Some((vpart, tpart)) = t.text.split_once(':')
    {
        let n = vpart
            .strip_prefix('v')
            .filter(|d| !d.is_empty() && d.chars().all(|ch| ch.is_ascii_digit()))
            .and_then(|d| d.parse::<usize>().ok());
        match n {
            Some(n) if n == defined => {}
            Some(n) => {
                return c.err(
                    t.col,
                    format!("result v{n} should be v{defined}, values are numbered by position"),
                );
            }
            None => return c.err(t.col, format!("bad result name `{vpart}`")),
        }
        width = Some(c.width_of(tpart, t.col + vpart.chars().count() + 1)?);
        c.pos += 1;
        c.keyword("=")?;
    }
    let (op, at) = parse_op(c, regs, defined)?;
    Ok((Inst { op, width }, at))
}

fn parse_exit(c: &mut Cursor, defined: usize) -> Result<Exit, ParseError> {
    c.keyword("exit")?;
    let ft = c.word("a flow")?;
    let flow = match ft.text.as_str() {
        "jump" => Flow::Jump,
        "return" => Flow::Return,
        "call" => Flow::Call {
            ret: c.hex64("a return address")?,
        },
        "trap" => {
            let t = c.word("a trap code")?;
            let v = c.hex_of(&t, "a trap code")?;
            match u32::try_from(v) {
                Ok(code) => Flow::Trap(code),
                Err(_) => return c.err(t.col, "trap code does not fit 32 bits"),
            }
        }
        other => return c.err(ft.col, format!("unknown flow `{other}`")),
    };
    let t = c.word("an exit target")?;
    let next = match t.text.as_str() {
        "select" => {
            let cond = c.value(defined)?;
            c.comma()?;
            let taken = c.hex64("an address")?;
            c.comma()?;
            let not_taken = c.hex64("an address")?;
            Next::Select {
                cond,
                taken,
                not_taken,
            }
        }
        "dynamic" => Next::Dynamic(c.value(defined)?),
        _ => {
            let v = c.hex_of(&t, "an address")?;
            match u64::try_from(v) {
                Ok(a) => Next::Const(a),
                Err(_) => return c.err(t.col, "address does not fit 64 bits"),
            }
        }
    };
    c.end()?;
    Ok(Exit { next, flow })
}

/// Parses one block from `text`, with register files looked up by name in
/// `files`. See the module documentation for the grammar.
///
/// The block is verified; a type error is reported at the line of the
/// offending instruction (at its mnemonic) or of the exit.
pub fn parse(text: &str, files: &[RegFile]) -> Result<Block, ParseError> {
    let mut header: Option<Header> = None;
    let mut insts: Vec<Inst> = Vec::new();
    let mut locs: Vec<(usize, usize)> = Vec::new();
    let mut exit: Option<(Exit, usize, usize)> = None;
    let mut last_line = 1;
    for (idx, raw) in text.lines().enumerate() {
        let line = idx + 1;
        last_line = line;
        let toks = tokenize(raw, line)?;
        if toks.is_empty() {
            continue;
        }
        let end_col = raw.chars().count() + 1;
        let mut c = Cursor {
            toks,
            pos: 0,
            line,
            end_col,
        };
        if exit.is_some() {
            return c.err(c.toks[0].col, "text after the exit");
        }
        let Some(h) = &header else {
            header = Some(parse_header(&mut c, files)?);
            continue;
        };
        if c.toks[0].text == "exit" && c.toks[0].kind == Kind::Word {
            let col = c.toks[0].col;
            exit = Some((parse_exit(&mut c, insts.len())?, line, col));
        } else {
            let (inst, at) = parse_inst(&mut c, &h.regs, insts.len())?;
            insts.push(inst);
            locs.push((line, at));
        }
    }
    let end = ParseError {
        line: last_line + 1,
        col: 1,
        message: String::new(),
    };
    let Some(h) = header else {
        return Err(ParseError {
            message: "expected a `block` header".to_string(),
            ..end
        });
    };
    let Some((exit, exit_line, exit_col)) = exit else {
        return Err(ParseError {
            message: "expected `exit`".to_string(),
            ..end
        });
    };
    let block = Block {
        regs: h.regs,
        addr_width: h.addr_width,
        pc: h.pc,
        len: h.len,
        insts,
        exit,
        bank: h.bank,
    };
    block.verify().map_err(|e| {
        let (line, col) = match e.at {
            Some(i) => locs[i],
            None => (exit_line, exit_col),
        };
        ParseError {
            line,
            col,
            message: e.message,
        }
    })?;
    Ok(block)
}
