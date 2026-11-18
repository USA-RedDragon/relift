#!/usr/bin/env python3
"""Writes src/form.rs and src/field.rs from Ryujinx's shader decoder tables
(MIT, Copyright (c) Ryujinx Team and Contributors), plus the three
encodings only envytools lists.

    gen.py <Ryujinx checkout>

The committed files come from Ryujinx 6af327bdd50366163d80bc692f6bf47ec1c1993e
(src/Ryujinx.Graphics.Shader/Decoders/InstTable.cs and InstDecoders.cs).
Run `cargo fmt -p relift-gpu-sm53` afterwards.
"""
import os
import re
import sys

R = os.path.join(sys.argv[1], "src/Ryujinx.Graphics.Shader/Decoders/")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "src/")
FORM_HEAD = r"""//! The Maxwell instruction forms: every opcode pattern, the instruction it
//! encodes and the form of its last source operand.
//!
//! The patterns are Ryujinx's `InstTable` (MIT, Copyright (c) Ryujinx Team
//! and Contributors), most significant bit first, with three encodings only
//! envytools' `gm107.c` (MIT) lists: `LDG` 0xeec8, `SURED` 0xeb40 and
//! `SUATOM` 0xea60. Where several patterns match a word, the one with the
//! most fixed bits decides.

use std::sync::OnceLock;

use crate::field::Form;

/// An instruction, whatever the form of its operands.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
"""
FORM_MID = r"""
impl Op {
    /// The instruction's name in capitals, with `_` between the words of
    /// the variant (`TEXS_F16`, `SHF_L`).
    pub fn name(self) -> &'static str {
        names(self)
    }
}

/// The form of an instruction's last source operand.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Src {
    /// The instruction has one encoding with no operand variants.
    None,
    /// A register.
    Reg,
    /// A 20-bit immediate.
    Imm,
    /// A constant bank entry.
    CBuf,
    /// A register as the second source and a constant bank entry as the third.
    RegCBuf,
    /// A 32-bit immediate (the `32I` encodings).
    Imm32,
}

/// One encoding: the bits a word must have, and what it then is.
#[derive(Clone, Copy, Debug)]
pub struct Encoding {
    pub op: Op,
    pub src: Src,
    pub form: Form,
    pub mask: u64,
    pub bits: u64,
}

impl Encoding {
    /// Whether `w` has this encoding's fixed bits.
    pub fn matches(&self, w: u64) -> bool {
        w & self.mask == self.bits
    }

    /// How many bits the encoding fixes.
    pub fn fixed(&self) -> u32 {
        self.mask.count_ones()
    }
}

const fn enc(p: &[u8; 64], op: Op, src: Src, form: Form) -> Encoding {
    let mut mask = 0u64;
    let mut bits = 0u64;
    let mut i = 0;
    while i < 64 {
        let b = 1u64 << (63 - i);
        if p[i] != b'x' {
            mask |= b;
        }
        if p[i] == b'1' {
            bits |= b;
        }
        i += 1;
    }
    Encoding {
        op,
        src,
        form,
        mask,
        bits,
    }
}

/// Top bits of a word the lookup indexes by.
const INDEX_BITS: u32 = 14;

/// The encoding of `w`, or none when no pattern matches.
pub fn classify(w: u64) -> Option<&'static Encoding> {
    index()[(w >> (64 - INDEX_BITS)) as usize]
        .iter()
        .map(|&k| &ENCODINGS[usize::from(k)])
        .find(|e| e.matches(w))
}

/// Per value of the top bits, the encodings that can match it, most fixed
/// bits first.
fn index() -> &'static [Vec<u16>] {
    static INDEX: OnceLock<Vec<Vec<u16>>> = OnceLock::new();
    INDEX.get_or_init(|| {
        let top = |v: u64| v >> (64 - INDEX_BITS);
        (0..1u64 << INDEX_BITS)
            .map(|t| {
                let mut ks: Vec<u16> = (0..ENCODINGS.len() as u16)
                    .filter(|&k| {
                        let e = &ENCODINGS[usize::from(k)];
                        t & top(e.mask) == top(e.bits)
                    })
                    .collect();
                ks.sort_by_key(|&k| std::cmp::Reverse(ENCODINGS[usize::from(k)].fixed()));
                ks
            })
            .collect()
    })
}

"""
FIELD_HEAD = r"""//! The fields of each instruction form: where each operand and modifier
//! sits in the word, from Ryujinx's `InstDecoders` (MIT, Copyright (c)
//! Ryujinx Team and Contributors). The three encodings only envytools lists
//! take the fields of Ryujinx's form of the same instruction.
//!
//! A field's value is the OR of its parts, each `(word >> shift) & mask`.

/// What a field holds.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    /// An unsigned integer: a register, an immediate, a count.
    Int,
    /// A flag.
    Bool,
    /// A value of an enumeration.
    Enum(Enum),
}

/// One field of a form.
#[derive(Clone, Copy, Debug)]
pub struct Field {
    pub name: &'static str,
    pub kind: Kind,
    pub parts: &'static [(u8, u64)],
}

impl Field {
    /// The field's value in `w`: 0 or 1 for a flag.
    pub fn get(&self, w: u64) -> u64 {
        let v = self
            .parts
            .iter()
            .fold(0, |v, &(shift, mask)| v | ((w >> shift) & mask));
        match self.kind {
            Kind::Bool => u64::from(v != 0),
            _ => v,
        }
    }
}

macro_rules! kind {
    (int) => {
        Kind::Int
    };
    (bool) => {
        Kind::Bool
    };
    ($e:ident) => {
        Kind::Enum(Enum::$e)
    };
}

macro_rules! enums {
    ($($e:ident { $($v:literal $n:ident),* })*) => {
        /// The enumerations fields hold, as Ryujinx names them.
        #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
        pub enum Enum {
            $($e,)*
        }

        impl Enum {
            /// Ryujinx's name for the value `v`, if it has one.
            pub fn value_name(self, v: u64) -> Option<&'static str> {
                let names: &[(u64, &str)] = match self {
                    $(Enum::$e => &[$(($v, stringify!($n))),*],)*
                };
                names.iter().find(|&&(k, _)| k == v).map(|&(_, n)| n)
            }
        }
    };
}

macro_rules! forms {
    ($($form:ident { $($field:ident : $kind:tt = $($shift:literal & $mask:literal)|+),* })*) => {
        /// The form of an encoding, by Ryujinx's name for it.
        #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
        pub enum Form {
            $($form,)*
        }

        static FORMS: &[(&str, &[Field])] = &[
            $((stringify!($form), &[$(Field {
                name: stringify!($field),
                kind: kind!($kind),
                parts: &[$(($shift, $mask)),+],
            }),*]),)*
        ];
    };
}

impl Form {
    /// The form's name (Ryujinx's, without its `Inst` prefix).
    pub fn name(self) -> &'static str {
        FORMS[self as usize].0
    }

    /// The form's fields.
    pub fn fields(self) -> &'static [Field] {
        FORMS[self as usize].1
    }

    /// The field `name`, if the form has it.
    pub fn field(self, name: &str) -> Option<&'static Field> {
        self.fields().iter().find(|f| f.name == name)
    }
}
"""

