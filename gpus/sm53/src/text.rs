//! The text form of an instruction, in envydis's spelling (envytools
//! `envydis -m gm107 -V sm52`, MIT) so the decoder can be checked against
//! it word for word. Field values come from the form's fields
//! ([`crate::field`]); only the names, their order and the number syntax
//! are envydis's. Forms envydis does not know (the half-precision ones and
//! `TEXS.F16`) and forms no NVN program of the census uses print as the
//! form's name and its fields.
//!
//! One form differs from envydis on purpose: the immediate `LOP3` prints
//! its third source and `X` flag, which Ryujinx decodes (bits 39 to 46 and
//! 57) and envydis does not; a three-input lookup needs the third input.

use std::fmt;

use crate::field::{Form, Kind};
use crate::form::{Op, Src};
use crate::inst::Inst;

const RZ: u64 = 255;
const PT: u64 = 7;

const FCOMP: &[&str] = &[
    "f", "lt", "eq", "le", "gt", "ne", "ge", "num", "nan", "ltu", "equ", "leu", "gtu", "neu",
    "geu", "t",
];
const ICOMP: &[&str] = &["f", "lt", "eq", "le", "gt", "ne", "ge", "t"];
const BOP: &[&str] = &["and", "or", "xor"];
const CCC: &[&str] = &[
    "f", "lt", "eq", "le", "gt", "ne", "ge", "num", "nan", "ltu", "equ", "leu", "gtu", "neu",
    "geu", "", "off", "lo", "sff", "ls", "hi", "sft", "hs", "oft", "csm_ta", "csm_tr", "csm_mx",
    "fcsm_ta", "fcsm_tr", "fcsm_mx", "rle", "rgt",
];
const RND: &[&str] = &["", "rm", "rp", "rz"];
const FMZ: &[&str] = &["", "ftz", "fmz"];
const FTYPE: &[&str] = &["???", "f16", "f32", "f64"];
const ITYPE: &[&str] = &["u8", "u16", "u32", "u64", "s8", "s16", "s32", "s64"];
const LSSIZE: &[&str] = &["u8", "s8", "u16", "s16", "b32", "b64", "b128", "u b128"];
const DIM: &[&str] = &["t1d", "t2d", "t3d", "tcube"];
const SYS: &[(u64, &str)] = &[
    (0x00, "laneid"),
    (0x01, "clock"),
    (0x02, "virtcfg"),
    (0x03, "virtid"),
    (0x04, "pm0"),
    (0x05, "pm1"),
    (0x06, "pm2"),
    (0x07, "pm3"),
    (0x08, "pm4"),
    (0x09, "pm5"),
    (0x0a, "pm6"),
    (0x0b, "pm7"),
    (0x10, "prim_type"),
    (0x11, "invocation_id"),
    (0x12, "y_direction"),
    (0x13, "thread_kill"),
    (0x14, "shader_type"),
    (0x15, "directbewriteaddresslow"),
    (0x16, "directbewriteaddresshigh"),
    (0x17, "directbewriteenabled"),
    (0x18, "machine_id_0"),
    (0x19, "machine_id_1"),
    (0x1a, "machine_id_2"),
    (0x1b, "machine_id_3"),
    (0x1c, "affinity"),
    (0x1d, "invocation_info"),
    (0x1e, "wscalefactor_xy"),
    (0x1f, "wscalefactor_z"),
    (0x20, "tid"),
    (0x21, "tidx"),
    (0x22, "tidy"),
    (0x23, "tidz"),
    (0x24, "cta_param"),
    (0x25, "ctaidx"),
    (0x26, "ctaidy"),
    (0x27, "ctaidz"),
    (0x28, "ntid"),
    (0x29, "cirqueueincrminusone"),
    (0x2a, "nlatc"),
    (0x30, "swinlo"),
    (0x31, "swinsz"),
    (0x32, "smemsz"),
    (0x33, "smembanks"),
    (0x34, "lwinlo"),
    (0x35, "lwinsz"),
    (0x36, "lmemlosz"),
    (0x37, "lmemhioff"),
    (0x38, "eqmask"),
    (0x39, "ltmask"),
    (0x3a, "lemask"),
    (0x3b, "gtmask"),
    (0x3c, "gemask"),
    (0x3d, "regalloc"),
    (0x3e, "ctxaddr"),
    (0x40, "globalerrorstatus"),
    (0x42, "warperrorstatus"),
    (0x43, "warperrorstatusclear"),
    (0x48, "pm_hi0"),
    (0x49, "pm_hi1"),
    (0x4a, "pm_hi2"),
    (0x4b, "pm_hi3"),
    (0x4c, "pm_hi4"),
    (0x4d, "pm_hi5"),
    (0x4e, "pm_hi6"),
    (0x4f, "pm_hi7"),
    (0x50, "clocklo"),
    (0x51, "clockhi"),
    (0x52, "globaltimerlo"),
    (0x53, "globaltimerhi"),
    (0x60, "hwtaskid"),
    (0x61, "circularqueueentryindex"),
    (0x62, "circularqueueentryaddresslow"),
    (0x63, "circularqueueentryaddresshigh"),
];

