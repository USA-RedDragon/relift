//! The fields of each instruction form: where each operand and modifier
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

enums! {
    ASelect4 { 0 _0000, 1 _1111, 2 _2222, 3 _3333, 4 _3210, 6 _5432, 7 _6543, 8 _3201, 9 _3012, 10 _0213, 11 _3120, 12 _1230, 13 _2310 }
    AddressMode { 1 Il, 2 Is, 3 Isl }
    AlSize { 0 _32, 1 _64, 2 _96, 3 _128 }
    AtomOp { 0 Add, 1 Min, 2 Max, 3 Inc, 4 Dec, 5 And, 6 Or, 7 Xor, 8 Exch, 10 Safeadd }
    AtomSize { 0 U32, 1 S32, 2 U64, 3 F32FtzRn, 4 F16x2FtzRn, 5 S64 }
    AtomsSize { 0 U32, 1 S32, 2 U64, 3 S64 }
    AvgMode { 0 NoNeg, 1 NegB, 2 NegA, 3 PlusOne }
    BSelect4 { 0 _4444, 1 _5555, 2 _6666, 3 _7777, 4 _7654, 6 _5432, 7 _4321, 8 _4567, 9 _6745, 10 _5476 }
    BarMode { 0 Bar, 1 Result, 2 Warp }
    BarOp { 0 Sync, 1 Arv, 2 Red, 3 Scan, 4 SyncAll }
    BarRedOp { 0 Popc, 1 And, 2 Or }
    BoolOp { 0 And, 1 Or, 2 Xor }
    Bpt { 0 DrainIllegal, 1 Cal, 2 Pause, 3 Trap, 4 Int, 5 Drain }
    ByteSel { 0 B0, 1 B1, 2 B2, 3 B3 }
    CacheOp2 { 1 Lu, 2 Ci, 3 Cv }
    CacheOpLd { 0 Ca, 1 Cg, 2 Ci, 3 Cv }
    CacheOpSt { 0 Wb, 1 Cg, 2 Ci, 3 Wt }
    CacheType { 1 U, 2 C, 3 I, 4 Crs }
    Ccc { 0 F, 1 Lt, 2 Eq, 3 Le, 4 Gt, 5 Ne, 6 Ge, 7 Num, 8 Nan, 9 Ltu, 10 Equ, 11 Leu, 12 Gtu, 13 Neu, 14 Geu, 15 T, 16 Off, 17 Lo, 18 Sff, 19 Ls, 20 Hi, 21 Sft, 22 Hs, 23 Oft, 24 CsmTa, 25 CsmTr, 26 CsmMx, 27 FcsmTa, 28 FcsmTr, 29 FcsmMx, 30 Rle, 31 Rgt }
    CctlOp { 1 Pf1, 2 Pf1_5, 3 Pf2, 4 Wb, 5 Iv, 6 Ivall, 7 Rs, 9 Rslb }
    CctltOp { 1 Ivth }
    ChkModeF { 0 Divide }
    Clamp { 0 Ign, 2 Trap }
    DstFmt { 1 F16, 2 F32, 3 F64 }
    FComp { 0 F, 1 Lt, 2 Eq, 3 Le, 4 Gt, 5 Ne, 6 Ge, 7 Num, 8 Nan, 9 Ltu, 10 Equ, 11 Leu, 12 Gtu, 13 Neu, 14 Geu, 15 T }
    Fmz { 1 Ftz, 2 Fmz }
    HalfSelect { 0 B32, 1 H0, 2 H1 }
    HalfSwizzle { 0 F16, 1 F32, 2 H0H0, 3 H1H1 }
    IBase { 1 Patch, 2 Prim, 3 Attr }
    IComp { 0 F, 1 Lt, 2 Eq, 3 Le, 4 Gt, 5 Ne, 6 Ge, 7 T }
    IDstFmt { 1 U16, 2 U32, 3 U64, 5 S16, 6 S32, 7 S64 }
    ISrcDstFmt { 0 U8, 1 U16, 2 U32, 4 S8, 5 S16, 6 S32 }
    ISrcFmt { 0 U8, 1 U16, 2 U32, 3 U64, 4 S8, 5 S16, 6 S32, 7 S64 }
    ImadspASelect { 0 U32, 1 S32, 2 U24, 3 S24, 4 U16h0, 5 S16h0, 6 U16h1, 7 S16h1 }
    ImadspBSelect { 0 U24, 1 S24, 2 U16h0, 3 S16h0 }
    IntegerRound { 1 Pass, 4 Round, 5 Floor, 6 Ceil, 7 Trunc }
    IpaOp { 0 Pass, 1 Multiply, 2 Constant, 3 Sc }
    Ivall { 1 Ivalld, 2 Ivallt, 3 Ivalltd }
    LaneMask4 { 1 Z, 2 W, 3 Zw, 4 X, 5 Xz, 6 Xw, 7 Xzw, 8 Y, 9 Yz, 10 Yw, 11 Yzw, 12 Xy, 13 Xyz, 14 Xyw, 15 Xyzw }
    Lod { 1 Lz, 2 Lb, 3 Ll, 6 Lba, 7 Lla }
    LogicOp { 0 And, 1 Or, 2 Xor, 3 PassB }
    Lrs { 0 None, 1 RightShift, 2 LeftShift }
    LsSize { 0 U8, 1 S8, 2 U16, 3 S16, 4 B32, 5 B64, 6 B128, 7 UB128 }
    LsSize2 { 0 U8, 1 S8, 2 U16, 3 S16, 4 B32, 5 B64, 6 B128 }
    MaxShift { 2 U64, 3 S64 }
    Membar { 0 Cta, 1 Gl, 2 Sys, 3 Vc }
    MufuOp { 0 Cos, 1 Sin, 2 Ex2, 3 Lg2, 4 Rcp, 5 Rsq, 6 Rcp64h, 7 Rsq64h, 8 Sqrt }
    MultiplyScale { 0 NoScale, 1 D2, 2 D4, 3 D8, 4 M8, 5 M4, 6 M2 }
    OFmt { 0 F16, 1 F32, 2 MrgH0, 3 MrgH1 }
    OutType { 1 Emit, 2 Cut, 3 EmitThenCut }
    PMode { 1 F4e, 2 B4e, 3 Rc8, 4 Ecl, 5 Ecr, 6 Rc16 }
    PixMode { 1 Covmask, 2 Covered, 3 Offset, 4 CentroidOffset, 5 MyIndex }
    PredicateOp { 0 F, 1 T, 2 Z, 3 Nz }
    RedOp { 0 Add, 1 Min, 2 Max, 3 Inc, 4 Dec, 5 And, 6 Or, 7 Xor }
    RoundMode { 0 Rn, 1 Rm, 2 Rp, 3 Rz }
    RoundMode2 { 0 Round, 1 Floor, 2 Ceil, 3 Trunc }
    SReg { 0 LaneId, 1 Clock, 2 VirtCfg, 3 VirtId, 4 Pm0, 5 Pm1, 6 Pm2, 7 Pm3, 8 Pm4, 9 Pm5, 10 Pm6, 11 Pm7, 15 OrderingTicket, 16 PrimType, 17 InvocationId, 18 YDirection, 19 ThreadKill, 20 ShaderType, 21 DirectCbeWriteAddressLow, 22 DirectCbeWriteAddressHigh, 23 DirectCbeWriteEnabled, 24 MachineId0, 25 MachineId1, 26 MachineId2, 27 MachineId3, 28 Affinity, 29 InvocationInfo, 30 WScaleFactorXY, 31 WScaleFactorZ, 32 TId, 33 TIdX, 34 TIdY, 35 TIdZ, 36 CtaParam, 37 CtaIdX, 38 CtaIdY, 39 CtaIdZ, 40 Ntid, 41 CirQueueIncrMinusOne, 42 Nlatc, 48 Swinlo, 49 Swinsz, 50 Smemsz, 51 Smembanks, 52 LWinLo, 53 LWinSz, 54 LMemLoSz, 55 LMemHiOff, 56 EqMask, 57 LtMask, 58 LeMask, 59 GtMask, 60 GeMask, 61 RegAlloc, 62 CtxAddr, 64 GlobalErrorStatus, 66 WarpErrorStatus, 67 WarpErrorStatusClear, 72 PmHi0, 73 PmHi1, 74 PmHi2, 75 PmHi3, 76 PmHi4, 77 PmHi5, 78 PmHi6, 79 PmHi7, 80 ClockLo, 81 ClockHi, 82 GlobalTimerLo, 83 GlobalTimerHi, 96 HwTaskId, 97 CircularQueueEntryIndex, 98 CircularQueueEntryAddressLow, 99 CircularQueueEntryAddressHigh }
    ShflMode { 0 Idx, 1 Up, 2 Down, 3 Bfly }
    SuDim { 0 _1d, 1 _1dBuffer, 2 _1dArray, 3 _2d, 4 _2dArray, 5 _3d }
    SuRgba { 1 R, 2 G, 3 Rg, 4 B, 5 Rb, 6 Gb, 7 Rgb, 8 A, 9 Ra, 10 Ga, 11 Rga, 12 Ba, 13 Rba, 14 Gba, 15 Rgba }
    SuSize { 0 U8, 1 S8, 2 U16, 3 S16, 4 B32, 5 B64, 6 B128, 7 UB128 }
    SuatomOp { 0 Add, 1 Min, 2 Max, 3 Inc, 4 Dec, 5 And, 6 Or, 7 Xor, 8 Exch }
    SuatomSize { 0 U32, 1 S32, 2 U64, 3 F32FtzRn, 4 F16x2FtzRn, 5 S64, 6 Sd32, 7 Sd64 }
    TexComp { 0 R, 1 G, 2 B, 3 A }
    TexDim { 0 _1d, 1 Array1d, 2 _2d, 3 Array2d, 4 _3d, 5 Array3d, 6 Cube, 7 ArrayCube }
    TexOffset { 0 None, 1 Aoffi, 2 Ptp }
    TexQuery { 1 TexHeaderDimension, 2 TexHeaderTextureType, 5 TexHeaderSamplerPos, 16 TexSamplerFilter, 18 TexSamplerLod, 20 TexSamplerWrap, 22 TexSamplerBorderColor }
    TexsTarget { 0 Texture1DLodZero, 1 Texture2D, 2 Texture2DLodZero, 3 Texture2DLodLevel, 4 Texture2DDepthCompare, 5 Texture2DLodLevelDepthCompare, 6 Texture2DLodZeroDepthCompare, 7 Texture2DArray, 8 Texture2DArrayLodZero, 9 Texture2DArrayLodZeroDepthCompare, 10 Texture3D, 11 Texture3DLodZero, 12 TextureCube, 13 TextureCubeLodLevel }
    TldsTarget { 0 Texture1DLodZero, 1 Texture1DLodLevel, 2 Texture2DLodZero, 4 Texture2DLodZeroOffset, 5 Texture2DLodLevel, 6 Texture2DLodZeroMultisample, 7 Texture3DLodZero, 8 Texture2DArrayLodZero, 12 Texture2DLodLevelOffset }
    VectorSelect { 0 U8B0, 1 U8B1, 2 U8B2, 3 U8B3, 4 U16H0, 5 U16H1, 6 U32, 8 S8B0, 9 S8B1, 10 S8B2, 11 S8B3, 12 S16H0, 13 S16H1, 14 S32 }
    VideoOp { 0 Mrg16h, 1 Mrg16l, 2 Mrg8b0, 3 Mrg8b2, 4 Acc, 5 Min, 6 Max }
    VideoRed { 1 Acc }
    VideoScale { 1 Shr7, 2 Shr15 }
    VoteMode { 0 All, 1 Any, 2 Eq }
    XMode { 1 Xlo, 2 Xmed, 3 Xhi }
    XModeShf { 1 Hi, 2 X, 3 Xhi }
    XmadCop { 0 Cfull, 1 Clo, 2 Chi, 3 Csfu, 4 Cbcc }
    XmadCop2 { 0 Cfull, 1 Clo, 2 Chi, 3 Csfu }
}

