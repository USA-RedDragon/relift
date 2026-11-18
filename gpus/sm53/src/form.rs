//! The Maxwell instruction forms: every opcode pattern, the instruction it
//! encodes and the form of its last source operand.
//!
//! The patterns are Ryujinx's `InstTable` (MIT, Copyright (c) Ryujinx Team
//! and Contributors), most significant bit first, with three encodings only
//! envytools' `gm107.c` (MIT) lists: `LDG` 0xeec8, `SURED` 0xeb40 and
//! `SUATOM` 0xea60. Where several patterns match a word, the one with the
//! most fixed bits decides.

use std::sync::OnceLock;

/// An instruction, whatever the form of its operands.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Op {
    Al2p,
    Ald,
    Ast,
    Atom,
    AtomCas,
    Atoms,
    AtomsCas,
    B2r,
    Bar,
    Bfe,
    Bfi,
    Bpt,
    Bra,
    Brk,
    Brx,
    Cal,
    Cctl,
    Cctll,
    Cctlt,
    Cont,
    Cs2r,
    Cset,
    Csetp,
    Dadd,
    Depbar,
    Dfma,
    Dmnmx,
    Dmul,
    Dset,
    Dsetp,
    Exit,
    F2f,
    F2i,
    Fadd,
    Fchk,
    Fcmp,
    Ffma,
    Flo,
    Fmnmx,
    Fmul,
    Fset,
    Fsetp,
    Fswzadd,
    Getcrsptr,
    Getlmembase,
    Hadd2,
    Hfma2,
    Hmul2,
    Hset2,
    Hsetp2,
    I2f,
    I2i,
    Iadd,
    Iadd3,
    Icmp,
    Ide,
    Idp,
    Imad,
    Imadsp,
    Imnmx,
    Imul,
    Ipa,
    Isberd,
    Iscadd,
    Iset,
    Isetp,
    Jcal,
    Jmp,
    Jmx,
    Kil,
    Ld,
    Ldc,
    Ldg,
    Ldl,
    Lds,
    Lea,
    LeaHi,
    Lepc,
    Longjmp,
    Lop,
    Lop3,
    Membar,
    Mov,
    Mufu,
    Nop,
    Out,
    P2r,
    Pbk,
    Pcnt,
    Pexit,
    Pixld,
    Plongjmp,
    Popc,
    Pret,
    Prmt,
    Pset,
    Psetp,
    R2b,
    R2p,
    Ram,
    Red,
    Ret,
    Rro,
    Rtt,
    S2r,
    Sam,
    Sel,
    Setcrsptr,
    Setlmembase,
    ShfL,
    ShfR,
    Shfl,
    Shl,
    Shr,
    Ssy,
    St,
    Stg,
    Stl,
    Stp,
    Sts,
    Suatom,
    SuatomB,
    SuatomB2,
    SuatomCas,
    SuatomCasB,
    Suld,
    SuldB,
    SuldD,
    SuldDB,
    Sured,
    SuredB,
    Sust,
    SustB,
    SustD,
    SustDB,
    Sync,
    Tex,
    TexB,
    Texs,
    TexsF16,
    Tld,
    Tld4,
    Tld4B,
    Tld4s,
    Tld4sF16,
    TldB,
    Tlds,
    TldsF16,
    Tmml,
    TmmlB,
    Txa,
    Txd,
    TxdB,
    Txq,
    TxqB,
    Vabsdiff,
    Vabsdiff4,
    Vadd,
    Vmad,
    Vmnmx,
    Vote,
    Votevtg,
    Vset,
    Vsetp,
    Vshl,
    Vshr,
    Xmad,
}

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