/// Hex as envydis prints numbers: `0x` and lower case, `-0x` below zero.
fn hex(v: i64) -> String {
    if v < 0 {
        format!("-{:#x}", v.unsigned_abs())
    } else {
        format!("{v:#x}")
    }
}

/// `v`'s low `bits` bits as a signed number.
fn signed(v: u64, bits: u32) -> i64 {
    ((v << (64 - bits)) as i64) >> (64 - bits)
}

/// A memory operand: `space[base+offset]`, the base left out when it is
/// RZ and the offset when it is zero and there is a base; an offset alone
/// prints as an unsigned 64-bit number.
fn mem(space: &str, base: u64, off: i64) -> String {
    match (base == RZ, off) {
        (true, o) => format!("{space}[{:#x}]", o as u64),
        (false, 0) => format!("{space}[$r{base}]"),
        (false, o) if o < 0 => format!("{space}[$r{base}-{:#x}]", o.unsigned_abs()),
        (false, o) => format!("{space}[$r{base}+{o:#x}]"),
    }
}

struct W<'a> {
    i: &'a Inst,
    out: Vec<String>,
}

impl W<'_> {
    fn v(&self, f: &str) -> u64 {
        self.i.get(f).unwrap_or(u64::MAX)
    }

    fn on(&self, f: &str) -> bool {
        self.v(f) == 1
    }

    fn n(&mut self, s: &str) {
        if !s.is_empty() {
            self.out.extend(s.split(' ').map(String::from));
        }
    }

    fn flag(&mut self, f: &str, s: &str) {
        if self.on(f) {
            self.n(s);
        }
    }

    /// The name `names[v]` of field `f`, `???` past the end.
    fn pick(&mut self, f: &str, names: &[&str]) {
        let v = self.v(f) as usize;
        let s = names.get(v).copied().unwrap_or("???");
        self.n(s);
    }

    fn r_val(&mut self, v: u64) {
        self.out.push(if v == RZ {
            "0x0".into()
        } else {
            format!("$r{v}")
        });
    }

    fn r(&mut self, f: &str) {
        let v = self.v(f);
        self.r_val(v);
    }

    fn p_val(&mut self, v: u64) {
        self.out.push(if v == PT {
            "0x1".into()
        } else {
            format!("$p{v}")
        });
    }

    fn p(&mut self, f: &str) {
        let v = self.v(f);
        self.p_val(v);
    }

    /// A source predicate with its negation flag.
    fn sp(&mut self, f: &str, inv: &str) {
        self.flag(inv, "not");
        self.p(f);
    }

    fn x(&mut self, v: i64) {
        self.out.push(hex(v));
    }

    fn u(&mut self, f: &str) {
        let v = self.v(f);
        self.x(v as i64);
    }

    fn cbuf(&mut self) {
        let off = signed(self.v("CbufOffset"), 14) << 2;
        let s = mem(&format!("c{}", self.v("CbufSlot")), RZ, off);
        self.out.push(s);
    }

    /// The operand the form varies: a register, a 20-bit immediate (as
    /// an integer, or as the top of a float when `float`), or a constant
    /// bank entry.
    fn b(&mut self, float: bool) {
        match self.i.enc.src {
            Src::Reg => self.r("SrcB"),
            Src::Imm => {
                let v = self.v("Imm20");
                if float {
                    self.x((v << 12) as i64);
                } else {
                    self.x(signed(v, 20));
                }
            }
            Src::CBuf => self.cbuf(),
            Src::RegCBuf => self.r("SrcC"),
            Src::None | Src::Imm32 => self.n("???"),
        }
    }

    /// The third operand: `SrcC`, or the constant bank entry of the
    /// register and constant form.
    fn c(&mut self) {
        if self.i.enc.src == Src::RegCBuf {
            self.cbuf();
        } else {
            self.r("SrcC");
        }
    }

    fn target(&mut self, f: &str) {
        if self.on("Ca") {
            let off = signed((self.i.word >> 20) & 0xffff, 16);
            let slot = (self.i.word >> 36) & 0x1f;
            self.out.push(mem(&format!("c{slot}"), RZ, off));
        } else {
            let t = self.i.at as i64 + 8 + signed(self.v(f), 24);
            self.out.push(format!("{:#x}", t as u64));
        }
    }

    fn ccc(&mut self) {
        self.pick("Ccc", CCC);
    }

    fn texdim(&mut self) {
        let d = self.v("Dim");
        if d & 1 == 1 {
            self.n("array");
        }
        self.n(DIM[(d >> 1) as usize & 3]);
    }

    fn sign(&mut self, f: &str) {
        if !self.on(f) {
            self.n("u32");
        }
    }
}