table = open(R + "InstTable.cs").read()
dec = open(R + "InstDecoders.cs").read()

enums = {}
for m in re.finditer(r"\n    enum (\w+)\n    \{\n(.*?)\n    \}", dec, re.S):
    vals = []
    for v in re.finditer(r"(\w+) = (-?\w+),", m.group(2)):
        vals.append((v.group(1), int(v.group(2), 0)))
    enums[m.group(1)] = vals

structs = {}
for m in re.finditer(r"readonly struct Inst(\w+)\n    \{\n(.*?)\n    \}", dec, re.S):
    fields = []
    for p in re.finditer(r"public (\w+) (\w+) => (.*?);\n", m.group(2) + "\n"):
        ty, name, expr = p.groups()
        if "RegisterConsts" in expr:
            continue
        parts = []
        e = expr
        cast = None
        mc = re.match(r"^\((\w+)\)\((.*)\)$", e)
        if mc and mc.group(1) in enums:
            cast, e = mc.group(1), mc.group(2)
        if ty == "bool":
            mb = re.match(r"^\(_opcode & (0x[0-9A-Fa-f]+)\) != 0$", e)
            assert mb, (name, expr)
            parts.append((0, int(mb.group(1), 16)))
            kind = "Bool"
        else:
            for term in e.split(" | "):
                t = term.strip()
                t = re.sub(r"^\(int\)", "", t)
                mm = re.match(r"^\(\(_opcode >> (\d+)\) & (0x[0-9A-Fa-f]+)\)$", t) or re.match(r"^\(_opcode >> (\d+)\) & (0x[0-9A-Fa-f]+)$", t)
                if mm:
                    parts.append((int(mm.group(1)), int(mm.group(2), 16)))
                    continue
                mm = re.match(r"^\(_opcode & (0x[0-9A-Fa-f]+)\)$", t) or re.match(r"^_opcode & (0x[0-9A-Fa-f]+)$", t)
                if mm:
                    parts.append((0, int(mm.group(1), 16)))
                    continue
                mm = re.match(r"^\(_opcode >> (\d+)\)$", t)
                if mm:
                    s = int(mm.group(1))
                    parts.append((s, min((1 << (64 - s)) - 1, 0xFFFFFFFF)))
                    continue
                raise SystemExit(f"unknown term {name}: {expr}")
            if ty in enums:
                cast = ty
            kind = f"Enum(Enum::{cast})" if cast else "Int"
        fields.append((name, kind, parts))
    structs[m.group(1)] = fields