forms! {
    Al2p { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, AlSize: AlSize = 47 & 0x3, Aio: bool = 0 & 0x100000000, Imm11: int = 20 & 0x7ff, DestPred: int = 44 & 0x7 }
    Ald { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Imm11: int = 20 & 0x7ff, P: bool = 0 & 0x80000000, O: bool = 0 & 0x100000000, AlSize: AlSize = 47 & 0x3 }
    Ast { SrcA: int = 8 & 0xff, SrcB: int = 0 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Imm11: int = 20 & 0x7ff, P: bool = 0 & 0x80000000, AlSize: AlSize = 47 & 0x3 }
    Atom { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Imm20: int = 28 & 0xfffff, Size: AtomSize = 49 & 0x7, Op: AtomOp = 52 & 0xf, E: bool = 0 & 0x1000000000000 }
    AtomCas { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, BcRz: int = 50 & 0x3, E: bool = 0 & 0x1000000000000 }
    Atoms { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Imm22: int = 30 & 0x3fffff, AtomsSize: AtomsSize = 28 & 0x3, AtomOp: AtomOp = 52 & 0xf }
    AtomsCas { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, AtomsBcRz: int = 28 & 0x3 }
    B2r { Dest: int = 0 & 0xff, DestPred: int = 45 & 0x7, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Mode: BarMode = 32 & 0x3 }
    Bar { SrcA: int = 8 & 0xff, Imm12: int = 20 & 0xfff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, SrcPredInv: bool = 0 & 0x40000000000, SrcPred: int = 39 & 0x7, BarOp: BarOp = 32 & 0x7, BarRedOp: BarRedOp = 35 & 0x3, AFixBar: bool = 0 & 0x100000000000, BFixBar: bool = 0 & 0x80000000000 }
    BfeC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Signed: bool = 0 & 0x1000000000000, Brev: bool = 0 & 0x10000000000 }
    BfeI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Signed: bool = 0 & 0x1000000000000, Brev: bool = 0 & 0x10000000000 }
    BfeR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Signed: bool = 0 & 0x1000000000000, Brev: bool = 0 & 0x10000000000 }
    BfiC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000 }
    BfiI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000 }
    BfiR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000 }
    BfiRc { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcC: int = 39 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000 }
    Bpt { Imm20: int = 20 & 0xfffff, Bpt: Bpt = 6 & 0x7 }
    Bra { Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Ccc: Ccc = 0 & 0x1f, Imm24: int = 20 & 0xffffff, Ca: bool = 0 & 0x20, Lmt: bool = 0 & 0x40, U: bool = 0 & 0x80 }
    Brk { Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Ccc: Ccc = 0 & 0x1f }
    Brx { SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Ccc: Ccc = 0 & 0x1f, Imm24: int = 20 & 0xffffff, Ca: bool = 0 & 0x20, Lmt: bool = 0 & 0x40 }
    Cal { Ca: bool = 0 & 0x20, Imm24: int = 20 & 0xffffff, Inc: bool = 0 & 0x40 }
    Cctl { SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Imm30: int = 22 & 0x3fffffff, E: bool = 0 & 0x10000000000000, Cache: CacheType = 4 & 0x7, CctlOp: CctlOp = 0 & 0xf }
    Cctll { SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Imm22: int = 22 & 0x3fffff, Cache: int = 4 & 0x3, CctlOp: CctlOp = 0 & 0xf }
    Cctlt { Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, TsIdx13: int = 36 & 0x1fff, CctltOp: CctltOp = 0 & 0x3 }
    CctltR { SrcC: int = 39 & 0xff, CctltOp: CctltOp = 0 & 0x3 }
    Cont { Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Ccc: Ccc = 0 & 0x1f }
    Cs2r { Dest: int = 0 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, SReg: SReg = 20 & 0xff }
    Cset { Dest: int = 0 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Ccc: Ccc = 8 & 0x1f, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000, BVal: bool = 0 & 0x100000000000, Bop: BoolOp = 45 & 0x3 }
    Csetp { Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Ccc: Ccc = 8 & 0x1f, DestPred: int = 3 & 0x7, DestPredInv: int = 0 & 0x7, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000, Bop: BoolOp = 45 & 0x3 }
    DaddC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, AbsB: bool = 0 & 0x2000000000000, NegA: bool = 0 & 0x1000000000000, AbsA: bool = 0 & 0x400000000000, NegB: bool = 0 & 0x200000000000, RoundMode: RoundMode = 39 & 0x3 }
    DaddI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, AbsB: bool = 0 & 0x2000000000000, NegA: bool = 0 & 0x1000000000000, AbsA: bool = 0 & 0x400000000000, NegB: bool = 0 & 0x200000000000, RoundMode: RoundMode = 39 & 0x3 }
    DaddR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, AbsB: bool = 0 & 0x2000000000000, NegA: bool = 0 & 0x1000000000000, AbsA: bool = 0 & 0x400000000000, NegB: bool = 0 & 0x200000000000, RoundMode: RoundMode = 39 & 0x3 }
    Depbar { Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Le: bool = 0 & 0x20000000, Sbid: int = 26 & 0x7, PendCnt: int = 20 & 0x3f, Imm6: int = 0 & 0x3f }
    DfmaC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, RoundMode: RoundMode = 50 & 0x3, NegC: bool = 0 & 0x2000000000000, NegA: bool = 0 & 0x1000000000000 }
    DfmaI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, RoundMode: RoundMode = 50 & 0x3, NegC: bool = 0 & 0x2000000000000, NegA: bool = 0 & 0x1000000000000 }
    DfmaR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, RoundMode: RoundMode = 50 & 0x3, NegC: bool = 0 & 0x2000000000000, NegA: bool = 0 & 0x1000000000000 }
    DfmaRc { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcC: int = 39 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, RoundMode: RoundMode = 50 & 0x3, NegC: bool = 0 & 0x2000000000000, NegA: bool = 0 & 0x1000000000000 }
    DmnmxC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, AbsB: bool = 0 & 0x2000000000000, NegA: bool = 0 & 0x1000000000000, AbsA: bool = 0 & 0x400000000000, NegB: bool = 0 & 0x200000000000, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000 }
    DmnmxI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, AbsB: bool = 0 & 0x2000000000000, NegA: bool = 0 & 0x1000000000000, AbsA: bool = 0 & 0x400000000000, NegB: bool = 0 & 0x200000000000, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000 }
    DmnmxR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, AbsB: bool = 0 & 0x2000000000000, NegA: bool = 0 & 0x1000000000000, AbsA: bool = 0 & 0x400000000000, NegB: bool = 0 & 0x200000000000, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000 }
    DmulC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, RoundMode: RoundMode = 39 & 0x3, NegA: bool = 0 & 0x1000000000000 }
    DmulI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, RoundMode: RoundMode = 39 & 0x3, NegA: bool = 0 & 0x1000000000000 }
    DmulR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, RoundMode: RoundMode = 39 & 0x3, NegA: bool = 0 & 0x1000000000000 }
    DsetC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, AbsA: bool = 0 & 0x40000000000000, NegB: bool = 0 & 0x20000000000000, BVal: bool = 0 & 0x10000000000000, FComp: FComp = 48 & 0xf, Bop: BoolOp = 45 & 0x3, AbsB: bool = 0 & 0x100000000000, NegA: bool = 0 & 0x80000000000, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000 }
    DsetI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, AbsA: bool = 0 & 0x40000000000000, NegB: bool = 0 & 0x20000000000000, BVal: bool = 0 & 0x10000000000000, FComp: FComp = 48 & 0xf, Bop: BoolOp = 45 & 0x3, AbsB: bool = 0 & 0x100000000000, NegA: bool = 0 & 0x80000000000, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000 }
    DsetR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, AbsA: bool = 0 & 0x40000000000000, NegB: bool = 0 & 0x20000000000000, BVal: bool = 0 & 0x10000000000000, FComp: FComp = 48 & 0xf, Bop: BoolOp = 45 & 0x3, AbsB: bool = 0 & 0x100000000000, NegA: bool = 0 & 0x80000000000, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000 }
    DsetpC { SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, FComp: FComp = 48 & 0xf, Bop: BoolOp = 45 & 0x3, AbsB: bool = 0 & 0x100000000000, NegA: bool = 0 & 0x80000000000, SrcPredInv: bool = 0 & 0x40000000000, SrcPred: int = 39 & 0x7, AbsA: bool = 0 & 0x80, NegB: bool = 0 & 0x40, DestPred: int = 3 & 0x7, DestPredInv: int = 0 & 0x7 }
    DsetpI { SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, FComp: FComp = 48 & 0xf, Bop: BoolOp = 45 & 0x3, AbsB: bool = 0 & 0x100000000000, NegA: bool = 0 & 0x80000000000, SrcPredInv: bool = 0 & 0x40000000000, SrcPred: int = 39 & 0x7, AbsA: bool = 0 & 0x80, NegB: bool = 0 & 0x40, DestPred: int = 3 & 0x7, DestPredInv: int = 0 & 0x7 }
    DsetpR { SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, FComp: FComp = 48 & 0xf, Bop: BoolOp = 45 & 0x3, AbsB: bool = 0 & 0x100000000000, NegA: bool = 0 & 0x80000000000, SrcPredInv: bool = 0 & 0x40000000000, SrcPred: int = 39 & 0x7, AbsA: bool = 0 & 0x80, NegB: bool = 0 & 0x40, DestPred: int = 3 & 0x7, DestPredInv: int = 0 & 0x7 }
    Exit { Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Ccc: Ccc = 0 & 0x1f, KeepRefCnt: bool = 0 & 0x20 }
    F2fC { Dest: int = 0 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, AbsB: bool = 0 & 0x2000000000000, NegB: bool = 0 & 0x200000000000, Ftz: bool = 0 & 0x100000000000, DstFmt: DstFmt = 8 & 0x3, SrcFmt: DstFmt = 10 & 0x3, RoundMode: IntegerRound = 40 & 0x4 | 39 & 0x3, Sh: bool = 0 & 0x20000000000, Sat: bool = 0 & 0x4000000000000 }
    F2fI { Dest: int = 0 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, AbsB: bool = 0 & 0x2000000000000, NegB: bool = 0 & 0x200000000000, Ftz: bool = 0 & 0x100000000000, DstFmt: DstFmt = 8 & 0x3, SrcFmt: DstFmt = 10 & 0x3, RoundMode: IntegerRound = 40 & 0x4 | 39 & 0x3, Sh: bool = 0 & 0x20000000000, Sat: bool = 0 & 0x4000000000000 }
    F2fR { Dest: int = 0 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, AbsB: bool = 0 & 0x2000000000000, NegB: bool = 0 & 0x200000000000, Ftz: bool = 0 & 0x100000000000, DstFmt: DstFmt = 8 & 0x3, SrcFmt: DstFmt = 10 & 0x3, RoundMode: IntegerRound = 40 & 0x4 | 39 & 0x3, Sh: bool = 0 & 0x20000000000, Sat: bool = 0 & 0x4000000000000 }
    F2iC { Dest: int = 0 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, AbsB: bool = 0 & 0x2000000000000, NegB: bool = 0 & 0x200000000000, Ftz: bool = 0 & 0x100000000000, Sh: bool = 0 & 0x20000000000, IDstFmt: IDstFmt = 10 & 0x4 | 8 & 0x3, SrcFmt: DstFmt = 10 & 0x3, RoundMode: RoundMode2 = 39 & 0x3 }
    F2iI { Dest: int = 0 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, AbsB: bool = 0 & 0x2000000000000, NegB: bool = 0 & 0x200000000000, Ftz: bool = 0 & 0x100000000000, Sh: bool = 0 & 0x20000000000, IDstFmt: IDstFmt = 10 & 0x4 | 8 & 0x3, SrcFmt: DstFmt = 10 & 0x3, RoundMode: RoundMode2 = 39 & 0x3 }
    F2iR { Dest: int = 0 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, AbsB: bool = 0 & 0x2000000000000, NegB: bool = 0 & 0x200000000000, Ftz: bool = 0 & 0x100000000000, Sh: bool = 0 & 0x20000000000, IDstFmt: IDstFmt = 10 & 0x4 | 8 & 0x3, SrcFmt: DstFmt = 10 & 0x3, RoundMode: RoundMode2 = 39 & 0x3 }
    Fadd32i { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x10000000000000, Imm32: int = 20 & 0xffffffff, AbsB: bool = 0 & 0x200000000000000, NegA: bool = 0 & 0x100000000000000, Ftz: bool = 0 & 0x80000000000000, AbsA: bool = 0 & 0x40000000000000, NegB: bool = 0 & 0x20000000000000 }
    FaddC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Sat: bool = 0 & 0x4000000000000, AbsB: bool = 0 & 0x2000000000000, NegA: bool = 0 & 0x1000000000000, AbsA: bool = 0 & 0x400000000000, NegB: bool = 0 & 0x200000000000, Ftz: bool = 0 & 0x100000000000, RoundMode: RoundMode = 39 & 0x3 }
    FaddI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Sat: bool = 0 & 0x4000000000000, AbsB: bool = 0 & 0x2000000000000, NegA: bool = 0 & 0x1000000000000, AbsA: bool = 0 & 0x400000000000, NegB: bool = 0 & 0x200000000000, Ftz: bool = 0 & 0x100000000000, RoundMode: RoundMode = 39 & 0x3 }
    FaddR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Sat: bool = 0 & 0x4000000000000, AbsB: bool = 0 & 0x2000000000000, NegA: bool = 0 & 0x1000000000000, AbsA: bool = 0 & 0x400000000000, NegB: bool = 0 & 0x200000000000, Ftz: bool = 0 & 0x100000000000, RoundMode: RoundMode = 39 & 0x3 }
    FchkC { DestPred: int = 3 & 0x7, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, AbsB: bool = 0 & 0x2000000000000, NegA: bool = 0 & 0x1000000000000, AbsA: bool = 0 & 0x400000000000, NegB: bool = 0 & 0x200000000000, ChkModeF: ChkModeF = 39 & 0x3f }
    FchkI { DestPred: int = 3 & 0x7, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, AbsB: bool = 0 & 0x2000000000000, NegA: bool = 0 & 0x1000000000000, AbsA: bool = 0 & 0x400000000000, NegB: bool = 0 & 0x200000000000, ChkModeF: ChkModeF = 39 & 0x3f }
    FchkR { DestPred: int = 3 & 0x7, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, AbsB: bool = 0 & 0x2000000000000, NegA: bool = 0 & 0x1000000000000, AbsA: bool = 0 & 0x400000000000, NegB: bool = 0 & 0x200000000000, ChkModeF: ChkModeF = 39 & 0x3f }
    FcmpC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, FComp: FComp = 48 & 0xf, Ftz: bool = 0 & 0x800000000000 }
    FcmpI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, FComp: FComp = 48 & 0xf, Ftz: bool = 0 & 0x800000000000 }
    FcmpR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, FComp: FComp = 48 & 0xf, Ftz: bool = 0 & 0x800000000000 }
    FcmpRc { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcC: int = 39 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, FComp: FComp = 48 & 0xf, Ftz: bool = 0 & 0x800000000000 }
    Ffma32i { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm32: int = 20 & 0xffffffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, NegC: bool = 0 & 0x200000000000000, NegA: bool = 0 & 0x100000000000000, Sat: bool = 0 & 0x80000000000000, WriteCC: bool = 0 & 0x10000000000000, Fmz: Fmz = 53 & 0x3 }
    FfmaC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, NegA: bool = 0 & 0x1000000000000, NegC: bool = 0 & 0x2000000000000, Sat: bool = 0 & 0x4000000000000, RoundMode: RoundMode = 51 & 0x3, Fmz: Fmz = 53 & 0x3 }
    FfmaI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, NegA: bool = 0 & 0x1000000000000, NegC: bool = 0 & 0x2000000000000, Sat: bool = 0 & 0x4000000000000, RoundMode: RoundMode = 51 & 0x3, Fmz: Fmz = 53 & 0x3 }
    FfmaR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, NegA: bool = 0 & 0x1000000000000, NegC: bool = 0 & 0x2000000000000, Sat: bool = 0 & 0x4000000000000, RoundMode: RoundMode = 51 & 0x3, Fmz: Fmz = 53 & 0x3 }
    FfmaRc { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcC: int = 39 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, NegA: bool = 0 & 0x1000000000000, NegC: bool = 0 & 0x2000000000000, Sat: bool = 0 & 0x4000000000000, RoundMode: RoundMode = 51 & 0x3, Fmz: Fmz = 53 & 0x3 }
    FloC { Dest: int = 0 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Signed: bool = 0 & 0x1000000000000, Sh: bool = 0 & 0x20000000000, NegB: bool = 0 & 0x10000000000 }
    FloI { Dest: int = 0 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Signed: bool = 0 & 0x1000000000000, Sh: bool = 0 & 0x20000000000, NegB: bool = 0 & 0x10000000000 }
    FloR { Dest: int = 0 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Signed: bool = 0 & 0x1000000000000, Sh: bool = 0 & 0x20000000000, NegB: bool = 0 & 0x10000000000 }
    FmnmxC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, AbsB: bool = 0 & 0x2000000000000, NegA: bool = 0 & 0x1000000000000, AbsA: bool = 0 & 0x400000000000, NegB: bool = 0 & 0x200000000000, Ftz: bool = 0 & 0x100000000000, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000 }
    FmnmxI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, AbsB: bool = 0 & 0x2000000000000, NegA: bool = 0 & 0x1000000000000, AbsA: bool = 0 & 0x400000000000, NegB: bool = 0 & 0x200000000000, Ftz: bool = 0 & 0x100000000000, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000 }
    FmnmxR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, AbsB: bool = 0 & 0x2000000000000, NegA: bool = 0 & 0x1000000000000, AbsA: bool = 0 & 0x400000000000, NegB: bool = 0 & 0x200000000000, Ftz: bool = 0 & 0x100000000000, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000 }
    Fmul32i { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Imm32: int = 20 & 0xffffffff, Sat: bool = 0 & 0x80000000000000, Fmz: Fmz = 53 & 0x3, WriteCC: bool = 0 & 0x10000000000000 }
    FmulC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, RoundMode: RoundMode = 39 & 0x3, Fmz: Fmz = 44 & 0x3, Scale: MultiplyScale = 41 & 0x7, NegA: bool = 0 & 0x1000000000000, Sat: bool = 0 & 0x4000000000000 }
    FmulI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, RoundMode: RoundMode = 39 & 0x3, Fmz: Fmz = 44 & 0x3, Scale: MultiplyScale = 41 & 0x7, NegA: bool = 0 & 0x1000000000000, Sat: bool = 0 & 0x4000000000000 }
    FmulR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, RoundMode: RoundMode = 39 & 0x3, Fmz: Fmz = 44 & 0x3, Scale: MultiplyScale = 41 & 0x7, NegA: bool = 0 & 0x1000000000000, Sat: bool = 0 & 0x4000000000000 }
    FsetC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, NegA: bool = 0 & 0x80000000000, NegB: bool = 0 & 0x20000000000000, AbsA: bool = 0 & 0x40000000000000, AbsB: bool = 0 & 0x100000000000, FComp: FComp = 48 & 0xf, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000, Bop: BoolOp = 45 & 0x3, Ftz: bool = 0 & 0x80000000000000, BVal: bool = 0 & 0x10000000000000 }
    FsetI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, NegA: bool = 0 & 0x80000000000, NegB: bool = 0 & 0x20000000000000, AbsA: bool = 0 & 0x40000000000000, AbsB: bool = 0 & 0x100000000000, FComp: FComp = 48 & 0xf, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000, Bop: BoolOp = 45 & 0x3, Ftz: bool = 0 & 0x80000000000000, BVal: bool = 0 & 0x10000000000000 }
    FsetR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, NegA: bool = 0 & 0x80000000000, NegB: bool = 0 & 0x20000000000000, AbsA: bool = 0 & 0x40000000000000, AbsB: bool = 0 & 0x100000000000, FComp: FComp = 48 & 0xf, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000, Bop: BoolOp = 45 & 0x3, Ftz: bool = 0 & 0x80000000000000, BVal: bool = 0 & 0x10000000000000 }
    FsetpC { DestPred: int = 3 & 0x7, DestPredInv: int = 0 & 0x7, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, NegA: bool = 0 & 0x80000000000, NegB: bool = 0 & 0x40, AbsA: bool = 0 & 0x80, AbsB: bool = 0 & 0x100000000000, FComp: FComp = 48 & 0xf, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000, Bop: BoolOp = 45 & 0x3, Ftz: bool = 0 & 0x800000000000 }
    FsetpI { DestPred: int = 3 & 0x7, DestPredInv: int = 0 & 0x7, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, NegA: bool = 0 & 0x80000000000, NegB: bool = 0 & 0x40, AbsA: bool = 0 & 0x80, AbsB: bool = 0 & 0x100000000000, FComp: FComp = 48 & 0xf, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000, Bop: BoolOp = 45 & 0x3, Ftz: bool = 0 & 0x800000000000 }
    FsetpR { DestPred: int = 3 & 0x7, DestPredInv: int = 0 & 0x7, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, NegA: bool = 0 & 0x80000000000, NegB: bool = 0 & 0x40, AbsA: bool = 0 & 0x80, AbsB: bool = 0 & 0x100000000000, FComp: FComp = 48 & 0xf, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000, Bop: BoolOp = 45 & 0x3, Ftz: bool = 0 & 0x800000000000 }
    Fswzadd { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Ftz: bool = 0 & 0x100000000000, RoundMode: RoundMode = 39 & 0x3, Ndv: bool = 0 & 0x4000000000, PnWord: int = 28 & 0xff }
    Getcrsptr { Dest: int = 0 & 0xff }
    Getlmembase { Dest: int = 0 & 0xff }
    Hadd232i { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm: int = 20 & 0xffffffff, ASwizzle: HalfSwizzle = 53 & 0x3, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, NegA: bool = 0 & 0x100000000000000, Sat: bool = 0 & 0x10000000000000, Ftz: bool = 0 & 0x80000000000000 }
    Hadd2C { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, OFmt: OFmt = 49 & 0x3, ASwizzle: HalfSwizzle = 47 & 0x3, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, NegA: bool = 0 & 0x80000000000, NegB: bool = 0 & 0x100000000000000, AbsA: bool = 0 & 0x100000000000, AbsB: bool = 0 & 0x40000000000000, Sat: bool = 0 & 0x10000000000000, Ftz: bool = 0 & 0x8000000000 }
    Hadd2I { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, BimmH0: int = 20 & 0x3ff, BimmH1: int = 47 & 0x200 | 30 & 0x1ff, OFmt: OFmt = 49 & 0x3, ASwizzle: HalfSwizzle = 47 & 0x3, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, NegA: bool = 0 & 0x80000000000, AbsA: bool = 0 & 0x100000000000, Sat: bool = 0 & 0x10000000000000, Ftz: bool = 0 & 0x8000000000 }
    Hadd2R { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, OFmt: OFmt = 49 & 0x3, ASwizzle: HalfSwizzle = 47 & 0x3, BSwizzle: HalfSwizzle = 28 & 0x3, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, NegA: bool = 0 & 0x80000000000, NegB: bool = 0 & 0x80000000, AbsA: bool = 0 & 0x100000000000, AbsB: bool = 0 & 0x40000000, Sat: bool = 0 & 0x100000000, Ftz: bool = 0 & 0x8000000000 }
    Hfma232i { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm: int = 20 & 0xffffffff, ASwizzle: HalfSwizzle = 47 & 0x3, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, NegC: bool = 0 & 0x8000000000000, Fmz: Fmz = 57 & 0x3 }
    Hfma2C { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, SrcC: int = 39 & 0xff, OFmt: OFmt = 49 & 0x3, ASwizzle: HalfSwizzle = 47 & 0x3, CSwizzle: HalfSwizzle = 53 & 0x3, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, NegA: bool = 0 & 0x100000000000000, NegC: bool = 0 & 0x8000000000000, Sat: bool = 0 & 0x10000000000000, Fmz: Fmz = 57 & 0x3 }
    Hfma2I { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, BimmH0: int = 20 & 0x3ff, BimmH1: int = 47 & 0x200 | 30 & 0x1ff, SrcC: int = 39 & 0xff, OFmt: OFmt = 49 & 0x3, ASwizzle: HalfSwizzle = 47 & 0x3, CSwizzle: HalfSwizzle = 53 & 0x3, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, NegC: bool = 0 & 0x8000000000000, Sat: bool = 0 & 0x10000000000000, Fmz: Fmz = 57 & 0x3 }
    Hfma2R { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, OFmt: OFmt = 49 & 0x3, ASwizzle: HalfSwizzle = 47 & 0x3, BSwizzle: HalfSwizzle = 28 & 0x3, CSwizzle: HalfSwizzle = 35 & 0x3, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, NegA: bool = 0 & 0x80000000, NegC: bool = 0 & 0x40000000, Sat: bool = 0 & 0x100000000, Fmz: Fmz = 37 & 0x3 }
    Hfma2Rc { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, SrcC: int = 39 & 0xff, OFmt: OFmt = 49 & 0x3, ASwizzle: HalfSwizzle = 47 & 0x3, CSwizzle: HalfSwizzle = 53 & 0x3, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, NegA: bool = 0 & 0x100000000000000, NegC: bool = 0 & 0x8000000000000, Sat: bool = 0 & 0x10000000000000, Fmz: Fmz = 57 & 0x3 }
    Hmul232i { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm32: int = 20 & 0xffffffff, ASwizzle: HalfSwizzle = 53 & 0x3, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Sat: bool = 0 & 0x10000000000000, Fmz: Fmz = 55 & 0x3 }
    Hmul2C { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, OFmt: OFmt = 49 & 0x3, ASwizzle: HalfSwizzle = 47 & 0x3, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, NegA: bool = 0 & 0x80000000000, AbsA: bool = 0 & 0x100000000000, AbsB: bool = 0 & 0x40000000000000, Sat: bool = 0 & 0x10000000000000, Fmz: Fmz = 39 & 0x3 }
    Hmul2I { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, BimmH0: int = 20 & 0x3ff, BimmH1: int = 47 & 0x200 | 30 & 0x1ff, OFmt: OFmt = 49 & 0x3, ASwizzle: HalfSwizzle = 47 & 0x3, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, NegA: bool = 0 & 0x80000000000, AbsA: bool = 0 & 0x100000000000, Sat: bool = 0 & 0x10000000000000, Fmz: Fmz = 39 & 0x3 }
    Hmul2R { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, OFmt: OFmt = 49 & 0x3, ASwizzle: HalfSwizzle = 47 & 0x3, BSwizzle: HalfSwizzle = 28 & 0x3, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, NegA: bool = 0 & 0x80000000, AbsA: bool = 0 & 0x100000000000, AbsB: bool = 0 & 0x40000000, Sat: bool = 0 & 0x100000000, Fmz: Fmz = 39 & 0x3 }
    Hset2C { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, ASwizzle: HalfSwizzle = 47 & 0x3, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, NegA: bool = 0 & 0x80000000000, AbsA: bool = 0 & 0x100000000000, NegB: bool = 0 & 0x100000000000000, Bval: bool = 0 & 0x20000000000000, Cmp: FComp = 49 & 0xf, Bop: BoolOp = 45 & 0x3, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000, Ftz: bool = 0 & 0x40000000000000 }
    Hset2I { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, BimmH0: int = 20 & 0x3ff, BimmH1: int = 47 & 0x200 | 30 & 0x1ff, ASwizzle: HalfSwizzle = 47 & 0x3, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, NegA: bool = 0 & 0x80000000000, AbsA: bool = 0 & 0x100000000000, Bval: bool = 0 & 0x20000000000000, Cmp: FComp = 49 & 0xf, Bop: BoolOp = 45 & 0x3, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000, Ftz: bool = 0 & 0x40000000000000 }
    Hset2R { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, ASwizzle: HalfSwizzle = 47 & 0x3, BSwizzle: HalfSwizzle = 28 & 0x3, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, NegA: bool = 0 & 0x80000000000, AbsA: bool = 0 & 0x100000000000, NegB: bool = 0 & 0x80000000, AbsB: bool = 0 & 0x40000000, Bval: bool = 0 & 0x2000000000000, Cmp: FComp = 35 & 0xf, Bop: BoolOp = 45 & 0x3, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000, Ftz: bool = 0 & 0x4000000000000 }
    Hsetp2C { DestPred: int = 3 & 0x7, DestPredInv: int = 0 & 0x7, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, NegA: bool = 0 & 0x80000000000, NegB: bool = 0 & 0x100000000000000, AbsA: bool = 0 & 0x100000000000, AbsB: bool = 0 & 0x40000000000000, FComp: FComp = 49 & 0xf, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000, Bop: BoolOp = 45 & 0x3, Ftz: bool = 0 & 0x40, HAnd: bool = 0 & 0x20000000000000, ASwizzle: HalfSwizzle = 47 & 0x3 }
    Hsetp2I { DestPred: int = 3 & 0x7, DestPredInv: int = 0 & 0x7, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, BimmH0: int = 20 & 0x3ff, BimmH1: int = 47 & 0x200 | 30 & 0x1ff, NegA: bool = 0 & 0x80000000000, AbsA: bool = 0 & 0x100000000000, FComp: FComp = 49 & 0xf, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000, Bop: BoolOp = 45 & 0x3, Ftz: bool = 0 & 0x40, HAnd: bool = 0 & 0x20000000000000, ASwizzle: HalfSwizzle = 47 & 0x3 }
    Hsetp2R { DestPred: int = 3 & 0x7, DestPredInv: int = 0 & 0x7, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, NegA: bool = 0 & 0x80000000000, NegB: bool = 0 & 0x80000000, AbsA: bool = 0 & 0x100000000000, AbsB: bool = 0 & 0x40000000, FComp2: FComp = 35 & 0xf, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000, Bop: BoolOp = 45 & 0x3, Ftz: bool = 0 & 0x40, HAnd: bool = 0 & 0x2000000000000, ASwizzle: HalfSwizzle = 47 & 0x3, BSwizzle: HalfSwizzle = 28 & 0x3 }
    I2fC { Dest: int = 0 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, AbsB: bool = 0 & 0x2000000000000, NegB: bool = 0 & 0x200000000000, ByteSel: ByteSel = 41 & 0x3, RoundMode: RoundMode = 39 & 0x3, ISrcFmt: ISrcFmt = 11 & 0x4 | 10 & 0x3, DstFmt: DstFmt = 8 & 0x3 }
    I2fI { Dest: int = 0 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, AbsB: bool = 0 & 0x2000000000000, NegB: bool = 0 & 0x200000000000, ByteSel: ByteSel = 41 & 0x3, RoundMode: RoundMode = 39 & 0x3, ISrcFmt: ISrcFmt = 11 & 0x4 | 10 & 0x3, DstFmt: DstFmt = 8 & 0x3 }
    I2fR { Dest: int = 0 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, AbsB: bool = 0 & 0x2000000000000, NegB: bool = 0 & 0x200000000000, ByteSel: ByteSel = 41 & 0x3, RoundMode: RoundMode = 39 & 0x3, ISrcFmt: ISrcFmt = 11 & 0x4 | 10 & 0x3, DstFmt: DstFmt = 8 & 0x3 }
    I2iC { Dest: int = 0 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Sat: bool = 0 & 0x4000000000000, AbsB: bool = 0 & 0x2000000000000, NegB: bool = 0 & 0x200000000000, ByteSel: ByteSel = 41 & 0x3, IDstFmt: ISrcDstFmt = 10 & 0x4 | 8 & 0x3, ISrcFmt: ISrcDstFmt = 11 & 0x4 | 10 & 0x3 }
    I2iI { Dest: int = 0 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Sat: bool = 0 & 0x4000000000000, AbsB: bool = 0 & 0x2000000000000, NegB: bool = 0 & 0x200000000000, ByteSel: ByteSel = 41 & 0x3, IDstFmt: ISrcDstFmt = 10 & 0x4 | 8 & 0x3, ISrcFmt: ISrcDstFmt = 11 & 0x4 | 10 & 0x3 }
    I2iR { Dest: int = 0 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Sat: bool = 0 & 0x4000000000000, AbsB: bool = 0 & 0x2000000000000, NegB: bool = 0 & 0x200000000000, ByteSel: ByteSel = 41 & 0x3, IDstFmt: ISrcDstFmt = 10 & 0x4 | 8 & 0x3, ISrcFmt: ISrcDstFmt = 11 & 0x4 | 10 & 0x3 }
    Iadd32i { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Imm32: int = 20 & 0xffffffff, AvgMode: AvgMode = 55 & 0x3, Sat: bool = 0 & 0x40000000000000, WriteCC: bool = 0 & 0x10000000000000, X: bool = 0 & 0x20000000000000 }
    Iadd3C { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, NegA: bool = 0 & 0x8000000000000, NegB: bool = 0 & 0x4000000000000, NegC: bool = 0 & 0x2000000000000, X: bool = 0 & 0x1000000000000 }
    Iadd3I { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, NegA: bool = 0 & 0x8000000000000, NegB: bool = 0 & 0x4000000000000, NegC: bool = 0 & 0x2000000000000, X: bool = 0 & 0x1000000000000 }
    Iadd3R { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, NegA: bool = 0 & 0x8000000000000, NegB: bool = 0 & 0x4000000000000, NegC: bool = 0 & 0x2000000000000, X: bool = 0 & 0x1000000000000, Lrs: Lrs = 37 & 0x3, Apart: HalfSelect = 35 & 0x3, Bpart: HalfSelect = 33 & 0x3, Cpart: HalfSelect = 31 & 0x3 }
    IaddC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Sat: bool = 0 & 0x4000000000000, AvgMode: AvgMode = 48 & 0x3, X: bool = 0 & 0x80000000000 }
    IaddI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Sat: bool = 0 & 0x4000000000000, AvgMode: AvgMode = 48 & 0x3, X: bool = 0 & 0x80000000000 }
    IaddR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Sat: bool = 0 & 0x4000000000000, AvgMode: AvgMode = 48 & 0x3, X: bool = 0 & 0x80000000000 }
    IcmpC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, IComp: IComp = 49 & 0x7, Signed: bool = 0 & 0x1000000000000 }
    IcmpI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, IComp: IComp = 49 & 0x7, Signed: bool = 0 & 0x1000000000000 }
    IcmpR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, IComp: IComp = 49 & 0x7, Signed: bool = 0 & 0x1000000000000 }
    IcmpRc { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcC: int = 39 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, IComp: IComp = 49 & 0x7, Signed: bool = 0 & 0x1000000000000 }
    Ide { Imm16: int = 20 & 0xffff, Di: bool = 0 & 0x20 }
    IdpC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, IsHi: bool = 0 & 0x4000000000000, SrcASign: bool = 0 & 0x2000000000000, IsDp: bool = 0 & 0x1000000000000, SrcBSign: bool = 0 & 0x800000000000 }
    IdpR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, IsHi: bool = 0 & 0x4000000000000, SrcASign: bool = 0 & 0x2000000000000, IsDp: bool = 0 & 0x1000000000000, SrcBSign: bool = 0 & 0x800000000000 }
    Imad32i { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Imm32: int = 20 & 0xffffffff, BSigned: bool = 0 & 0x200000000000000, AvgMode: AvgMode = 55 & 0x3, ASigned: bool = 0 & 0x40000000000000, WriteCC: bool = 0 & 0x10000000000000, Hilo: bool = 0 & 0x20000000000000 }
    ImadC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Hilo: bool = 0 & 0x40000000000000, BSigned: bool = 0 & 0x20000000000000, AvgMode: AvgMode = 51 & 0x3, Sat: bool = 0 & 0x4000000000000, X: bool = 0 & 0x2000000000000, ASigned: bool = 0 & 0x1000000000000 }
    ImadI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Hilo: bool = 0 & 0x40000000000000, BSigned: bool = 0 & 0x20000000000000, AvgMode: AvgMode = 51 & 0x3, Sat: bool = 0 & 0x4000000000000, X: bool = 0 & 0x2000000000000, ASigned: bool = 0 & 0x1000000000000 }
    ImadR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Hilo: bool = 0 & 0x40000000000000, BSigned: bool = 0 & 0x20000000000000, AvgMode: AvgMode = 51 & 0x3, Sat: bool = 0 & 0x4000000000000, X: bool = 0 & 0x2000000000000, ASigned: bool = 0 & 0x1000000000000 }
    ImadRc { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcC: int = 39 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Hilo: bool = 0 & 0x40000000000000, BSigned: bool = 0 & 0x20000000000000, AvgMode: AvgMode = 51 & 0x3, Sat: bool = 0 & 0x4000000000000, X: bool = 0 & 0x2000000000000, ASigned: bool = 0 & 0x1000000000000 }
    ImadspC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, ASelect: ImadspASelect = 48 & 0x7, BSelect: ImadspBSelect = 53 & 0x3, CSelect: ImadspASelect = 50 & 0x6 | 48 & 0x1 }
    ImadspI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, ASelect: ImadspASelect = 48 & 0x7, BSelect: ImadspBSelect = 53 & 0x3, CSelect: ImadspASelect = 50 & 0x6 | 48 & 0x1 }
    ImadspR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, ASelect: ImadspASelect = 48 & 0x7, BSelect: ImadspBSelect = 53 & 0x3, CSelect: ImadspASelect = 50 & 0x6 | 48 & 0x1 }
    ImadspRc { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcC: int = 39 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, ASelect: ImadspASelect = 48 & 0x7, BSelect: ImadspBSelect = 53 & 0x3, CSelect: ImadspASelect = 50 & 0x6 | 48 & 0x1 }
    ImnmxC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Signed: bool = 0 & 0x1000000000000, XMode: XMode = 43 & 0x3, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000 }
    ImnmxI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Signed: bool = 0 & 0x1000000000000, XMode: XMode = 43 & 0x3, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000 }
    ImnmxR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Signed: bool = 0 & 0x1000000000000, XMode: XMode = 43 & 0x3, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000 }
    Imul32i { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Imm32: int = 20 & 0xffffffff, ASigned: bool = 0 & 0x40000000000000, BSigned: bool = 0 & 0x80000000000000, Hilo: bool = 0 & 0x20000000000000, WriteCC: bool = 0 & 0x10000000000000 }
    ImulC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, ASigned: bool = 0 & 0x10000000000, BSigned: bool = 0 & 0x20000000000, Hilo: bool = 0 & 0x8000000000 }
    ImulI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, ASigned: bool = 0 & 0x10000000000, BSigned: bool = 0 & 0x20000000000, Hilo: bool = 0 & 0x8000000000 }
    ImulR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, ASigned: bool = 0 & 0x10000000000, BSigned: bool = 0 & 0x20000000000, Hilo: bool = 0 & 0x8000000000 }
    Ipa { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, IpaOp: IpaOp = 54 & 0x3, Msi: int = 52 & 0x3, Sat: bool = 0 & 0x8000000000000, Idx: bool = 0 & 0x4000000000, Imm10: int = 28 & 0x3ff, SrcPred: int = 47 & 0x7, SrcPredInv: bool = 0 & 0x4000000000000 }
    Isberd { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, AlSize: AlSize = 47 & 0x3, IBase: IBase = 33 & 0x3, O: bool = 0 & 0x100000000, P: bool = 0 & 0x80000000 }
    Iscadd32i { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Imm32: int = 20 & 0xffffffff, WriteCC: bool = 0 & 0x10000000000000, Imm5: int = 53 & 0x1f }
    IscaddC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Imm5: int = 39 & 0x1f, AvgMode: AvgMode = 48 & 0x3 }
    IscaddI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Imm5: int = 39 & 0x1f, AvgMode: AvgMode = 48 & 0x3 }
    IscaddR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Imm5: int = 39 & 0x1f, AvgMode: AvgMode = 48 & 0x3 }
    IsetC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, IComp: IComp = 49 & 0x7, Signed: bool = 0 & 0x1000000000000, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000, BVal: bool = 0 & 0x100000000000, Bop: BoolOp = 45 & 0x3, X: bool = 0 & 0x80000000000 }
    IsetI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, IComp: IComp = 49 & 0x7, Signed: bool = 0 & 0x1000000000000, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000, BVal: bool = 0 & 0x100000000000, Bop: BoolOp = 45 & 0x3, X: bool = 0 & 0x80000000000 }
    IsetR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, IComp: IComp = 49 & 0x7, Signed: bool = 0 & 0x1000000000000, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000, BVal: bool = 0 & 0x100000000000, Bop: BoolOp = 45 & 0x3, X: bool = 0 & 0x80000000000 }
    IsetpC { SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, IComp: IComp = 49 & 0x7, Signed: bool = 0 & 0x1000000000000, Bop: BoolOp = 45 & 0x3, X: bool = 0 & 0x80000000000, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000, DestPred: int = 3 & 0x7, DestPredInv: int = 0 & 0x7 }
    IsetpI { SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, IComp: IComp = 49 & 0x7, Signed: bool = 0 & 0x1000000000000, Bop: BoolOp = 45 & 0x3, X: bool = 0 & 0x80000000000, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000, DestPred: int = 3 & 0x7, DestPredInv: int = 0 & 0x7 }
    IsetpR { SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, IComp: IComp = 49 & 0x7, Signed: bool = 0 & 0x1000000000000, Bop: BoolOp = 45 & 0x3, X: bool = 0 & 0x80000000000, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000, DestPred: int = 3 & 0x7, DestPredInv: int = 0 & 0x7 }
    Jcal { Imm32: int = 20 & 0xffffffff, Ca: bool = 0 & 0x20, Inc: bool = 0 & 0x40 }
    Jmp { SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Ccc: Ccc = 0 & 0x1f, Ca: bool = 0 & 0x20, Imm32: int = 20 & 0xffffffff, Lmt: bool = 0 & 0x40, U: bool = 0 & 0x80 }
    Jmx { SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Ccc: Ccc = 0 & 0x1f, Ca: bool = 0 & 0x20, Imm32: int = 20 & 0xffffffff, Lmt: bool = 0 & 0x40 }
    Kil { Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Ccc: Ccc = 0 & 0x1f }
    Ld { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, SrcPred: int = 58 & 0x7, CacheOp: CacheOpLd = 56 & 0x3, LsSize: LsSize = 53 & 0x7, E: bool = 0 & 0x10000000000000, Imm32: int = 20 & 0xffffffff }
    Ldc { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, LsSize: LsSize2 = 48 & 0x7, AddressMode: AddressMode = 44 & 0x3, CbufSlot: int = 36 & 0x1f, CbufOffset: int = 20 & 0xffff }
    Ldg { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, LsSize: LsSize = 48 & 0x7, CacheOp: CacheOpLd = 46 & 0x3, E: bool = 0 & 0x200000000000, Imm24: int = 20 & 0xffffff }
    Ldl { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, LsSize: LsSize2 = 48 & 0x7, CacheOp: CacheOp2 = 44 & 0x3, Imm24: int = 20 & 0xffffff }
    Lds { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, LsSize: LsSize2 = 48 & 0x7, U: bool = 0 & 0x100000000000, Imm24: int = 20 & 0xffffff }
    LeaC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, X: bool = 0 & 0x400000000000, NegA: bool = 0 & 0x200000000000, ImmU5: int = 39 & 0x1f, DestPred: int = 48 & 0x7 }
    LeaHiC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, X: bool = 0 & 0x200000000000000, NegA: bool = 0 & 0x100000000000000, ImmU5: int = 51 & 0x1f, DestPred: int = 48 & 0x7 }
    LeaHiR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, X: bool = 0 & 0x4000000000, NegA: bool = 0 & 0x2000000000, ImmU5: int = 28 & 0x1f, DestPred: int = 48 & 0x7 }
    LeaI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, X: bool = 0 & 0x400000000000, NegA: bool = 0 & 0x200000000000, ImmU5: int = 39 & 0x1f, DestPred: int = 48 & 0x7 }
    LeaR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, X: bool = 0 & 0x400000000000, NegA: bool = 0 & 0x200000000000, ImmU5: int = 39 & 0x1f, DestPred: int = 48 & 0x7 }
    Lepc {  }
    Longjmp { Ccc: Ccc = 0 & 0x1f }
    Lop32i { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x10000000000000, Imm32: int = 20 & 0xffffffff, X: bool = 0 & 0x200000000000000, LogicOp: LogicOp = 53 & 0x3, NegA: bool = 0 & 0x80000000000000, NegB: bool = 0 & 0x100000000000000 }
    Lop3C { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, X: bool = 0 & 0x100000000000000, Imm: int = 48 & 0xff }
    Lop3I { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, X: bool = 0 & 0x200000000000000, Imm: int = 48 & 0xff }
    Lop3R { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, DestPred: int = 48 & 0x7, PredicateOp: PredicateOp = 36 & 0x3, X: bool = 0 & 0x4000000000, Imm: int = 28 & 0xff }
    LopC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, DestPred: int = 48 & 0x7, PredicateOp: PredicateOp = 44 & 0x3, X: bool = 0 & 0x80000000000, LogicOp: LogicOp = 41 & 0x3, NegA: bool = 0 & 0x8000000000, NegB: bool = 0 & 0x10000000000 }
    LopI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, DestPred: int = 48 & 0x7, PredicateOp: PredicateOp = 44 & 0x3, X: bool = 0 & 0x80000000000, LogicOp: LogicOp = 41 & 0x3, NegA: bool = 0 & 0x8000000000, NegB: bool = 0 & 0x10000000000 }
    LopR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, DestPred: int = 48 & 0x7, PredicateOp: PredicateOp = 44 & 0x3, X: bool = 0 & 0x80000000000, Lop: LogicOp = 41 & 0x3, NegA: bool = 0 & 0x8000000000, NegB: bool = 0 & 0x10000000000 }
    Membar { Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Membar: Membar = 8 & 0x3, Ivall: Ivall = 0 & 0x3 }
    Mov32i { Dest: int = 0 & 0xff, Imm32: int = 20 & 0xffffffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, QuadMask: int = 12 & 0xf }
    MovC { Dest: int = 0 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, QuadMask: int = 39 & 0xf }
    MovI { Dest: int = 0 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, QuadMask: int = 39 & 0xf }
    MovR { Dest: int = 0 & 0xff, SrcA: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, QuadMask: int = 39 & 0xf }
    Mufu { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, MufuOp: MufuOp = 20 & 0xf, AbsA: bool = 0 & 0x400000000000, NegA: bool = 0 & 0x1000000000000, Sat: bool = 0 & 0x4000000000000 }
    Nop { Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Imm16: int = 20 & 0xffff, Trig: bool = 0 & 0x2000, Ccc: Ccc = 8 & 0x1f }
    OutC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, OutType: OutType = 39 & 0x3 }
    OutI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, OutType: OutType = 39 & 0x3 }
    OutR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, OutType: OutType = 39 & 0x3 }
    P2rC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, ByteSel: ByteSel = 41 & 0x3, Ccpr: bool = 0 & 0x10000000000 }
    P2rI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, ByteSel: ByteSel = 41 & 0x3, Ccpr: bool = 0 & 0x10000000000 }
    P2rR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, ByteSel: ByteSel = 41 & 0x3, Ccpr: bool = 0 & 0x10000000000 }
    Pbk { Imm24: int = 20 & 0xffffff, Ca: bool = 0 & 0x20 }
    Pcnt { Imm24: int = 20 & 0xffffff, Ca: bool = 0 & 0x20 }
    Pexit { Imm24: int = 20 & 0xffffff }
    Pixld { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, DestPred: int = 45 & 0x7, PixMode: PixMode = 31 & 0x7, Imm8: int = 20 & 0xff }
    Plongjmp { Imm24: int = 20 & 0xffffff, Ca: bool = 0 & 0x20 }
    PopcC { Dest: int = 0 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, NegB: bool = 0 & 0x10000000000 }
    PopcI { Dest: int = 0 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, NegB: bool = 0 & 0x10000000000 }
    PopcR { Dest: int = 0 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, NegB: bool = 0 & 0x10000000000 }
    Pret { Ca: bool = 0 & 0x20, Imm24: int = 20 & 0xffffff, Inc: bool = 0 & 0x40 }
    PrmtC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, PMode: PMode = 48 & 0xf }
    PrmtI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, PMode: PMode = 48 & 0xf }
    PrmtR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, PMode: PMode = 48 & 0xf }
    PrmtRc { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcC: int = 39 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, PMode: PMode = 48 & 0xf }
    Pset { Dest: int = 0 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Src2Pred: int = 12 & 0x7, Src2PredInv: bool = 0 & 0x8000, Src1Pred: int = 29 & 0x7, Src1PredInv: bool = 0 & 0x100000000, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000, BoolOpAB: BoolOp = 24 & 0x3, BoolOpC: BoolOp = 45 & 0x3, BVal: bool = 0 & 0x100000000000 }
    Psetp { DestPred: int = 3 & 0x7, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, DestPredInv: int = 0 & 0x7, Src2Pred: int = 12 & 0x7, Src2PredInv: bool = 0 & 0x8000, Src1Pred: int = 29 & 0x7, Src1PredInv: bool = 0 & 0x100000000, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000, BoolOpAB: BoolOp = 24 & 0x3, BoolOpC: BoolOp = 45 & 0x3 }
    R2b { SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Mode: BarMode = 32 & 0x3, Name: int = 28 & 0xf }
    R2pC { SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, ByteSel: ByteSel = 41 & 0x3, Ccpr: bool = 0 & 0x10000000000 }
    R2pI { SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, ByteSel: ByteSel = 41 & 0x3, Ccpr: bool = 0 & 0x10000000000 }
    R2pR { SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, ByteSel: ByteSel = 41 & 0x3, Ccpr: bool = 0 & 0x10000000000 }
    Ram {  }
    Red { SrcA: int = 8 & 0xff, SrcB: int = 0 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Imm20: int = 28 & 0xfffff, RedSize: AtomSize = 20 & 0x7, RedOp: RedOp = 23 & 0x7, E: bool = 0 & 0x1000000000000 }
    Ret { Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Ccc: Ccc = 0 & 0x1f }
    RroC { Dest: int = 0 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, AbsB: bool = 0 & 0x2000000000000, NegB: bool = 0 & 0x200000000000, RroOp: bool = 0 & 0x8000000000 }
    RroI { Dest: int = 0 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, AbsB: bool = 0 & 0x2000000000000, NegB: bool = 0 & 0x200000000000, RroOp: bool = 0 & 0x8000000000 }
    RroR { Dest: int = 0 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, AbsB: bool = 0 & 0x2000000000000, NegB: bool = 0 & 0x200000000000, RroOp: bool = 0 & 0x8000000000 }
    Rtt {  }
    S2r { Dest: int = 0 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, SReg: SReg = 20 & 0xff }
    Sam {  }
    SelC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000 }
    SelI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000 }
    SelR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000 }
    Setcrsptr { SrcA: int = 8 & 0xff }
    Setlmembase { SrcA: int = 8 & 0xff }
    ShfLI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, M: bool = 0 & 0x4000000000000, XModeShf: XModeShf = 48 & 0x3, MaxShift: MaxShift = 37 & 0x3, Imm6: int = 20 & 0x3f }
    ShfLR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, M: bool = 0 & 0x4000000000000, XModeShf: XModeShf = 48 & 0x3, MaxShift: MaxShift = 37 & 0x3 }
    ShfRI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, M: bool = 0 & 0x4000000000000, XModeShf: XModeShf = 48 & 0x3, MaxShift: MaxShift = 37 & 0x3, Imm6: int = 20 & 0x3f }
    ShfRR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, M: bool = 0 & 0x4000000000000, XModeShf: XModeShf = 48 & 0x3, MaxShift: MaxShift = 37 & 0x3 }
    Shfl { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, SrcBImm: int = 20 & 0x1f, SrcCImm: int = 34 & 0x1fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, ShflMode: ShflMode = 30 & 0x3, CFixShfl: bool = 0 & 0x20000000, BFixShfl: bool = 0 & 0x10000000, DestPred: int = 48 & 0x7 }
    ShlC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, X: bool = 0 & 0x80000000000, M: bool = 0 & 0x8000000000 }
    ShlI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, X: bool = 0 & 0x80000000000, M: bool = 0 & 0x8000000000 }
    ShlR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, X: bool = 0 & 0x80000000000, M: bool = 0 & 0x8000000000 }
    ShrC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Signed: bool = 0 & 0x1000000000000, XMode: XMode = 43 & 0x3, Brev: bool = 0 & 0x10000000000, M: bool = 0 & 0x8000000000 }
    ShrI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Imm20: int = 37 & 0x80000 | 20 & 0x7ffff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Signed: bool = 0 & 0x1000000000000, XMode: XMode = 43 & 0x3, Brev: bool = 0 & 0x10000000000, M: bool = 0 & 0x8000000000 }
    ShrR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Signed: bool = 0 & 0x1000000000000, XMode: XMode = 43 & 0x3, Brev: bool = 0 & 0x10000000000, M: bool = 0 & 0x8000000000 }
    Ssy { Imm24: int = 20 & 0xffffff, Ca: bool = 0 & 0x20 }
    St { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, SrcPred: int = 58 & 0x7, CacheOp: CacheOpSt = 56 & 0x3, LsSize: LsSize = 53 & 0x7, E: bool = 0 & 0x10000000000000, Imm32: int = 20 & 0xffffffff }
    Stg { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, LsSize: LsSize2 = 48 & 0x7, CacheOp: CacheOpSt = 46 & 0x3, E: bool = 0 & 0x200000000000, Imm24: int = 20 & 0xffffff }
    Stl { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, LsSize: LsSize2 = 48 & 0x7, CacheOp: CacheOpSt = 44 & 0x3, Imm24: int = 20 & 0xffffff }
    Stp { Wait: bool = 0 & 0x80000000, Imm8: int = 20 & 0xff }
    Sts { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, LsSize: LsSize2 = 48 & 0x7, Imm24: int = 20 & 0xffffff }
    Suatom { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Size: SuatomSize = 51 & 0x7, Clamp: Clamp = 49 & 0x3, TidB: int = 36 & 0x1fff, Dim: SuDim = 33 & 0x7, Op: SuatomOp = 29 & 0xf, Ba: bool = 0 & 0x10000000 }
    SuatomB { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Clamp: Clamp = 49 & 0x3, Size: SuatomSize = 36 & 0x7, Dim: SuDim = 33 & 0x7, Op: SuatomOp = 29 & 0xf, Ba: bool = 0 & 0x10000000 }
    SuatomB2 { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, DestPred: int = 51 & 0x7, Clamp: Clamp = 49 & 0x3, Size: SuatomSize = 36 & 0x7, Dim: SuDim = 33 & 0x7, Op: SuatomOp = 29 & 0xf, Ba: bool = 0 & 0x10000000 }
    SuatomCas { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Size: SuatomSize = 51 & 0x7, Clamp: Clamp = 49 & 0x3, TidB: int = 36 & 0x1fff, Dim: SuDim = 33 & 0x7, DestPred: int = 30 & 0x7, Ba: bool = 0 & 0x10000000 }
    SuatomCasB { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Clamp: Clamp = 49 & 0x3, Size: SuatomSize = 36 & 0x7, Dim: SuDim = 33 & 0x7, DestPred: int = 30 & 0x7, Ba: bool = 0 & 0x10000000 }
    Suld { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Clamp: Clamp = 49 & 0x3, TidB: int = 36 & 0x1fff, Dim: SuDim = 33 & 0x7, DestPred2: int = 30 & 0x7, CacheOp: CacheOpLd = 24 & 0x3, Rgba: SuRgba = 20 & 0xf }
    SuldB { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Clamp: Clamp = 49 & 0x3, Dim: SuDim = 33 & 0x7, DestPred2: int = 30 & 0x7, CacheOp: CacheOpLd = 24 & 0x3, Rgba: SuRgba = 20 & 0xf }
    SuldD { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Clamp: Clamp = 49 & 0x3, TidB: int = 36 & 0x1fff, Dim: SuDim = 33 & 0x7, DestPred2: int = 30 & 0x7, CacheOp: CacheOpLd = 24 & 0x3, Ba: bool = 0 & 0x800000, Size: SuSize = 20 & 0x7 }
    SuldDB { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Clamp: Clamp = 49 & 0x3, Dim: SuDim = 33 & 0x7, DestPred2: int = 30 & 0x7, CacheOp: CacheOpLd = 24 & 0x3, Ba: bool = 0 & 0x800000, Size: SuSize = 20 & 0x7 }
    Sured { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Clamp: Clamp = 49 & 0x3, TidB: int = 36 & 0x1fff, Dim: SuDim = 33 & 0x7, Op: RedOp = 24 & 0x7, Ba: bool = 0 & 0x800000, Size: SuatomSize = 20 & 0x7 }
    SuredB { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Clamp: Clamp = 49 & 0x3, Dim: SuDim = 33 & 0x7, Op: RedOp = 24 & 0x7, Ba: bool = 0 & 0x800000, Size: SuatomSize = 20 & 0x7 }
    Sust { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Clamp: Clamp = 49 & 0x3, TidB: int = 36 & 0x1fff, Dim: SuDim = 33 & 0x7, CacheOp: CacheOpSt = 24 & 0x3, Rgba: SuRgba = 20 & 0xf }
    SustB { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Clamp: Clamp = 49 & 0x3, Dim: SuDim = 33 & 0x7, CacheOp: CacheOpSt = 24 & 0x3, Rgba: SuRgba = 20 & 0xf }
    SustD { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Clamp: Clamp = 49 & 0x3, TidB: int = 36 & 0x1fff, Dim: SuDim = 33 & 0x7, CacheOp: CacheOpSt = 24 & 0x3, Ba: bool = 0 & 0x800000, Size: SuSize = 20 & 0x7 }
    SustDB { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Clamp: Clamp = 49 & 0x3, Dim: SuDim = 33 & 0x7, CacheOp: CacheOpSt = 24 & 0x3, Ba: bool = 0 & 0x800000, Size: SuSize = 20 & 0x7 }
    Sync { Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Ccc: Ccc = 0 & 0x1f }
    Tex { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Lc: bool = 0 & 0x400000000000000, DestPred: int = 51 & 0x7, TidB: int = 36 & 0x1fff, Lod: Lod = 55 & 0x7, Aoffi: bool = 0 & 0x40000000000000, Dc: bool = 0 & 0x4000000000000, Ndv: bool = 0 & 0x800000000, Dim: TexDim = 28 & 0x7, WMask: int = 31 & 0xf, Nodep: bool = 0 & 0x2000000000000 }
    TexB { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Lcb: bool = 0 & 0x10000000000, DestPred: int = 51 & 0x7, Lodb: Lod = 37 & 0x7, Aoffib: bool = 0 & 0x1000000000, Dc: bool = 0 & 0x4000000000000, Ndv: bool = 0 & 0x800000000, Dim: TexDim = 28 & 0x7, WMask: int = 31 & 0xf, Nodep: bool = 0 & 0x2000000000000 }
    Texs { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, TidB: int = 36 & 0x1fff, Target: TexsTarget = 53 & 0xf, WMask: int = 50 & 0x7, Nodep: bool = 0 & 0x2000000000000, Dest2: int = 28 & 0xff }
    Tld { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, TidB: int = 36 & 0x1fff, WMask: int = 31 & 0xf, Lod: bool = 0 & 0x80000000000000, Toff: bool = 0 & 0x800000000, Ms: bool = 0 & 0x4000000000000, Cl: bool = 0 & 0x40000000000000, Nodep: bool = 0 & 0x2000000000000, DestPred: int = 51 & 0x7, Dim: TexDim = 28 & 0x7 }
    Tld4 { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Lc: bool = 0 & 0x400000000000000, DestPred: int = 51 & 0x7, TidB: int = 36 & 0x1fff, TexComp: TexComp = 56 & 0x3, Toff: TexOffset = 54 & 0x3, Dc: bool = 0 & 0x4000000000000, Ndv: bool = 0 & 0x800000000, Dim: TexDim = 28 & 0x7, WMask: int = 31 & 0xf, Nodep: bool = 0 & 0x2000000000000 }
    Tld4B { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Lc: bool = 0 & 0x10000000000, DestPred: int = 51 & 0x7, TexComp: TexComp = 38 & 0x3, Toff: TexOffset = 36 & 0x3, Dc: bool = 0 & 0x4000000000000, Ndv: bool = 0 & 0x800000000, Dim: TexDim = 28 & 0x7, WMask: int = 31 & 0xf, Nodep: bool = 0 & 0x2000000000000 }
    Tld4s { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, TidB: int = 36 & 0x1fff, TexComp: TexComp = 52 & 0x3, Aoffi: bool = 0 & 0x8000000000000, Dc: bool = 0 & 0x4000000000000, Nodep: bool = 0 & 0x2000000000000, Dest2: int = 28 & 0xff }
    TldB { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WMask: int = 31 & 0xf, Lod: bool = 0 & 0x80000000000000, Toff: bool = 0 & 0x800000000, Ms: bool = 0 & 0x4000000000000, Cl: bool = 0 & 0x40000000000000, Nodep: bool = 0 & 0x2000000000000, DestPred: int = 51 & 0x7, Dim: TexDim = 28 & 0x7 }
    Tlds { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, TidB: int = 36 & 0x1fff, Target: TldsTarget = 53 & 0xf, WMask: int = 50 & 0x7, Nodep: bool = 0 & 0x2000000000000, Dest2: int = 28 & 0xff }
    Tmml { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Nodep: bool = 0 & 0x2000000000000, TidB: int = 36 & 0x1fff, Ndv: bool = 0 & 0x800000000, WMask: int = 31 & 0xf, Dim: TexDim = 28 & 0x7 }
    TmmlB { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Nodep: bool = 0 & 0x2000000000000, Ndv: bool = 0 & 0x800000000, WMask: int = 31 & 0xf, Dim: TexDim = 28 & 0x7 }
    Txa { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Nodep: bool = 0 & 0x2000000000000, TidB: int = 36 & 0x1fff, Ndv: bool = 0 & 0x800000000, WMask: int = 31 & 0xf }
    Txd { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, DestPred: int = 51 & 0x7, Lc: bool = 0 & 0x4000000000000, Nodep: bool = 0 & 0x2000000000000, TidB: int = 36 & 0x1fff, Toff: bool = 0 & 0x800000000, WMask: int = 31 & 0xf, Dim: TexDim = 28 & 0x7 }
    TxdB { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, DestPred: int = 51 & 0x7, Lc: bool = 0 & 0x4000000000000, Nodep: bool = 0 & 0x2000000000000, Toff: bool = 0 & 0x800000000, WMask: int = 31 & 0xf, Dim: TexDim = 28 & 0x7 }
    Txq { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Nodep: bool = 0 & 0x2000000000000, TidB: int = 36 & 0x1fff, WMask: int = 31 & 0xf, TexQuery: TexQuery = 22 & 0x3f }
    TxqB { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Nodep: bool = 0 & 0x2000000000000, WMask: int = 31 & 0xf, TexQuery: TexQuery = 22 & 0x3f }
    Vabsdiff { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, DFormat: bool = 0 & 0x40000000000000, ASelect: VectorSelect = 45 & 0x8 | 36 & 0x7, BSelect: VectorSelect = 46 & 0x8 | 28 & 0x7, Sat: bool = 0 & 0x80000000000000, VideoOp: VideoOp = 51 & 0x7, BVideo: bool = 0 & 0x4000000000000 }
    Vabsdiff4 { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, VRed: VideoRed = 53 & 0x3, LaneMask4: LaneMask4 = 49 & 0xc | 36 & 0x3, Sat: bool = 0 & 0x4000000000000, SrcBFmt: bool = 0 & 0x2000000000000, SrcAFmt: bool = 0 & 0x1000000000000, DFormat: bool = 0 & 0x4000000000, Asel4: ASelect4 = 32 & 0xf, Bsel4: BSelect4 = 28 & 0xf }
    Vadd { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Imm16: int = 20 & 0xffff, WriteCC: bool = 0 & 0x800000000000, AvgMode: AvgMode = 56 & 0x3, DFormat: bool = 0 & 0x40000000000000, ASelect: VectorSelect = 45 & 0x8 | 36 & 0x7, BSelect: VectorSelect = 46 & 0x8 | 28 & 0x7, Sat: bool = 0 & 0x80000000000000, VideoOp: VideoOp = 51 & 0x7, BVideo: bool = 0 & 0x4000000000000 }
    Vmad { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Imm16: int = 20 & 0xffff, WriteCC: bool = 0 & 0x800000000000, ASelect: VectorSelect = 45 & 0x8 | 36 & 0x7, BSelect: VectorSelect = 46 & 0x8 | 28 & 0x7, Sat: bool = 0 & 0x80000000000000, AvgMode: AvgMode = 53 & 0x3, VideoScale: VideoScale = 51 & 0x3, BVideo: bool = 0 & 0x4000000000000 }
    Vmnmx { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Imm16: int = 20 & 0xffff, WriteCC: bool = 0 & 0x800000000000, DFormat: bool = 0 & 0x40000000000000, ASelect: VectorSelect = 45 & 0x8 | 36 & 0x7, BSelect: VectorSelect = 46 & 0x8 | 28 & 0x7, Sat: bool = 0 & 0x80000000000000, VideoOp: VideoOp = 51 & 0x7, Mn: bool = 0 & 0x100000000000000, BVideo: bool = 0 & 0x4000000000000 }
    Vote { Dest: int = 0 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000, VoteMode: VoteMode = 48 & 0x3, VpDest: int = 45 & 0x7 }
    Votevtg { Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, VoteMode: VoteMode = 48 & 0x3, Imm28: int = 20 & 0xfffffff }
    Vset { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, VComp: IComp = 54 & 0x7, ASelect: VectorSelect = 45 & 0x8 | 36 & 0x7, BSelect: VectorSelect = 46 & 0x8 | 28 & 0x7, VideoOp: VideoOp = 51 & 0x7, BVideo: bool = 0 & 0x4000000000000 }
    Vsetp { SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, Imm16: int = 20 & 0xffff, ASelect: VectorSelect = 45 & 0x8 | 36 & 0x7, BSelect: VectorSelect = 46 & 0x8 | 28 & 0x7, VComp: IComp = 45 & 0x4 | 43 & 0x3, BoolOp: BoolOp = 45 & 0x3, SrcPred: int = 39 & 0x7, SrcPredInv: bool = 0 & 0x40000000000, DestPred: int = 3 & 0x7, DestPredInv: int = 0 & 0x7, BVideo: bool = 0 & 0x4000000000000 }
    Vshl { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Mv: bool = 0 & 0x2000000000000, DFormat: bool = 0 & 0x40000000000000, ASelect: VectorSelect = 45 & 0x8 | 36 & 0x7, BSelect: VectorSelect = 28 & 0x7, Sat: bool = 0 & 0x80000000000000, VideoOp: VideoOp = 51 & 0x7, BVideo: bool = 0 & 0x4000000000000 }
    Vshr { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Mv: bool = 0 & 0x2000000000000, DFormat: bool = 0 & 0x40000000000000, ASelect: VectorSelect = 45 & 0x8 | 36 & 0x7, BSelect: VectorSelect = 28 & 0x7, Sat: bool = 0 & 0x80000000000000, VideoOp: VideoOp = 51 & 0x7, BVideo: bool = 0 & 0x4000000000000 }
    XmadC { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Mrg: bool = 0 & 0x100000000000000, Psl: bool = 0 & 0x80000000000000, X: bool = 0 & 0x40000000000000, HiloA: bool = 0 & 0x20000000000000, HiloB: bool = 0 & 0x10000000000000, XmadCop: XmadCop2 = 50 & 0x3, BSigned: bool = 0 & 0x2000000000000, ASigned: bool = 0 & 0x1000000000000 }
    XmadI { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, Imm16: int = 20 & 0xffff, HiloA: bool = 0 & 0x20000000000000, XmadCop: XmadCop = 50 & 0x7, BSigned: bool = 0 & 0x2000000000000, ASigned: bool = 0 & 0x1000000000000, X: bool = 0 & 0x4000000000, Mrg: bool = 0 & 0x2000000000, Psl: bool = 0 & 0x1000000000 }
    XmadR { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcB: int = 20 & 0xff, SrcC: int = 39 & 0xff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, HiloA: bool = 0 & 0x20000000000000, XmadCop: XmadCop = 50 & 0x7, BSigned: bool = 0 & 0x2000000000000, ASigned: bool = 0 & 0x1000000000000, X: bool = 0 & 0x4000000000, Mrg: bool = 0 & 0x2000000000, Psl: bool = 0 & 0x1000000000, HiloB: bool = 0 & 0x800000000 }
    XmadRc { Dest: int = 0 & 0xff, SrcA: int = 8 & 0xff, SrcC: int = 39 & 0xff, CbufSlot: int = 34 & 0x1f, CbufOffset: int = 20 & 0x3fff, Pred: int = 16 & 0x7, PredInv: bool = 0 & 0x80000, WriteCC: bool = 0 & 0x800000000000, X: bool = 0 & 0x40000000000000, HiloA: bool = 0 & 0x20000000000000, HiloB: bool = 0 & 0x10000000000000, XmadCop: XmadCop2 = 50 & 0x3, BSigned: bool = 0 & 0x2000000000000, ASigned: bool = 0 & 0x1000000000000 }
}