const fn enc(p: &[u8; 64], op: Op, src: Src) -> Encoding {
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

const fn names(op: Op) -> &'static str {
    match op {
        Op::Al2p => "AL2P",
        Op::Ald => "ALD",
        Op::Ast => "AST",
        Op::Atom => "ATOM",
        Op::AtomCas => "ATOM_CAS",
        Op::Atoms => "ATOMS",
        Op::AtomsCas => "ATOMS_CAS",
        Op::B2r => "B2R",
        Op::Bar => "BAR",
        Op::Bfe => "BFE",
        Op::Bfi => "BFI",
        Op::Bpt => "BPT",
        Op::Bra => "BRA",
        Op::Brk => "BRK",
        Op::Brx => "BRX",
        Op::Cal => "CAL",
        Op::Cctl => "CCTL",
        Op::Cctll => "CCTLL",
        Op::Cctlt => "CCTLT",
        Op::Cont => "CONT",
        Op::Cs2r => "CS2R",
        Op::Cset => "CSET",
        Op::Csetp => "CSETP",
        Op::Dadd => "DADD",
        Op::Depbar => "DEPBAR",
        Op::Dfma => "DFMA",
        Op::Dmnmx => "DMNMX",
        Op::Dmul => "DMUL",
        Op::Dset => "DSET",
        Op::Dsetp => "DSETP",
        Op::Exit => "EXIT",
        Op::F2f => "F2F",
        Op::F2i => "F2I",
        Op::Fadd => "FADD",
        Op::Fchk => "FCHK",
        Op::Fcmp => "FCMP",
        Op::Ffma => "FFMA",
        Op::Flo => "FLO",
        Op::Fmnmx => "FMNMX",
        Op::Fmul => "FMUL",
        Op::Fset => "FSET",
        Op::Fsetp => "FSETP",
        Op::Fswzadd => "FSWZADD",
        Op::Getcrsptr => "GETCRSPTR",
        Op::Getlmembase => "GETLMEMBASE",
        Op::Hadd2 => "HADD2",
        Op::Hfma2 => "HFMA2",
        Op::Hmul2 => "HMUL2",
        Op::Hset2 => "HSET2",
        Op::Hsetp2 => "HSETP2",
        Op::I2f => "I2F",
        Op::I2i => "I2I",
        Op::Iadd => "IADD",
        Op::Iadd3 => "IADD3",
        Op::Icmp => "ICMP",
        Op::Ide => "IDE",
        Op::Idp => "IDP",
        Op::Imad => "IMAD",
        Op::Imadsp => "IMADSP",
        Op::Imnmx => "IMNMX",
        Op::Imul => "IMUL",
        Op::Ipa => "IPA",
        Op::Isberd => "ISBERD",
        Op::Iscadd => "ISCADD",
        Op::Iset => "ISET",
        Op::Isetp => "ISETP",
        Op::Jcal => "JCAL",
        Op::Jmp => "JMP",
        Op::Jmx => "JMX",
        Op::Kil => "KIL",
        Op::Ld => "LD",
        Op::Ldc => "LDC",
        Op::Ldg => "LDG",
        Op::Ldl => "LDL",
        Op::Lds => "LDS",
        Op::Lea => "LEA",
        Op::LeaHi => "LEA_HI",
        Op::Lepc => "LEPC",
        Op::Longjmp => "LONGJMP",
        Op::Lop => "LOP",
        Op::Lop3 => "LOP3",
        Op::Membar => "MEMBAR",
        Op::Mov => "MOV",
        Op::Mufu => "MUFU",
        Op::Nop => "NOP",
        Op::Out => "OUT",
        Op::P2r => "P2R",
        Op::Pbk => "PBK",
        Op::Pcnt => "PCNT",
        Op::Pexit => "PEXIT",
        Op::Pixld => "PIXLD",
        Op::Plongjmp => "PLONGJMP",
        Op::Popc => "POPC",
        Op::Pret => "PRET",
        Op::Prmt => "PRMT",
        Op::Pset => "PSET",
        Op::Psetp => "PSETP",
        Op::R2b => "R2B",
        Op::R2p => "R2P",
        Op::Ram => "RAM",
        Op::Red => "RED",
        Op::Ret => "RET",
        Op::Rro => "RRO",
        Op::Rtt => "RTT",
        Op::S2r => "S2R",
        Op::Sam => "SAM",
        Op::Sel => "SEL",
        Op::Setcrsptr => "SETCRSPTR",
        Op::Setlmembase => "SETLMEMBASE",
        Op::ShfL => "SHF_L",
        Op::ShfR => "SHF_R",
        Op::Shfl => "SHFL",
        Op::Shl => "SHL",
        Op::Shr => "SHR",
        Op::Ssy => "SSY",
        Op::St => "ST",
        Op::Stg => "STG",
        Op::Stl => "STL",
        Op::Stp => "STP",
        Op::Sts => "STS",
        Op::Suatom => "SUATOM",
        Op::SuatomB => "SUATOM_B",
        Op::SuatomB2 => "SUATOM_B2",
        Op::SuatomCas => "SUATOM_CAS",
        Op::SuatomCasB => "SUATOM_CAS_B",
        Op::Suld => "SULD",
        Op::SuldB => "SULD_B",
        Op::SuldD => "SULD_D",
        Op::SuldDB => "SULD_DB",
        Op::Sured => "SURED",
        Op::SuredB => "SURED_B",
        Op::Sust => "SUST",
        Op::SustB => "SUST_B",
        Op::SustD => "SUST_D",
        Op::SustDB => "SUST_DB",
        Op::Sync => "SYNC",
        Op::Tex => "TEX",
        Op::TexB => "TEX_B",
        Op::Texs => "TEXS",
        Op::TexsF16 => "TEXS_F16",
        Op::Tld => "TLD",
        Op::Tld4 => "TLD4",
        Op::Tld4B => "TLD4_B",
        Op::Tld4s => "TLD4S",
        Op::Tld4sF16 => "TLD4S_F16",
        Op::TldB => "TLD_B",
        Op::Tlds => "TLDS",
        Op::TldsF16 => "TLDS_F16",
        Op::Tmml => "TMML",
        Op::TmmlB => "TMML_B",
        Op::Txa => "TXA",
        Op::Txd => "TXD",
        Op::TxdB => "TXD_B",
        Op::Txq => "TXQ",
        Op::TxqB => "TXQ_B",
        Op::Vabsdiff => "VABSDIFF",
        Op::Vabsdiff4 => "VABSDIFF4",
        Op::Vadd => "VADD",
        Op::Vmad => "VMAD",
        Op::Vmnmx => "VMNMX",
        Op::Vote => "VOTE",
        Op::Votevtg => "VOTEVTG",
        Op::Vset => "VSET",
        Op::Vsetp => "VSETP",
        Op::Vshl => "VSHL",
        Op::Vshr => "VSHR",
        Op::Xmad => "XMAD",
    }
}

