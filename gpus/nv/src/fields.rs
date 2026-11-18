//! The values of an instruction that the translator reads beside its
//! operands and modifiers, by name. A front end sets the ones of each form
//! it produces (see the field documentation for which forms read which);
//! the rest keep their defaults and nothing reads them.

/// Named values of one instruction.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Fields {
    /// The destination register (255 is `RZ`): `IADD3`, texture and
    /// surface instructions.
    pub rd: u8,
    /// The A source register: `IADD3`, `RET`, `BRX`, the first source of
    /// a texture or surface instruction, the address register of `ISBERD`
    /// and `ISBEWR`.
    pub ra: u8,
    /// The B source register: the second source of a texture or surface
    /// instruction, the data of `ISBEWR`.
    pub rb: u8,
    /// The C source register: `IADD3`, the second destination of a
    /// texture instruction, the handle of a surface instruction.
    pub rc: u8,
    /// The uniform register of the A source: `BRX`, `BRXU`.
    pub ura: u8,
    /// The A source of `RET` or `BRX` is a uniform register.
    pub uniform: bool,
    /// The first and second predicate destinations (7 is `PT`): the carry
    /// outs of `IADD3`, `IMAD` and `LEA`.
    pub pd: u8,
    pub pd2: u8,
    /// The signed byte offset of `RET` and `BRX`: from the instruction
    /// after them, or for `RET.ABS` the address itself.
    pub offset: i64,
    /// The immediate B source of `IADD3`.
    pub imm: Option<u32>,
    /// The component mask of a texture instruction, the channel and size
    /// bits of a surface instruction.
    pub mask: u8,
    /// The control byte of `FSWZADD`.
    pub swizzle: u8,
    /// `LDC` and `ULDC`.
    pub cbuf: Cbuf,
    /// Texture and surface instructions.
    pub tex: Tex,
    /// `ISBERD` and `ISBEWR`.
    pub isbe: Isbe,
}

/// Where `LDC` or `ULDC` reads.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Cbuf {
    /// The bank is a uniform register (which the translator rejects).
    pub bank_ureg: bool,
    pub bank: u32,
    /// The signed byte offset.
    pub offset: i64,
    /// The index register added to the offset.
    pub index: CbufIndex,
    /// `LDC`'s indexing mode: 0 plain, 1 `.IL`, 2 `.IS`, 3 `.ISL`.
    pub mode: u8,
}

/// The index register of a constant bank load.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum CbufIndex {
    #[default]
    None,
    Reg(u8),
    UReg(u8),
}

/// The handle of a texture or surface instruction.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Handle {
    #[default]
    None,
    /// Bound: the handle word at a constant bank byte offset.
    Bound { bank: u32, offset: u32 },
    /// Bindless, in a register source.
    Reg,
    /// Bindless, in uniform register `n` (texture index) and `n + 1`
    /// (sampler index); `plain` when no offset or bank is added.
    UReg { n: u8, plain: bool },
    /// A surface handle split over a bank and two offsets (rejected).
    Split,
}

/// The values of a texture or surface instruction.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Tex {
    pub handle: Handle,
    /// The dimension index of the instruction's dimension table.
    pub dim: u8,
    /// The LOD mode of `TEX` and `TLD`: 0 implicit, 1 `.LZ`, 2 `.LB`, 3
    /// `.LL`, 4 `.LC`, 5 `.LB.LC`, 6 `.LC.FDV`.
    pub lod: u8,
    /// `TLD4`'s component: 0 to 3 for R, G, B, A.
    pub component: u8,
    /// `TLD4`'s offset mode: 1 `.AOFFI`, 2 `.PTP`.
    pub offsets: u8,
    /// `.AOFFI` of `TEX`, `TLD` and `TXD`.
    pub aoffi: bool,
    /// `.DC` of `TEX` and `TLD4`.
    pub dc: bool,
    /// `.MS` of `TLD`.
    pub ms: bool,
    /// `.CL` of `TLD`.
    pub cl: bool,
    /// `.LC` of `TXD`.
    pub lc: bool,
    /// `.SCR` of `TEX`, `TLD` and `TLD4`.
    pub scr: bool,
    /// Half results: 0 none, 1 `.F16.RN`, 2 `.F16.RZ`.
    pub f16: u8,
    /// `TXQ`'s query; for `SUQUERY` 0 is `COMP_STATUS`.
    pub query: u8,
    /// The sparse fault predicate destination (7 is `PT`) of the forms
    /// that have one.
    pub fault: u8,
    /// The atomic operation of `SUATOM` and `SURED`.
    pub atom: u8,
    /// The data type of a surface instruction.
    pub ty: u8,
    /// `.BA` of a surface instruction.
    pub ba: bool,
}

/// The values of `ISBERD` and `ISBEWR`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Isbe {
    /// `.O`: the output stage buffer.
    pub out: bool,
    /// `.SKEW`.
    pub skew: bool,
    /// 0 plain, 1 `.PATCH`, 2 `.PRIM`, 3 `.ATTR`.
    pub class: u8,
    /// 0 for 8 bits, 1 `.U16`, 2 `.32`.
    pub size: u8,
    /// The byte offset added to the address register.
    pub offset: u32,
}