/// Lod names of `TEXS` by its target field, as envydis gives them.
const TEXS_LOD: &[&str] = &[
    "lz", "", "lz", "ll", "dc", "ll dc", "lz dc", "", "lz", "lz dc", "", "lz", "", "ll",
];
const TEXS_DIM: &[&str] = &[
    "t1d", "t2d", "t2d", "t2d", "t2d", "t2d", "t2d", "a2d", "a2d", "a2d", "t3d", "t3d", "tcube",
    "tcube",
];
const TLDS_LOD: &[&str] = &[
    "lz", "ll", "lz", "???", "lz aoffi", "ll", "lz mz", "lz", "lz", "???", "???", "???", "ll aoffi",
];
const TLDS_DIM: &[&str] = &[
    "t1d", "t1d", "t2d", "???", "t2d", "t2d", "t2d", "t3d", "a2d", "???", "???", "???", "t2d",
];

/// The channel mask of the compact texture forms: from the mask field
/// alone with one destination pair, else with both.
fn compact_mask(w: &mut W, mask: &str, dest2: &str) {
    let m = w.v(mask) as usize;
    let s = if w.v(dest2) == RZ {
        ["r", "g", "b", "a", "rg", "ra", "ga", "ba"].get(m)
    } else {
        ["rgb", "rga", "rba", "gba", "rgba"].get(m)
    };
    w.n(s.copied().unwrap_or("???"));
}

impl fmt::Display for Inst {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let mut w = W {
            i: self,
            out: Vec::new(),
        };
        if let Some((p, inv)) = self.guard() {
            match (u64::from(p) == PT, inv) {
                (true, false) => {}
                (true, true) => w.n("never"),
                (false, _) => {
                    if inv {
                        w.n("not");
                    }
                    w.p_val(u64::from(p));
                }
            }
        }
        let guard = w.out.len();
        if !spell(&mut w) {
            w.out.truncate(guard);
            w.n(&self.op().name().to_ascii_lowercase());
            for fl in self.form().fields() {
                if fl.name == "Pred" || fl.name == "PredInv" {
                    continue;
                }
                let v = fl.get(self.word);
                let s = match fl.kind {
                    Kind::Enum(e) => e.value_name(v).map_or_else(|| hex(v as i64), String::from),
                    _ => hex(v as i64),
                };
                w.out.push(format!("{}={s}", fl.name));
            }
        }
        f.write_str(&w.out.join(" "))
    }
}