rows = re.findall(r'Add\("([01x]{64})",\s*InstName\.(\w+),\s*InstEmit\.(\w+)(?:,\s*([^)]*))?\)', table)
ents = []
ops = []
cctlt = 0
for pat, name, emit, props in rows:
    form = emit
    if emit.endswith("F16") and emit[:-3] in structs:
        form = emit[:-3]
    if emit == "Cctlt":
        form = "Cctlt" if cctlt == 0 else "CctltR"
        cctlt += 1
    assert form in structs, form
    suf = emit[len(name):] if emit.startswith(name) else emit
    op = name
    if name == "Shf":
        op = "ShfL" if suf[0] == "L" else "ShfR"
        suf = suf[1:]
    if name.endswith("32i"):
        op, src = name[:-3], "Imm32"
    elif emit.endswith("F16"):
        op, src = emit, "None"
    else:
        src = {"": "None", "R": "Reg", "I": "Imm", "C": "CBuf", "Rc": "RegCBuf", "32i": "Imm32"}[suf]
    if op not in ops:
        ops.append(op)
    ents.append((pat, op, src, form))
envy = [("1110111011001", "Ldg", "Ldg"), ("11101011010", "Sured", "Sured"), ("11101010011", "Suatom", "Suatom")]
for top, op, form in envy:
    ents.append((top + "x" * (64 - len(top)), op, "None", form))

forms = sorted({e[3] for e in ents})
fidx = {f: i for i, f in enumerate(forms)}


def up(o):
    return re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", o).upper()


head = FORM_HEAD
mid = FORM_MID
o = [head.rstrip("\n"), "pub enum Op {"]
o += [f"    {x}," for x in sorted(ops)]
o += ["}", mid.rstrip("\n"), "", "const fn names(op: Op) -> &'static str {", "    match op {"]
o += [f'        Op::{x} => "{up(x)}",' for x in sorted(ops)]
o += ["    }", "}", ""]
o.append("/// Every encoding, in Ryujinx's order, then the three from envytools.")
o.append(f"pub static ENCODINGS: [Encoding; {len(ents)}] = [")
for pat, op, src, form in ents:
    o.append(f'    enc(b"{pat}", Op::{op}, Src::{src}, Form::{form}),')
o.append("];")
open(OUT + "form.rs", "w").write("\n".join(o) + "\n")

used = set()
for s_ in forms:
    for (_, k, _) in structs[s_]:
        if k.startswith("Enum"):
            used.add(k[len("Enum(Enum::"):-1])
f = [FIELD_HEAD.rstrip("\n"), "", "enums! {"]
for e in sorted(used):
    vals = ", ".join(f"{v} {n}" for n, v in enums[e])
    f.append(f"    {e} {{ {vals} }}")
f += ["}", "", "forms! {"]
for s_ in forms:
    fl = []
    for name, kind, parts in structs[s_]:
        k = "int" if kind == "Int" else "bool" if kind == "Bool" else kind[len("Enum(Enum::"):-1]
        ps = " | ".join(f"{a} & {b:#x}" for a, b in parts)
        fl.append(f"{name}: {k} = {ps}")
    f.append(f"    {s_} {{ {', '.join(fl)} }}")
f.append("}")
open(OUT + "field.rs", "w").write("\n".join(f) + "\n")