/// Every encoding, in Ryujinx's order, then the three from envytools.
pub static ENCODINGS: [Encoding; 297] = [
    enc(
        b"1110111110100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Al2p,
        Src::None,
    ),
    enc(
        b"1110111111011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Ald,
        Src::None,
    ),
    enc(
        b"1110111111110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Ast,
        Src::None,
    ),
    enc(
        b"11101101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Atom,
        Src::None,
    ),
    enc(
        b"111011101111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::AtomCas,
        Src::None,
    ),
    enc(
        b"11101100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Atoms,
        Src::None,
    ),
    enc(
        b"111011100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::AtomsCas,
        Src::None,
    ),
    enc(
        b"1111000010111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::B2r,
        Src::None,
    ),
    enc(
        b"1111000010101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Bar,
        Src::None,
    ),
    enc(
        b"0101110000000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Bfe,
        Src::Reg,
    ),
    enc(
        b"0011100x00000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Bfe,
        Src::Imm,
    ),
    enc(
        b"0100110000000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Bfe,
        Src::CBuf,
    ),
    enc(
        b"0101101111110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Bfi,
        Src::Reg,
    ),
    enc(
        b"0011011x11110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Bfi,
        Src::Imm,
    ),
    enc(
        b"0100101111110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Bfi,
        Src::CBuf,
    ),
    enc(
        b"0101001111110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Bfi,
        Src::RegCBuf,
    ),
    enc(
        b"111000111010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Bpt,
        Src::None,
    ),
    enc(
        b"111000100100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Bra,
        Src::None,
    ),
    enc(
        b"111000110100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Brk,
        Src::None,
    ),
    enc(
        b"111000100101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Brx,
        Src::None,
    ),
    enc(
        b"111000100110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Cal,
        Src::None,
    ),
    enc(
        b"11101111011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Cctl,
        Src::None,
    ),
    enc(
        b"1110111110000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Cctll,
        Src::None,
    ),
    enc(
        b"1110101111110xx0000000000000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Cctlt,
        Src::None,
    ),
    enc(
        b"1110101111101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Cctlt,
        Src::None,
    ),
    enc(
        b"111000110101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Cont,
        Src::None,
    ),
    enc(
        b"0101000010011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Cset,
        Src::None,
    ),
    enc(
        b"0101000010100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Csetp,
        Src::None,
    ),
    enc(
        b"0101000011001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Cs2r,
        Src::None,
    ),
    enc(
        b"0101110001110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Dadd,
        Src::Reg,
    ),
    enc(
        b"0011100x01110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Dadd,
        Src::Imm,
    ),
    enc(
        b"0100110001110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Dadd,
        Src::CBuf,
    ),
    enc(
        b"1111000011110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Depbar,
        Src::None,
    ),
    enc(
        b"010110110111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Dfma,
        Src::Reg,
    ),
    enc(
        b"0011011x0111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Dfma,
        Src::Imm,
    ),
    enc(
        b"010010110111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Dfma,
        Src::CBuf,
    ),
    enc(
        b"010100110111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Dfma,
        Src::RegCBuf,
    ),
    enc(
        b"0101110001010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Dmnmx,
        Src::Reg,
    ),
    enc(
        b"0011100x01010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Dmnmx,
        Src::Imm,
    ),
    enc(
        b"0100110001010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Dmnmx,
        Src::CBuf,
    ),
    enc(
        b"0101110010000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Dmul,
        Src::Reg,
    ),
    enc(
        b"0011100x10000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Dmul,
        Src::Imm,
    ),
    enc(
        b"0100110010000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Dmul,
        Src::CBuf,
    ),
    enc(
        b"010110010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Dset,
        Src::Reg,
    ),
    enc(
        b"0011001x0xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Dset,
        Src::Imm,
    ),
    enc(
        b"010010010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Dset,
        Src::CBuf,
    ),
    enc(
        b"010110111000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Dsetp,
        Src::Reg,
    ),
    enc(
        b"0011011x1000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Dsetp,
        Src::Imm,
    ),
    enc(
        b"010010111000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Dsetp,
        Src::CBuf,
    ),
    enc(
        b"111000110000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Exit,
        Src::None,
    ),
    enc(
        b"0101110010101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::F2f,
        Src::Reg,
    ),
    enc(
        b"0011100x10101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::F2f,
        Src::Imm,
    ),
    enc(
        b"0100110010101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::F2f,
        Src::CBuf,
    ),
    enc(
        b"0101110010110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::F2i,
        Src::Reg,
    ),
    enc(
        b"0011100x10110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::F2i,
        Src::Imm,
    ),
    enc(
        b"0100110010110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::F2i,
        Src::CBuf,
    ),
    enc(
        b"0101110001011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Fadd,
        Src::Reg,
    ),
    enc(
        b"0011100x01011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Fadd,
        Src::Imm,
    ),
    enc(
        b"0100110001011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Fadd,
        Src::CBuf,
    ),
    enc(
        b"000010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Fadd,
        Src::Imm32,
    ),
    enc(
        b"0101110010001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Fchk,
        Src::Reg,
    ),
    enc(
        b"0011100x10001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Fchk,
        Src::Imm,
    ),
    enc(
        b"0100110010001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Fchk,
        Src::CBuf,
    ),
    enc(
        b"010110111010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Fcmp,
        Src::Reg,
    ),
    enc(
        b"0011011x1010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Fcmp,
        Src::Imm,
    ),
    enc(
        b"010010111010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Fcmp,
        Src::CBuf,
    ),
    enc(
        b"010100111010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Fcmp,
        Src::RegCBuf,
    ),
    enc(
        b"010110011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Ffma,
        Src::Reg,
    ),
    enc(
        b"0011001x1xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Ffma,
        Src::Imm,
    ),
    enc(
        b"010010011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Ffma,
        Src::CBuf,
    ),
    enc(
        b"010100011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Ffma,
        Src::RegCBuf,
    ),
    enc(
        b"000011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Ffma,
        Src::Imm32,
    ),
    enc(
        b"0101110000110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Flo,
        Src::Reg,
    ),
    enc(
        b"0011100x00110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Flo,
        Src::Imm,
    ),
    enc(
        b"0100110000110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Flo,
        Src::CBuf,
    ),
    enc(
        b"0101110001100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Fmnmx,
        Src::Reg,
    ),
    enc(
        b"0011100x01100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Fmnmx,
        Src::Imm,
    ),
    enc(
        b"0100110001100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Fmnmx,
        Src::CBuf,
    ),
    enc(
        b"0101110001101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Fmul,
        Src::Reg,
    ),
    enc(
        b"0011100x01101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Fmul,
        Src::Imm,
    ),
    enc(
        b"0100110001101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Fmul,
        Src::CBuf,
    ),
    enc(
        b"00011110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Fmul,
        Src::Imm32,
    ),
    enc(
        b"01011000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Fset,
        Src::Reg,
    ),
    enc(
        b"0011000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Fset,
        Src::Imm,
    ),
    enc(
        b"01001000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Fset,
        Src::CBuf,
    ),
    enc(
        b"010110111011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Fsetp,
        Src::Reg,
    ),
    enc(
        b"0011011x1011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Fsetp,
        Src::Imm,
    ),
    enc(
        b"010010111011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Fsetp,
        Src::CBuf,
    ),
    enc(
        b"0101000011111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Fswzadd,
        Src::None,
    ),
    enc(
        b"111000101100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Getcrsptr,
        Src::None,
    ),
    enc(
        b"111000101101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Getlmembase,
        Src::None,
    ),
    enc(
        b"0101110100010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Hadd2,
        Src::Reg,
    ),
    enc(
        b"0111101x0xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Hadd2,
        Src::Imm,
    ),
    enc(
        b"0111101x1xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Hadd2,
        Src::CBuf,
    ),
    enc(
        b"0010110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Hadd2,
        Src::Imm32,
    ),
    enc(
        b"0101110100000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Hfma2,
        Src::Reg,
    ),
    enc(
        b"01110xxx0xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Hfma2,
        Src::Imm,
    ),
    enc(
        b"01110xxx1xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Hfma2,
        Src::CBuf,
    ),
    enc(
        b"01100xxx1xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Hfma2,
        Src::RegCBuf,
    ),
    enc(
        b"0010100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Hfma2,
        Src::Imm32,
    ),
    enc(
        b"0101110100001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Hmul2,
        Src::Reg,
    ),
    enc(
        b"0111100x0xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Hmul2,
        Src::Imm,
    ),
    enc(
        b"0111100x1xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Hmul2,
        Src::CBuf,
    ),
    enc(
        b"0010101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Hmul2,
        Src::Imm32,
    ),
    enc(
        b"0101110100011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Hset2,
        Src::Reg,
    ),
    enc(
        b"0111110x0xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Hset2,
        Src::Imm,
    ),
    enc(
        b"0111110x1xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Hset2,
        Src::CBuf,
    ),
    enc(
        b"0101110100100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Hsetp2,
        Src::Reg,
    ),
    enc(
        b"0111111x0xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Hsetp2,
        Src::Imm,
    ),
    enc(
        b"0111111x1xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Hsetp2,
        Src::CBuf,
    ),
    enc(
        b"0101110010111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::I2f,
        Src::Reg,
    ),
    enc(
        b"0011100x10111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::I2f,
        Src::Imm,
    ),
    enc(
        b"0100110010111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::I2f,
        Src::CBuf,
    ),
    enc(
        b"0101110011100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::I2i,
        Src::Reg,
    ),
    enc(
        b"0011100x11100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::I2i,
        Src::Imm,
    ),
    enc(
        b"0100110011100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::I2i,
        Src::CBuf,
    ),
    enc(
        b"0101110000010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Iadd,
        Src::Reg,
    ),
    enc(
        b"0011100x00010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Iadd,
        Src::Imm,
    ),
    enc(
        b"0100110000010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Iadd,
        Src::CBuf,
    ),
    enc(
        b"0001110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Iadd,
        Src::Imm32,
    ),
    enc(
        b"010111001100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Iadd3,
        Src::Reg,
    ),
    enc(
        b"0011100x1100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Iadd3,
        Src::Imm,
    ),
    enc(
        b"010011001100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Iadd3,
        Src::CBuf,
    ),
    enc(
        b"010110110100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Icmp,
        Src::Reg,
    ),
    enc(
        b"0011011x0100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Icmp,
        Src::Imm,
    ),
    enc(
        b"010010110100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Icmp,
        Src::CBuf,
    ),
    enc(
        b"010100110100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Icmp,
        Src::RegCBuf,
    ),
    enc(
        b"111000111001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Ide,
        Src::None,
    ),
    enc(
        b"0101001111111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Idp,
        Src::Reg,
    ),
    enc(
        b"0101001111011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Idp,
        Src::CBuf,
    ),
    enc(
        b"010110100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Imad,
        Src::Reg,
    ),
    enc(
        b"0011010x0xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Imad,
        Src::Imm,
    ),
    enc(
        b"010010100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Imad,
        Src::CBuf,
    ),
    enc(
        b"010100100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Imad,
        Src::RegCBuf,
    ),
    enc(
        b"000100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Imad,
        Src::Imm32,
    ),
    enc(
        b"010110101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Imadsp,
        Src::Reg,
    ),
    enc(
        b"0011010x1xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Imadsp,
        Src::Imm,
    ),
    enc(
        b"010010101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Imadsp,
        Src::CBuf,
    ),
    enc(
        b"010100101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Imadsp,
        Src::RegCBuf,
    ),
    enc(
        b"0101110000100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Imnmx,
        Src::Reg,
    ),
    enc(
        b"0011100x00100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Imnmx,
        Src::Imm,
    ),
    enc(
        b"0100110000100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Imnmx,
        Src::CBuf,
    ),
    enc(
        b"0101110000111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Imul,
        Src::Reg,
    ),
    enc(
        b"0011100x00111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Imul,
        Src::Imm,
    ),
    enc(
        b"0100110000111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Imul,
        Src::CBuf,
    ),
    enc(
        b"00011111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Imul,
        Src::Imm32,
    ),
    enc(
        b"11100000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Ipa,
        Src::None,
    ),
    enc(
        b"1110111111010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Isberd,
        Src::None,
    ),
    enc(
        b"0101110000011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Iscadd,
        Src::Reg,
    ),
    enc(
        b"0011100x00011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Iscadd,
        Src::Imm,
    ),
    enc(
        b"0100110000011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Iscadd,
        Src::CBuf,
    ),
    enc(
        b"000101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Iscadd,
        Src::Imm32,
    ),
    enc(
        b"010110110101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Iset,
        Src::Reg,
    ),
    enc(
        b"0011011x0101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Iset,
        Src::Imm,
    ),
    enc(
        b"010010110101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Iset,
        Src::CBuf,
    ),
    enc(
        b"010110110110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Isetp,
        Src::Reg,
    ),
    enc(
        b"0011011x0110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Isetp,
        Src::Imm,
    ),
    enc(
        b"010010110110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Isetp,
        Src::CBuf,
    ),
    enc(
        b"111000100010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Jcal,
        Src::None,
    ),
    enc(
        b"111000100001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Jmp,
        Src::None,
    ),
    enc(
        b"111000100000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Jmx,
        Src::None,
    ),
    enc(
        b"111000110011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Kil,
        Src::None,
    ),
    enc(
        b"100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Ld,
        Src::None,
    ),
    enc(
        b"1110111110010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Ldc,
        Src::None,
    ),
    enc(
        b"1110111011010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Ldg,
        Src::None,
    ),
    enc(
        b"1110111101000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Ldl,
        Src::None,
    ),
    enc(
        b"1110111101001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Lds,
        Src::None,
    ),
    enc(
        b"0101101111010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Lea,
        Src::Reg,
    ),
    enc(
        b"0011011x11010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Lea,
        Src::Imm,
    ),
    enc(
        b"0100101111010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Lea,
        Src::CBuf,
    ),
    enc(
        b"0101101111011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::LeaHi,
        Src::Reg,
    ),
    enc(
        b"000110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::LeaHi,
        Src::CBuf,
    ),
    enc(
        b"0101000011010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Lepc,
        Src::None,
    ),
    enc(
        b"111000110001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Longjmp,
        Src::None,
    ),
    enc(
        b"0101110001000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Lop,
        Src::Reg,
    ),
    enc(
        b"0011100x01000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Lop,
        Src::Imm,
    ),
    enc(
        b"0100110001000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Lop,
        Src::CBuf,
    ),
    enc(
        b"0101101111100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Lop3,
        Src::Reg,
    ),
    enc(
        b"001111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Lop3,
        Src::Imm,
    ),
    enc(
        b"0000001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Lop3,
        Src::CBuf,
    ),
    enc(
        b"000001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Lop,
        Src::Imm32,
    ),
    enc(
        b"1110111110011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Membar,
        Src::None,
    ),
    enc(
        b"0101110010011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Mov,
        Src::Reg,
    ),
    enc(
        b"0011100x10011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Mov,
        Src::Imm,
    ),
    enc(
        b"0100110010011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Mov,
        Src::CBuf,
    ),
    enc(
        b"000000010000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Mov,
        Src::Imm32,
    ),
    enc(
        b"0101000010000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Mufu,
        Src::None,
    ),
    enc(
        b"0101000010110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Nop,
        Src::None,
    ),
    enc(
        b"1111101111100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Out,
        Src::Reg,
    ),
    enc(
        b"1111011x11100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Out,
        Src::Imm,
    ),
    enc(
        b"1110101111100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Out,
        Src::CBuf,
    ),
    enc(
        b"0101110011101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::P2r,
        Src::Reg,
    ),
    enc(
        b"0011100x11101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::P2r,
        Src::Imm,
    ),
    enc(
        b"0100110011101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::P2r,
        Src::CBuf,
    ),
    enc(
        b"111000101010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Pbk,
        Src::None,
    ),
    enc(
        b"111000101011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Pcnt,
        Src::None,
    ),
    enc(
        b"111000100011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Pexit,
        Src::None,
    ),
    enc(
        b"1110111111101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Pixld,
        Src::None,
    ),
    enc(
        b"111000101000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Plongjmp,
        Src::None,
    ),
    enc(
        b"0101110000001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Popc,
        Src::Reg,
    ),
    enc(
        b"0011100x00001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Popc,
        Src::Imm,
    ),
    enc(
        b"0100110000001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Popc,
        Src::CBuf,
    ),
    enc(
        b"111000100111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Pret,
        Src::None,
    ),
    enc(
        b"010110111100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Prmt,
        Src::Reg,
    ),
    enc(
        b"0011011x1100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Prmt,
        Src::Imm,
    ),
    enc(
        b"010010111100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Prmt,
        Src::CBuf,
    ),
    enc(
        b"010100111100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Prmt,
        Src::RegCBuf,
    ),
    enc(
        b"0101000010001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Pset,
        Src::None,
    ),
    enc(
        b"0101000010010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Psetp,
        Src::None,
    ),
    enc(
        b"1111000011000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::R2b,
        Src::None,
    ),
    enc(
        b"0101110011110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::R2p,
        Src::Reg,
    ),
    enc(
        b"0011100x11110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::R2p,
        Src::Imm,
    ),
    enc(
        b"0100110011110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::R2p,
        Src::CBuf,
    ),
    enc(
        b"111000111000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Ram,
        Src::None,
    ),
    enc(
        b"1110101111111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Red,
        Src::None,
    ),
    enc(
        b"111000110010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Ret,
        Src::None,
    ),
    enc(
        b"0101110010010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Rro,
        Src::Reg,
    ),
    enc(
        b"0011100x10010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Rro,
        Src::Imm,
    ),
    enc(
        b"0100110010010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Rro,
        Src::CBuf,
    ),
    enc(
        b"111000110110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Rtt,
        Src::None,
    ),
    enc(
        b"1111000011001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::S2r,
        Src::None,
    ),
    enc(
        b"111000110111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Sam,
        Src::None,
    ),
    enc(
        b"0101110010100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Sel,
        Src::Reg,
    ),
    enc(
        b"0011100x10100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Sel,
        Src::Imm,
    ),
    enc(
        b"0100110010100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Sel,
        Src::CBuf,
    ),
    enc(
        b"111000101110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Setcrsptr,
        Src::None,
    ),
    enc(
        b"111000101111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Setlmembase,
        Src::None,
    ),
    enc(
        b"0101101111111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::ShfL,
        Src::Reg,
    ),
    enc(
        b"0101110011111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::ShfR,
        Src::Reg,
    ),
    enc(
        b"0011011x11111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::ShfL,
        Src::Imm,
    ),
    enc(
        b"0011100x11111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::ShfR,
        Src::Imm,
    ),
    enc(
        b"1110111100010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Shfl,
        Src::None,
    ),
    enc(
        b"0101110001001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Shl,
        Src::Reg,
    ),
    enc(
        b"0011100x01001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Shl,
        Src::Imm,
    ),
    enc(
        b"0100110001001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Shl,
        Src::CBuf,
    ),
    enc(
        b"0101110000101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Shr,
        Src::Reg,
    ),
    enc(
        b"0011100x00101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Shr,
        Src::Imm,
    ),
    enc(
        b"0100110000101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Shr,
        Src::CBuf,
    ),
    enc(
        b"111000101001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Ssy,
        Src::None,
    ),
    enc(
        b"101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::St,
        Src::None,
    ),
    enc(
        b"1110111011011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Stg,
        Src::None,
    ),
    enc(
        b"1110111101010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Stl,
        Src::None,
    ),
    enc(
        b"1110111010100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Stp,
        Src::None,
    ),
    enc(
        b"1110111101011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Sts,
        Src::None,
    ),
    enc(
        b"1110101001110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::SuatomB,
        Src::None,
    ),
    enc(
        b"11101010x0xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Suatom,
        Src::None,
    ),
    enc(
        b"1110101110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::SuatomB2,
        Src::None,
    ),
    enc(
        b"1110101011010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::SuatomCasB,
        Src::None,
    ),
    enc(
        b"1110101x1xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::SuatomCas,
        Src::None,
    ),
    enc(
        b"1110101100010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::SuldDB,
        Src::None,
    ),
    enc(
        b"1110101100011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::SuldD,
        Src::None,
    ),
    enc(
        b"1110101100000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::SuldB,
        Src::None,
    ),
    enc(
        b"1110101100001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Suld,
        Src::None,
    ),
    enc(
        b"1110101101010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::SuredB,
        Src::None,
    ),
    enc(
        b"1110101101011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Sured,
        Src::None,
    ),
    enc(
        b"1110101100110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::SustDB,
        Src::None,
    ),
    enc(
        b"1110101100111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::SustD,
        Src::None,
    ),
    enc(
        b"1110101100100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::SustB,
        Src::None,
    ),
    enc(
        b"1110101100101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Sust,
        Src::None,
    ),
    enc(
        b"1111000011111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Sync,
        Src::None,
    ),
    enc(
        b"11000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Tex,
        Src::None,
    ),
    enc(
        b"1101111010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::TexB,
        Src::None,
    ),
    enc(
        b"1101100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Texs,
        Src::None,
    ),
    enc(
        b"1101000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::TexsF16,
        Src::None,
    ),
    enc(
        b"11011100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Tld,
        Src::None,
    ),
    enc(
        b"11011101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::TldB,
        Src::None,
    ),
    enc(
        b"1101101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Tlds,
        Src::None,
    ),
    enc(
        b"1101001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::TldsF16,
        Src::None,
    ),
    enc(
        b"110010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Tld4,
        Src::None,
    ),
    enc(
        b"1101111011xxxxxxxxxxxxx0xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Tld4B,
        Src::None,
    ),
    enc(
        b"1101111100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Tld4s,
        Src::None,
    ),
    enc(
        b"1101111110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Tld4sF16,
        Src::None,
    ),
    enc(
        b"1101111101011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Tmml,
        Src::None,
    ),
    enc(
        b"1101111101100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::TmmlB,
        Src::None,
    ),
    enc(
        b"1101111101000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Txa,
        Src::None,
    ),
    enc(
        b"110111100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Txd,
        Src::None,
    ),
    enc(
        b"1101111001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::TxdB,
        Src::None,
    ),
    enc(
        b"1101111101001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Txq,
        Src::None,
    ),
    enc(
        b"1101111101010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::TxqB,
        Src::None,
    ),
    enc(
        b"01010100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Vabsdiff,
        Src::None,
    ),
    enc(
        b"010100000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Vabsdiff4,
        Src::None,
    ),
    enc(
        b"001000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Vadd,
        Src::None,
    ),
    enc(
        b"01011111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Vmad,
        Src::None,
    ),
    enc(
        b"0011101xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Vmnmx,
        Src::None,
    ),
    enc(
        b"0101000011011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Vote,
        Src::None,
    ),
    enc(
        b"0101000011100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Votevtg,
        Src::None,
    ),
    enc(
        b"0100000xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Vset,
        Src::None,
    ),
    enc(
        b"0101000011110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Vsetp,
        Src::None,
    ),
    enc(
        b"01010111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Vshl,
        Src::None,
    ),
    enc(
        b"01010110xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Vshr,
        Src::None,
    ),
    enc(
        b"0101101100xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Xmad,
        Src::Reg,
    ),
    enc(
        b"0011011x00xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Xmad,
        Src::Imm,
    ),
    enc(
        b"0100111xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Xmad,
        Src::CBuf,
    ),
    enc(
        b"010100010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Xmad,
        Src::RegCBuf,
    ),
    enc(
        b"1110111011001xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Ldg,
        Src::None,
    ),
    enc(
        b"11101011010xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Sured,
        Src::None,
    ),
    enc(
        b"11101010011xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        Op::Suatom,
        Src::None,
    ),
];