/// Writes the instruction in envydis's spelling; false when there is none.
fn spell(w: &mut W) -> bool {
    let i = w.i;
    let src = i.enc.src;
    match i.op() {
        Op::Ffma if src == Src::Imm32 => {
            w.n("ffma32i");
            w.pick("Fmz", FMZ);
            w.flag("Sat", "sat");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.flag("NegA", "neg");
            w.r("SrcA");
            w.u("Imm32");
            w.flag("NegC", "neg");
            w.r("Dest");
        }
        Op::Ffma => {
            w.n("ffma");
            w.pick("Fmz", FMZ);
            w.pick("RoundMode", RND);
            w.flag("Sat", "sat");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.r("SrcA");
            w.flag("NegA", "neg");
            w.b(true);
            w.flag("NegC", "neg");
            w.c();
        }
        Op::Fadd if src == Src::Imm32 => {
            w.n("fadd32i");
            w.flag("Ftz", "ftz");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.flag("NegA", "neg");
            w.flag("AbsA", "abs");
            w.r("SrcA");
            w.flag("NegB", "neg");
            w.flag("AbsB", "abs");
            w.u("Imm32");
        }
        Op::Fadd => {
            w.n("fadd");
            w.flag("Ftz", "ftz");
            w.flag("Sat", "sat");
            w.pick("RoundMode", RND);
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.flag("NegA", "neg");
            w.flag("AbsA", "abs");
            w.r("SrcA");
            w.flag("NegB", "neg");
            w.flag("AbsB", "abs");
            w.b(true);
        }
        Op::Fmul if src == Src::Imm32 => {
            w.n("fmul32i");
            w.pick("Fmz", FMZ);
            w.flag("Sat", "sat");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.r("SrcA");
            w.u("Imm32");
        }
        Op::Fmul => {
            w.n("fmul");
            w.pick("Fmz", FMZ);
            w.pick("Scale", &["", "d2", "d4", "d8", "m8", "m4", "m2", "???"]);
            w.pick("RoundMode", RND);
            w.flag("Sat", "sat");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.r("SrcA");
            w.flag("NegA", "neg");
            w.b(true);
        }
        Op::Fmnmx => {
            w.n("fmnmx");
            w.flag("Ftz", "ftz");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.flag("NegA", "neg");
            w.flag("AbsA", "abs");
            w.r("SrcA");
            w.flag("NegB", "neg");
            w.flag("AbsB", "abs");
            w.b(true);
            w.sp("SrcPred", "SrcPredInv");
        }
        Op::Mov if src == Src::Imm32 => {
            w.n("mov32i");
            w.r("Dest");
            w.u("Imm32");
            w.u("QuadMask");
        }
        Op::Mov => {
            w.n("mov");
            w.r("Dest");
            match src {
                Src::Reg => w.r("SrcA"),
                _ => w.b(false),
            }
            w.u("QuadMask");
        }
        Op::S2r => {
            w.n("mov");
            w.r("Dest");
            let v = w.v("SReg");
            let s = SYS
                .iter()
                .find(|&&(k, _)| k == v)
                .map_or_else(|| format!("$s{v}"), |&(_, n)| format!("${n}"));
            w.out.push(s);
        }
        Op::Mufu => {
            w.n("mufu");
            w.pick(
                "MufuOp",
                &[
                    "cos", "sin", "ex2", "lg2", "rcp", "rsq", "rcp64h", "rsq64h", "sqrt",
                ],
            );
            w.flag("Sat", "sat");
            w.r("Dest");
            w.flag("NegA", "neg");
            w.flag("AbsA", "abs");
            w.r("SrcA");
        }
        Op::Rro => {
            w.n("rro");
            w.n(if w.on("RroOp") { "ex2" } else { "sincos" });
            w.r("Dest");
            w.flag("NegB", "neg");
            w.flag("AbsB", "abs");
            w.b(true);
        }
        Op::Ipa => {
            w.n("ipa");
            if w.on("Idx") {
                w.n("idx");
            }
            w.pick("IpaOp", &["pass", "", "constant", "sc"]);
            w.pick("Msi", &["", "centroid", "offset", "???"]);
            w.flag("Sat", "sat");
            w.r("Dest");
            let base = w.v("SrcA");
            let off = if w.on("Idx") { 0 } else { w.v("Imm10") as i64 };
            w.out.push(mem("a", base, off));
            w.r("SrcB");
            w.r("SrcC");
            w.sp("SrcPred", "SrcPredInv");
        }
        Op::Ald => {
            w.n("ld");
            w.flag("O", "o");
            w.flag("P", "p");
            w.pick("AlSize", &["b32", "b64", "b96", "b128"]);
            w.r("Dest");
            let (base, off) = (w.v("SrcA"), w.v("Imm11") as i64);
            w.out.push(mem("a", base, off));
            w.r("SrcB");
        }
        Op::Ast => {
            w.n("st");
            w.flag("P", "p");
            w.pick("AlSize", &["b32", "b64", "b96", "b128"]);
            let (base, off) = (w.v("SrcA"), w.v("Imm11") as i64);
            w.out.push(mem("a", base, off));
            w.r("SrcB");
            w.r("SrcC");
        }
        Op::Ldc => {
            w.n("ld");
            w.pick("AddressMode", &["", "il", "is", "isl"]);
            w.pick("LsSize", LSSIZE);
            w.r("Dest");
            let s = format!("c{}", w.v("CbufSlot"));
            let (base, off) = (w.v("SrcA"), signed(w.v("CbufOffset"), 16));
            w.out.push(mem(&s, base, off));
        }
        Op::Ldg => {
            w.n("ldg");
            w.flag("E", "e");
            w.pick("CacheOp", &["", "cg", "ci", "cv"]);
            w.pick("LsSize", LSSIZE);
            w.r("Dest");
            let (base, off) = (w.v("SrcA"), signed(w.v("Imm24"), 24));
            w.out.push(mem("ncg", base, off));
        }
        Op::Stg => {
            w.n("stg");
            w.flag("E", "e");
            w.pick("CacheOp", &["", "cg", "cs", "wt"]);
            w.pick("LsSize", &LSSIZE[..7]);
            let (base, off) = (w.v("SrcA"), signed(w.v("Imm24"), 24));
            w.out.push(mem("ncg", base, off));
            w.r("Dest");
        }
        Op::Ldl => {
            w.n("ld");
            w.pick("CacheOp", &["", "lu", "ci", "cv"]);
            w.pick("LsSize", &LSSIZE[..7]);
            w.r("Dest");
            let (base, off) = (w.v("SrcA"), signed(w.v("Imm24"), 24));
            w.out.push(mem("l", base, off));
        }
        Op::Lds => {
            w.n("ld");
            w.flag("U", "u");
            w.pick("LsSize", &LSSIZE[..7]);
            w.r("Dest");
            let (base, off) = (w.v("SrcA"), signed(w.v("Imm24"), 24));
            w.out.push(mem("s", base, off));
        }
        Op::Sts => {
            w.n("st");
            w.pick("LsSize", &LSSIZE[..7]);
            let (base, off) = (w.v("SrcA"), signed(w.v("Imm24"), 24));
            w.out.push(mem("s", base, off));
            w.r("Dest");
        }
        Op::Stl => {
            w.n("st");
            w.pick("CacheOp", &["", "cg", "cs", "wt"]);
            w.pick("LsSize", &LSSIZE[..7]);
            let (base, off) = (w.v("SrcA"), signed(w.v("Imm24"), 24));
            w.out.push(mem("l", base, off));
            w.r("Dest");
        }
        Op::F2f => {
            w.n("f2f");
            w.flag("Ftz", "ftz");
            w.pick("SrcFmt", FTYPE);
            w.pick("DstFmt", FTYPE);
            w.flag("Sh", "sh");
            w.pick(
                "RoundMode",
                &["", "???", "???", "pass", "round", "floor", "ceil", "trunc"],
            );
            w.flag("Sat", "sat");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.flag("NegB", "neg");
            w.flag("AbsB", "abs");
            w.b(true);
        }
        Op::F2i => {
            w.n("f2i");
            w.flag("Ftz", "ftz");
            w.pick(
                "IDstFmt",
                &["u8", "u16", "u32", "u64", "s8", "s16", "s32", "u64"],
            );
            w.pick("SrcFmt", FTYPE);
            w.pick("RoundMode", &["", "floor", "ceil", "trunc"]);
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.flag("NegB", "neg");
            w.flag("AbsB", "abs");
            w.b(true);
        }
        Op::I2f => {
            w.n("i2f");
            w.pick("DstFmt", FTYPE);
            w.pick("ISrcFmt", ITYPE);
            w.pick("RoundMode", RND);
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.flag("NegB", "neg");
            w.flag("AbsB", "abs");
            w.pick("ByteSel", &["", "b1", "b2", "b3"]);
            w.b(false);
        }
        Op::I2i => {
            let t = &["u8", "u16", "u32", "???", "s8", "s16", "s32", "???"];
            w.n("i2i");
            w.pick("IDstFmt", t);
            w.pick("ISrcFmt", t);
            w.flag("Sat", "sat");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.pick("ByteSel", &["", "b1", "b2", "b3"]);
            w.flag("NegB", "neg");
            w.flag("AbsB", "abs");
            w.b(false);
        }
        Op::Fset => {
            w.n("fset");
            w.flag("BVal", "bf");
            w.pick("FComp", FCOMP);
            w.pick("Bop", BOP);
            w.flag("Ftz", "ftz");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.flag("NegA", "neg");
            w.flag("AbsA", "abs");
            w.r("SrcA");
            w.flag("NegB", "neg");
            w.flag("AbsB", "abs");
            w.b(true);
            w.sp("SrcPred", "SrcPredInv");
        }
        Op::Fsetp => {
            w.n("fsetp");
            w.pick("FComp", FCOMP);
            w.flag("Ftz", "ftz");
            w.pick("Bop", BOP);
            w.p("DestPred");
            w.p("DestPredInv");
            w.flag("NegA", "neg");
            w.flag("AbsA", "abs");
            w.r("SrcA");
            w.flag("NegB", "neg");
            w.flag("AbsB", "abs");
            w.b(true);
            w.sp("SrcPred", "SrcPredInv");
        }
        Op::Isetp => {
            w.n("isetp");
            w.pick("IComp", ICOMP);
            w.sign("Signed");
            w.flag("X", "x");
            w.pick("Bop", BOP);
            w.p("DestPred");
            w.p("DestPredInv");
            w.r("SrcA");
            w.b(false);
            w.sp("SrcPred", "SrcPredInv");
        }
        Op::Iset => {
            w.n("iset");
            w.flag("BVal", "bf");
            w.pick("IComp", ICOMP);
            w.sign("Signed");
            w.flag("X", "x");
            w.pick("Bop", BOP);
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.r("SrcA");
            w.b(false);
            w.sp("SrcPred", "SrcPredInv");
        }
        Op::Iadd if src == Src::Imm32 => {
            let avg = w.v("AvgMode");
            w.n("iadd32i");
            if avg == 3 {
                w.n("po");
            }
            w.flag("Sat", "sat");
            w.flag("X", "x");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            if avg == 2 {
                w.n("neg");
            }
            w.r("SrcA");
            let v = w.v("Imm32");
            w.x(signed(v, 32));
        }
        Op::Iadd => {
            let avg = w.v("AvgMode");
            w.n("iadd");
            w.flag("Sat", "sat");
            w.flag("X", "x");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            if avg & 2 != 0 {
                w.n("neg");
            }
            w.r("SrcA");
            if avg & 1 != 0 {
                w.n("neg");
            }
            w.b(false);
        }
        Op::Iadd3 if src == Src::Reg => {
            w.n("iadd3");
            w.pick("Lrs", &["", "rs", "ls", "???"]);
            w.flag("X", "x");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            let part = &["", "h0", "h1", "???"];
            w.flag("NegA", "neg");
            w.pick("Apart", part);
            w.r("SrcA");
            w.flag("NegB", "neg");
            w.pick("Bpart", part);
            w.r("SrcB");
            w.flag("NegC", "neg");
            w.pick("Cpart", part);
            w.r("SrcC");
        }
        Op::Iadd3 => {
            w.n("iadd3");
            w.flag("X", "x");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.flag("NegA", "neg");
            w.r("SrcA");
            w.flag("NegB", "neg");
            w.b(false);
            w.flag("NegC", "neg");
            w.r("SrcC");
        }
        Op::Iscadd if src != Src::Imm32 => {
            let avg = w.v("AvgMode");
            w.n("iscadd");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            if avg & 2 != 0 {
                w.n("neg");
            }
            w.r("SrcA");
            if avg & 1 != 0 {
                w.n("neg");
            }
            w.b(false);
            w.u("Imm5");
        }
        Op::Imnmx => {
            w.n("imnmx");
            w.sign("Signed");
            w.pick("XMode", &["", "xlo", "xmed", "xhi"]);
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.r("SrcA");
            w.b(false);
            w.sp("SrcPred", "SrcPredInv");
        }
        Op::Bfe => {
            w.n("bfe");
            w.sign("Signed");
            w.flag("Brev", "brev");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.r("SrcA");
            w.b(false);
        }
        Op::Bfi => {
            w.n("bfi");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.r("SrcA");
            w.b(false);
            w.c();
        }
        Op::Flo => {
            w.n("flo");
            w.sign("Signed");
            w.flag("Sh", "sh");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.flag("NegB", "inv");
            w.b(false);
        }
        Op::Popc => {
            w.n("popc");
            w.r("Dest");
            w.flag("NegB", "inv");
            w.b(false);
        }
        Op::Sel => {
            w.n("sel");
            w.r("Dest");
            w.r("SrcA");
            w.b(false);
            w.sp("SrcPred", "SrcPredInv");
        }
        Op::Shl => {
            w.n("shl");
            w.flag("M", "w");
            w.flag("X", "x");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.r("SrcA");
            w.b(false);
        }
        Op::Shr => {
            w.n("shr");
            w.sign("Signed");
            w.flag("M", "w");
            if w.v("XMode") & 2 != 0 {
                w.n("x");
            }
            w.flag("Brev", "brev");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.r("SrcA");
            w.b(false);
        }
        Op::Lop if src == Src::Imm32 => {
            w.n("lop32i");
            w.pick("LogicOp", &["and", "or", "xor", "pass_b"]);
            w.flag("X", "x");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.flag("NegA", "inv");
            w.r("SrcA");
            w.flag("NegB", "inv");
            w.u("Imm32");
        }
        Op::Lop => {
            let lop = if src == Src::Reg { "Lop" } else { "LogicOp" };
            w.n("lop");
            w.pick(lop, &["and", "or", "xor", "pass_b"]);
            w.flag("X", "x");
            w.pick("PredicateOp", &["", "t", "z", "nz"]);
            w.p("DestPred");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.flag("NegA", "inv");
            w.r("SrcA");
            w.flag("NegB", "inv");
            w.b(false);
        }
        Op::Lop3 if src == Src::Reg => {
            w.n("lop3 lut");
            w.flag("X", "x");
            w.pick("PredicateOp", &["", "t", "z", "nz"]);
            w.p("DestPred");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.r("SrcA");
            w.r("SrcB");
            w.r("SrcC");
            w.u("Imm");
        }
        Op::Lop3 if src == Src::Imm => {
            w.n("lop3 lut");
            w.flag("X", "x");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.r("SrcA");
            w.b(false);
            w.r("SrcC");
            w.u("Imm");
        }
        Op::Lop3 if src == Src::CBuf => {
            w.n("lop3 lut");
            w.flag("X", "x");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.r("SrcA");
            w.cbuf();
            w.r("SrcC");
            w.u("Imm");
        }
        Op::Xmad => {
            let rc = src == Src::RegCBuf;
            w.n("xmad");
            let s = w.v("ASigned") | (w.v("BSigned") << 1);
            w.n(["", "s16 u16", "u16 s16", "s16 s16"][s as usize & 3]);
            if !rc {
                w.flag("Psl", "psl");
                w.flag("Mrg", "mrg");
                w.pick(
                    "XmadCop",
                    &["", "clo", "chi", "csfu", "cbcc", "???", "???", "???"],
                );
            } else {
                w.pick("XmadCop", &["", "clo", "chi", "csfu"]);
            }
            w.flag("X", "x");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.flag("HiloA", "h1");
            w.r("SrcA");
            match src {
                Src::Imm => w.u("Imm16"),
                Src::RegCBuf => {
                    w.flag("HiloB", "h1");
                    w.r("SrcC");
                    w.cbuf();
                    return true;
                }
                Src::CBuf => {
                    w.flag("HiloB", "h1");
                    w.cbuf();
                }
                _ => {
                    w.flag("HiloB", "h1");
                    w.r("SrcB");
                }
            }
            w.r("SrcC");
        }
        Op::Fswzadd => {
            w.n("fswzadd");
            w.flag("Ftz", "ftz");
            w.pick("RoundMode", RND);
            w.flag("Ndv", "ndv");
            w.flag("WriteCC", "cc");
            w.r("Dest");
            w.r("SrcA");
            w.r("SrcB");
            w.u("PnWord");
        }
        Op::Csetp => {
            w.n("csetp");
            w.ccc();
            w.pick("Bop", BOP);
            w.p("DestPred");
            w.p("DestPredInv");
            w.n("cc");
            w.sp("SrcPred", "SrcPredInv");
        }
        Op::Psetp | Op::Pset => {
            let set = i.op() == Op::Pset;
            w.n(if set { "pset" } else { "psetp" });
            if set {
                w.flag("BVal", "bf");
            }
            w.pick("BoolOpAB", BOP);
            w.pick("BoolOpC", BOP);
            if set {
                w.flag("WriteCC", "cc");
                w.r("Dest");
            } else {
                w.p("DestPred");
                w.p("DestPredInv");
            }
            w.sp("Src2Pred", "Src2PredInv");
            w.sp("Src1Pred", "Src1PredInv");
            w.sp("SrcPred", "SrcPredInv");
        }
        Op::Vote => {
            w.n("vote");
            w.pick("VoteMode", &["all", "any", "eq"]);
            w.r("Dest");
            w.p("VpDest");
            w.sp("SrcPred", "SrcPredInv");
        }
        Op::Nop => {
            w.n("nop");
            w.flag("Trig", "trig");
            w.ccc();
            w.u("Imm16");
        }
        Op::Depbar => {
            w.n("depbar");
            w.flag("Le", "le");
            w.u("Sbid");
            w.u("PendCnt");
            w.u("Imm6");
        }
        Op::Bra => {
            w.n("bra");
            w.flag("U", "u");
            w.flag("Lmt", "lmt");
            w.ccc();
            w.target("Imm24");
        }
        Op::Brx => {
            w.n("brx");
            w.flag("Lmt", "lmt");
            w.ccc();
            if w.on("Ca") {
                let off = signed((i.word >> 20) & 0xffff, 16);
                let s = format!("c{}", (i.word >> 36) & 0x1f);
                let base = w.v("SrcA");
                w.out.push(mem(&s, base, off));
            } else {
                w.r("SrcA");
                let v = w.v("Imm24");
                w.x(signed(v, 24));
            }
        }
        Op::Ssy | Op::Pbk => {
            w.n(if i.op() == Op::Ssy { "ssy" } else { "pbk" });
            w.target("Imm24");
        }
        Op::Cal => {
            w.n("cal");
            if !w.on("Inc") {
                w.n("noinc");
            }
            w.target("Imm24");
        }
        Op::Sync | Op::Brk | Op::Kil | Op::Ret | Op::Exit => {
            w.n(&i.op().name().to_ascii_lowercase());
            if i.op() == Op::Exit {
                w.flag("KeepRefCnt", "keeprefcount");
            }
            w.ccc();
        }
        Op::Membar => {
            w.n("membar");
            w.pick("Membar", &["cta", "gl", "sys", "vc"]);
            w.pick("Ivall", &["", "ivalld", "ivallt", "ivalltd"]);
        }
        Op::Red => {
            w.n("red");
            w.pick(
                "RedOp",
                &["add", "min", "max", "inc", "dec", "and", "or", "xor"],
            );
            w.flag("E", "e");
            w.pick(
                "RedSize",
                &["u32", "s32", "u64", "ftz rn f32", "u128", "s64"],
            );
            let (base, off) = (w.v("SrcA"), signed(w.v("Imm20"), 20));
            w.out.push(mem("g", base, off));
            w.r("SrcB");
        }
        Op::Atom => {
            w.n("atom");
            let op = w.v("Op") as usize;
            let ops = [
                "add", "min", "max", "inc", "dec", "and", "or", "xor", "exch", "???", "safeadd",
            ];
            w.n(ops.get(op).copied().unwrap_or("???"));
            w.flag("E", "e");
            w.pick("Size", &["u32", "s32", "u64", "ftz rn f32", "u128", "s64"]);
            w.r("Dest");
            let (base, off) = (w.v("SrcA"), signed(w.v("Imm20"), 20));
            w.out.push(mem("g", base, off));
            w.r("SrcB");
        }
        Op::Out => {
            w.n("out");
            w.pick("OutType", &["???", "emit", "cut", "emit_then_cut"]);
            w.r("Dest");
            w.r("SrcA");
            w.b(false);
        }
        Op::Shfl => {
            w.n("shfl");
            w.pick("ShflMode", &["idx", "up", "down", "bfly"]);
            w.p("DestPred");
            w.r("Dest");
            w.r("SrcA");
            if w.on("BFixShfl") {
                w.u("SrcBImm");
            } else {
                w.r("SrcB");
            }
            if w.on("CFixShfl") {
                w.u("SrcCImm");
            } else {
                w.r("SrcC");
            }
        }
        Op::Tex | Op::TexB => tex(w),
        Op::Texs | Op::Tlds => {
            let tlds = i.op() == Op::Tlds;
            w.n(if tlds { "tlds" } else { "texs" });
            let t = w.v("Target") as usize;
            let lod = if tlds { TLDS_LOD } else { TEXS_LOD };
            w.n(lod.get(t).copied().unwrap_or("???"));
            w.flag("Nodep", "nodep");
            w.r("Dest2");
            w.r("Dest");
            w.r("SrcA");
            w.r("SrcB");
            w.u("TidB");
            let dim = if tlds { TLDS_DIM } else { TEXS_DIM };
            w.n(dim.get(t).copied().unwrap_or("???"));
            compact_mask(w, "WMask", "Dest2");
        }
        Op::Tld4s => {
            w.n("tld4s");
            w.pick("TexComp", &["r", "g", "b", "a"]);
            w.flag("Aoffi", "aoffi");
            w.flag("Dc", "dc");
            w.flag("Nodep", "nodep");
            w.r("Dest2");
            w.r("Dest");
            w.r("SrcA");
            w.r("SrcB");
            w.u("TidB");
        }
        Op::Tld4 | Op::Tld4B => {
            w.n("tld4");
            w.pick("TexComp", &["r", "g", "b", "a"]);
            let b = i.form() == Form::Tld4B;
            if b {
                w.n("b");
            }
            w.pick("Toff", &["", "aoffi", "ptp", "???"]);
            w.flag("Dc", "dc");
            w.flag("Ndv", "ndv");
            w.flag("Nodep", "nodep");
            w.r("Dest");
            w.r("SrcA");
            w.r("SrcB");
            if b {
                w.n("0x0");
            } else {
                w.u("TidB");
            }
            w.texdim();
            w.u("WMask");
        }
        Op::Tld | Op::TldB => {
            let b = i.op() == Op::TldB;
            w.n("tld");
            if b {
                w.n("b");
            }
            w.n(if w.on("Lod") { "ll" } else { "lz" });
            w.flag("Toff", "aoffi");
            w.flag("Ms", "ms");
            w.flag("Cl", "cl");
            w.flag("Nodep", "nodep");
            w.r("Dest");
            w.r("SrcA");
            w.r("SrcB");
            if b {
                w.n("0x0");
            } else {
                w.u("TidB");
            }
            w.texdim();
            w.u("WMask");
        }
        Op::Tmml | Op::TmmlB => {
            let b = i.op() == Op::TmmlB;
            w.n("tmml");
            if b {
                w.n("b");
            }
            w.n("lod");
            w.flag("Ndv", "ndv");
            w.flag("Nodep", "nodep");
            w.r("Dest");
            w.r("SrcA");
            if b {
                w.r("SrcB");
                w.n("0x0");
            } else {
                w.u("TidB");
            }
            w.texdim();
            w.u("WMask");
        }
        Op::Txq | Op::TxqB => {
            let b = i.op() == Op::TxqB;
            w.n("txq");
            if b {
                w.n("b");
            }
            w.flag("Nodep", "nodep");
            w.r("Dest");
            w.r("SrcA");
            let q = w.v("TexQuery");
            let names = [
                (1, "dimension"),
                (2, "texture_type"),
                (5, "sample_pos"),
                (0x10, "filter"),
                (0x12, "lod"),
                (0x14, "wrap"),
                (0x16, "border_color"),
            ];
            w.n(names
                .iter()
                .find(|&&(k, _)| k == q)
                .map_or("???", |&(_, n)| n));
            if b {
                let v = (i.word >> 36) & 0x1fff;
                w.x(v as i64);
            } else {
                w.u("TidB");
            }
            w.u("WMask");
        }
        Op::Suld | Op::SuldD | Op::Sust | Op::SustD => surface(w),
        _ => return false,
    }
    true
}

/// `TEX` and `TEX.B`.
fn tex(w: &mut W) {
    let b = w.i.op() == Op::TexB;
    w.n("tex");
    let lods = &["", "lz", "lb", "ll", "???", "???", "lba", "lla"];
    if b {
        w.n("b");
        w.pick("Lodb", lods);
        w.flag("Aoffib", "aoffi");
    } else {
        w.pick("Lod", lods);
        w.flag("Aoffi", "aoffi");
    }
    w.flag("Dc", "dc");
    if b {
        w.flag("Ndv", "ndv");
    }
    w.flag("Nodep", "nodep");
    w.r("Dest");
    w.r("SrcA");
    w.r("SrcB");
    if b {
        w.n("0x0");
    } else {
        w.u("TidB");
    }
    w.texdim();
    w.u("WMask");
}

/// `SULD` and `SUST`, typed (`D`) or by channels (`P`).
fn surface(w: &mut W) {
    let i = w.i;
    let st = matches!(i.op(), Op::Sust | Op::SustD);
    let d = (i.word >> 52) & 1 == 1;
    w.n(if st { "sust" } else { "suld" });
    w.n(if d { "d" } else { "p" });
    if st && d && w.on("Ba") {
        w.n("ba");
    }
    w.pick(
        "Dim",
        &["t1d", "b1d", "a1d", "t2d", "a2d", "t3d", "???", "???"],
    );
    if d {
        w.pick(
            "Size",
            &["u8", "s8", "u16", "s16", "b32", "b64", "b128", "???"],
        );
    } else {
        let rgba = [
            "???", "r", "g", "rg", "b", "rb", "gb", "rgb", "a", "ra", "ga", "rga", "ba", "rba",
            "gba", "rgba",
        ];
        w.pick("Rgba", &rgba);
    }
    if st {
        w.pick("CacheOp", &["", "cg", "cs", "wt"]);
    } else {
        w.pick("CacheOp", &["", "cg", "ci", "cv"]);
    }
    w.pick("Clamp", &["ign", "", "trap", "???"]);
    let base = w.v("SrcA");
    if !st {
        w.r("Dest");
    }
    w.out.push(mem("g", base, 0));
    if st {
        w.r("Dest");
    }
    if (i.word >> 51) & 1 == 1 {
        w.u("TidB");
    } else {
        w.r("SrcC");
    }
}
