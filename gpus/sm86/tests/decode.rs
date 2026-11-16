use relift_gpu_sm86::{Word, decode};

type Case<'a> = (u32, &'a [(u32, u32, u64)], &'a str);

#[derive(Clone, Copy)]
struct B(u128);

impl B {
    fn op(op: u32, form: u32) -> B {
        B(
            ((op as u128) | ((form as u128) << 9))
                | (7u128 << 12)
                | (7u128 << 110)
                | (7u128 << 113),
        )
    }
    fn f(mut self, lo: u32, len: u32, v: u64) -> B {
        let m = ((1u128 << len) - 1) << lo;
        self.0 = (self.0 & !m) | (((v as u128) << lo) & m);
        self
    }
    fn text(self, pc: u64) -> String {
        decode(Word(self.0), pc)
            .map(|i| i.to_string())
            .unwrap_or_else(|e| e.to_string())
    }
}

fn cb(bank: u64, off: u64) -> u64 {
    (bank << 22) | (off << 6)
}

#[test]
fn float_alu() {
    let t = B::op(0x023, 5)
        .f(16, 8, 5)
        .f(24, 8, 2)
        .f(32, 27, cb(3, 0))
        .f(64, 8, 5)
        .f(80, 1, 1)
        .f(122, 1, 1);
    assert_eq!(t.text(0), "FFMA.FTZ R5, R2.reuse, c[0x3][0x0], R5");
    let t = B::op(0x020, 4)
        .f(16, 8, 4)
        .f(24, 8, 4)
        .f(32, 32, 0x3f00_0000)
        .f(84, 3, 4)
        .f(77, 1, 1);
    assert_eq!(t.text(0), "FMUL.SAT R4, R4, 0.5");
    let t = B::op(0x020, 1)
        .f(16, 8, 1)
        .f(24, 8, 2)
        .f(32, 8, 3)
        .f(84, 3, 3)
        .f(78, 2, 3)
        .f(80, 1, 1);
    assert_eq!(t.text(0), "FMUL.FTZ.D2.RZ R1, R2, R3");
    let t = B::op(0x021, 2)
        .f(16, 8, 0)
        .f(24, 8, 2)
        .f(64, 8, 255)
        .f(32, 32, 0x3f80_0000)
        .f(72, 2, 3);
    assert_eq!(t.text(0), "FADD R0, -|R2|, 1");
    let t = B::op(0x021, 1)
        .f(16, 8, 0)
        .f(24, 8, 1)
        .f(32, 8, 26)
        .f(124, 1, 1);
    assert_eq!(t.text(0), "FADD R0, R1, R26.reuse");
    let t = B::op(0x108, 1).f(16, 8, 7).f(32, 8, 21).f(74, 6, 8);
    assert_eq!(t.text(0), "MUFU.SQRT R7, R21");
}

#[test]
fn float_immediates() {
    let t = B::op(0x021, 2)
        .f(16, 8, 3)
        .f(24, 8, 54)
        .f(64, 8, 255)
        .f(32, 32, 0x3727_c5ac);
    assert_eq!(t.text(0), "FADD R3, R54, 9.9999997473787516356e-06");
    let t = B::op(0x020, 4)
        .f(16, 8, 0)
        .f(24, 8, 9)
        .f(32, 32, 0x4e6e_6b28)
        .f(84, 3, 4);
    assert_eq!(t.text(0), "FMUL R0, R9, 1.00000000000000000000e+09");
    let t = B::op(0x020, 4)
        .f(16, 8, 0)
        .f(24, 8, 9)
        .f(32, 32, 0x4b7f_ffff)
        .f(84, 3, 4);
    assert_eq!(t.text(0), "FMUL R0, R9, 16777215");
}

#[test]
fn compare_and_int() {
    let t = B::op(0x00b, 5)
        .f(81, 3, 1)
        .f(84, 3, 7)
        .f(24, 8, 0)
        .f(72, 1, 1)
        .f(32, 27, cb(3, 0x280))
        .f(76, 4, 4)
        .f(80, 1, 1)
        .f(87, 3, 7);
    assert_eq!(t.text(0), "FSETP.GT.FTZ.AND P1, PT, -R0, c[0x3][0x280], PT");
    let t = B::op(0x00c, 4)
        .f(81, 3, 0)
        .f(84, 3, 7)
        .f(24, 8, 1)
        .f(32, 32, 0xffff_ffff)
        .f(76, 3, 5)
        .f(73, 1, 0)
        .f(87, 3, 7);
    assert_eq!(t.text(0), "ISETP.NE.U32.AND P0, PT, R1, -0x1, PT");
    let t = B::op(0x010, 4)
        .f(16, 8, 1)
        .f(24, 8, 255)
        .f(32, 32, 1)
        .f(64, 8, 255)
        .f(81, 3, 7)
        .f(84, 3, 7)
        .f(12, 4, 9);
    assert_eq!(t.text(0), "@!P1 IADD3 R1, RZ, 0x1, RZ");
    let t = B::op(0x002, 5)
        .f(16, 8, 43)
        .f(32, 27, cb(9, 0xa594))
        .f(72, 4, 0xf);
    assert_eq!(t.text(0), "MOV R43, c[0x9][-0x5a6c]");
}

#[test]
fn control() {
    let t = B::op(0x147, 4).f(34, 48, 0x210 / 4).f(87, 3, 7);
    assert_eq!(t.text(0x270), "BRA 0x490");
    let t = B::op(0x147, 4)
        .f(34, 48, (-4i64 as u64) & 0xffff_ffff_ffff)
        .f(87, 3, 7);
    assert_eq!(t.text(0x30), "BRA 0x30");
    let t = B::op(0x14d, 4).f(87, 3, 7);
    assert_eq!(t.text(0), "EXIT");
}

#[test]
fn attributes() {
    let t = B::op(0x121, 1)
        .f(16, 8, 4)
        .f(24, 8, 255)
        .f(32, 8, 255)
        .f(40, 11, 0xf0)
        .f(74, 2, 2);
    assert_eq!(t.text(0), "ALD.96 R4, a[0xf0]");
    assert_eq!(t.f(32, 8, 2).text(0), "ALD.96 R4, a[0xf0], R2");
    let t = B::op(0x122, 1)
        .f(24, 8, 255)
        .f(32, 8, 3)
        .f(40, 11, 0xec)
        .f(64, 8, 255);
    assert_eq!(t.text(0), "AST a[0xec], R3");
    assert_eq!(t.f(64, 8, 0).text(0), "AST a[0xec], R3, R0");
}

#[test]
fn integer_conversion_alu() {
    let t = B::op(0x024, 1)
        .f(16, 8, 0x6)
        .f(24, 8, 0xff)
        .f(32, 8, 0xff)
        .f(64, 8, 0x3)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "IMAD.MOV.U32 R6, RZ, RZ, R3");
    let t = B::op(0x024, 3)
        .f(16, 8, 0x6)
        .f(24, 8, 0xff)
        .f(32, 27, 0xc04200)
        .f(64, 8, 0xff)
        .f(73, 1, 0x0)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "IMAD.MOV.U32 R6, RZ, RZ, c[0x3][0x108]");
    let t = B::op(0x024, 4)
        .f(16, 8, 0x6)
        .f(24, 8, 0x6)
        .f(32, 32, 0x10)
        .f(64, 8, 0xff)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "IMAD.SHL.U32 R6, R6, 0x10, RZ");
    let t = B::op(0x024, 4)
        .f(16, 8, 0x6)
        .f(24, 8, 0x6)
        .f(32, 32, 0x2)
        .f(64, 8, 0xff)
        .f(73, 1, 0x1)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "IMAD.SHL R6, R6, 0x2, RZ");
    let t = B::op(0x024, 4)
        .f(16, 8, 0x6)
        .f(24, 8, 0x6)
        .f(32, 32, 0x1)
        .f(64, 8, 0x7)
        .f(73, 1, 0x1)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "IMAD.IADD R6, R6, 0x1, R7");
    let t = B::op(0x024, 4)
        .f(16, 8, 0x6)
        .f(24, 8, 0x6)
        .f(32, 32, 0x10000)
        .f(64, 8, 0xff)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "IMAD.U32 R6, R6, 0x10000, RZ");
    let t = B::op(0x024, 1)
        .f(16, 8, 0x6)
        .f(24, 8, 0xff)
        .f(32, 8, 0xff)
        .f(64, 8, 0x3)
        .f(73, 1, 0x1)
        .f(75, 1, 0x1)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "IMAD.MOV R6, RZ, RZ, -R3");
    let t = B::op(0x024, 1)
        .f(16, 8, 0x7)
        .f(24, 8, 0xff)
        .f(32, 8, 0xff)
        .f(64, 8, 0x3)
        .f(73, 1, 0x1)
        .f(74, 1, 0x1)
        .f(87, 3, 0x1);
    assert_eq!(t.text(0), "IMAD.X R7, RZ, RZ, R3, P1");
    let t = B::op(0x025, 1)
        .f(16, 8, 0x4)
        .f(24, 8, 0x2)
        .f(32, 8, 0x3)
        .f(64, 8, 0x6)
        .f(73, 1, 0x1)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "IMAD.WIDE R4, P0, R2, R3, R6");
    let t = B::op(0x027, 4)
        .f(16, 8, 0x3)
        .f(24, 8, 0xe)
        .f(32, 32, 0xc0000001)
        .f(64, 8, 0xff)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "IMAD.HI.U32 R3, P0, R14, -0x3fffffff, RZ");
    let t = B::op(0x011, 4)
        .f(16, 8, 0x45)
        .f(24, 8, 0xc)
        .f(32, 32, 0x45f0)
        .f(64, 8, 0xff)
        .f(75, 5, 0x4)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "LEA R69, P0, R12, 0x45f0, 0x4");
    let t = B::op(0x011, 1)
        .f(16, 8, 0x2c)
        .f(24, 8, 0x2c)
        .f(32, 8, 0x35)
        .f(64, 8, 0xff)
        .f(75, 5, 0x11)
        .f(80, 1, 0x1)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "LEA.HI R44, P0, R44, R53, RZ, 0x11");
    let t = B::op(0x019, 4)
        .f(16, 8, 0x8)
        .f(24, 8, 0x2)
        .f(32, 32, 0x2)
        .f(64, 8, 0xff)
        .f(73, 2, 0x3)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "SHF.L.U32 R8, R2, 0x2, RZ");
    let t = B::op(0x019, 4)
        .f(16, 8, 0x2a)
        .f(24, 8, 0xff)
        .f(32, 32, 0x1)
        .f(64, 8, 0x2a)
        .f(73, 2, 0x3)
        .f(76, 1, 0x1)
        .f(80, 1, 0x1)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "SHF.R.U32.HI R42, RZ, 0x1, R42");
    let t = B::op(0x019, 2)
        .f(16, 8, 0x5)
        .f(24, 8, 0x3)
        .f(32, 32, 0x4)
        .f(64, 8, 0x2)
        .f(73, 2, 0x1)
        .f(80, 1, 0x1)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "SHF.L.U64.HI R5, R3, R2, 0x4");
    let t = B::op(0x016, 4)
        .f(16, 8, 0x14)
        .f(24, 8, 0x14)
        .f(32, 32, 0x7650)
        .f(64, 8, 0xff)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "PRMT R20, R20, 0x7650, RZ");
    let t = B::op(0x016, 4)
        .f(16, 8, 0x14)
        .f(24, 8, 0x14)
        .f(32, 32, 0x7650)
        .f(64, 8, 0xff)
        .f(72, 3, 0x3)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "PRMT.RC8 R20, R20, 0x7650, RZ");
    let t = B::op(0x017, 1)
        .f(16, 8, 0x16)
        .f(24, 8, 0x17)
        .f(32, 8, 0x16)
        .f(73, 1, 0x1)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "IMNMX R22, R23, R22, PT");
    let t = B::op(0x01a, 4)
        .f(16, 8, 0x0)
        .f(24, 8, 0x0)
        .f(32, 32, 0x3)
        .f(75, 1, 0x1)
        .f(73, 1, 0x0)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "SGXT.W.U32 R0, R0, 0x3");
    let t = B::op(0x01b, 4)
        .f(16, 8, 0x5)
        .f(24, 8, 0x6)
        .f(32, 32, 0x7)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "BMSK R5, R6, 0x7");
    let t = B::op(0x01c, 4)
        .f(81, 3, 0x0)
        .f(84, 3, 0x7)
        .f(87, 3, 0x7)
        .f(77, 3, 0x7)
        .f(68, 3, 0x7)
        .f(72, 5, 0x10)
        .f(64, 3, 0x0);
    assert_eq!(t.text(0), "PLOP3.LUT P0, PT, PT, PT, PT, 0x80, 0x0");
    let t = B::op(0x003, 4)
        .f(16, 8, 0x25)
        .f(24, 8, 0xff)
        .f(32, 32, 0x4)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "P2R R37, PR, RZ, 0x4");
    let t = B::op(0x004, 4)
        .f(24, 8, 0x7)
        .f(32, 32, 0x70)
        .f(76, 2, 0x3)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "R2P PR, R7.B3, 0x70");
    let t = B::op(0x004, 4).f(24, 8, 0x7).f(32, 32, 0xff).f(87, 3, 0x7);
    assert_eq!(t.text(0), "R2P PR, R7");
    let t = B::op(0x005, 4)
        .f(16, 8, 0xa)
        .f(72, 8, 0x50)
        .f(80, 1, 0x1)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "CS2R R10, SR_CLOCKLO");
    let t = B::op(0x005, 4)
        .f(16, 8, 0xa)
        .f(72, 8, 0xff)
        .f(80, 1, 0x0)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "CS2R.32 R10, SRZ");
    let t = B::op(0x022, 4)
        .f(16, 8, 0x18)
        .f(24, 8, 0xc)
        .f(64, 8, 0x9)
        .f(32, 8, 0xeb)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "FSWZADD R24, R12, R9, ZPNPNPZP");
    let t = B::op(0x082, 4)
        .f(16, 6, 0x4)
        .f(32, 32, 0x3f800000)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "UMOV UR4, 0x3f800000");
    let t = B::op(0x082, 6).f(16, 6, 0x5).f(32, 6, 0x3f).f(87, 3, 0x7);
    assert_eq!(t.text(0), "UMOV UR5, URZ");
    let t = B::op(0x08c, 4)
        .f(16, 6, 0x0)
        .f(24, 6, 0x4)
        .f(32, 32, 0x10)
        .f(76, 3, 0x2)
        .f(73, 1, 0x1)
        .f(81, 3, 0x1)
        .f(84, 3, 0x7)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "UISETP.EQ.AND UP1, UPT, UR4, 0x10, UPT");
    let t = B::op(0x090, 4)
        .f(16, 6, 0x4)
        .f(24, 6, 0x4)
        .f(32, 32, 0x24)
        .f(64, 6, 0x3f)
        .f(81, 3, 0x0)
        .f(84, 3, 0x7)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "UIADD3 UR4, UP0, UR4, 0x24, URZ");
    let t = B::op(0x091, 4)
        .f(16, 6, 0x4)
        .f(24, 6, 0x4)
        .f(32, 32, 0x40)
        .f(64, 6, 0x3f)
        .f(75, 5, 0x4)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "ULEA UR4, UP0, UR4, 0x40, 0x4");
    let t = B::op(0x092, 4)
        .f(16, 6, 0x6)
        .f(24, 6, 0x5)
        .f(32, 32, 0xff)
        .f(64, 6, 0x3f)
        .f(72, 8, 0xc0)
        .f(81, 3, 0x7)
        .f(87, 3, 0x7)
        .f(90, 1, 0x1);
    assert_eq!(t.text(0), "ULOP3.LUT UR6, UR5, 0xff, URZ, 0xc0, !UPT");
    let t = B::op(0x099, 4)
        .f(16, 6, 0x4)
        .f(24, 6, 0x4)
        .f(32, 32, 0x4)
        .f(64, 6, 0x3f)
        .f(73, 2, 0x3)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "USHF.L.U32 UR4, UR4, 0x4, URZ");
    let t = B::op(0x09c, 4)
        .f(81, 3, 0x0)
        .f(84, 3, 0x7)
        .f(87, 3, 0x7)
        .f(77, 3, 0x7)
        .f(68, 3, 0x7)
        .f(72, 5, 0x10);
    assert_eq!(t.text(0), "UPLOP3.LUT UP0, UPT, UPT, UPT, UPT, 0x80, 0x0");
    let t = B::op(0x0a4, 1)
        .f(16, 6, 0x5)
        .f(24, 6, 0x5)
        .f(32, 6, 0x6)
        .f(64, 6, 0x3f)
        .f(73, 1, 0x1)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "UIMAD UR5, UR5, UR6, URZ");
    let t = B::op(0x0a5, 1)
        .f(16, 6, 0xa)
        .f(24, 6, 0xc)
        .f(32, 6, 0xe)
        .f(64, 6, 0xa)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "UIMAD.WIDE.U32 UR10, UP0, UR12, UR14, UR10");
    let t = B::op(0x0bd, 1)
        .f(16, 6, 0x4)
        .f(32, 6, 0x4)
        .f(73, 1, 0x0)
        .f(74, 1, 0x1)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "UFLO.U32.SH UR4, UP0, UR4");
    let t = B::op(0x0be, 1).f(16, 6, 0x4).f(32, 6, 0x5).f(87, 3, 0x7);
    assert_eq!(t.text(0), "UBREV UR4, UR5");
    let t = B::op(0x0bf, 1).f(16, 6, 0x9).f(32, 6, 0x8).f(87, 3, 0x7);
    assert_eq!(t.text(0), "UPOPC UR9, UR8");
    let t = B::op(0x100, 1)
        .f(16, 8, 0x6)
        .f(32, 8, 0x6)
        .f(73, 1, 0x0)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "FLO.U32 R6, P0, R6");
    let t = B::op(0x101, 1).f(16, 8, 0x6).f(32, 8, 0x6).f(87, 3, 0x7);
    assert_eq!(t.text(0), "BREV R6, R6");
    let t = B::op(0x109, 1).f(16, 8, 0x7).f(32, 8, 0x7).f(87, 3, 0x7);
    assert_eq!(t.text(0), "POPC R7, R7");
    let t = B::op(0x045, 1)
        .f(16, 8, 0x8)
        .f(32, 8, 0x1)
        .f(84, 3, 0x2)
        .f(75, 3, 0x2)
        .f(74, 1, 0x1)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "I2FP.F32.S32 R8, R1");
    let t = B::op(0x045, 1)
        .f(16, 8, 0x44)
        .f(32, 8, 0x41)
        .f(84, 3, 0x2)
        .f(75, 3, 0x2)
        .f(78, 2, 0x3)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "I2FP.F32.U32.RZ R68, R65");
    let t = B::op(0x105, 1)
        .f(16, 8, 0x2a)
        .f(32, 8, 0x2e)
        .f(75, 2, 0x2)
        .f(77, 1, 0x1)
        .f(78, 2, 0x3)
        .f(80, 1, 0x1)
        .f(84, 3, 0x2)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "F2I.FTZ.U32.TRUNC.NTZ R42, R46");
    let t = B::op(0x105, 1)
        .f(16, 8, 0x6)
        .f(32, 8, 0x8)
        .f(75, 2, 0x1)
        .f(77, 1, 0x1)
        .f(84, 3, 0x2)
        .f(80, 1, 0x1)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "F2I.FTZ.U16.NTZ R6, R8");
    let t = B::op(0x106, 1)
        .f(16, 8, 0x1)
        .f(32, 8, 0x6)
        .f(74, 1, 0x0)
        .f(75, 3, 0x2)
        .f(84, 3, 0x1)
        .f(60, 2, 0x1)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "I2F.U16 R1, R6.H1");
    let t = B::op(0x107, 1)
        .f(16, 8, 0x7)
        .f(32, 8, 0x6)
        .f(75, 3, 0x2)
        .f(84, 3, 0x2)
        .f(78, 2, 0x1)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "FRND.FLOOR R7, R6");
    let t = B::op(0x104, 1)
        .f(16, 8, 0x1)
        .f(32, 8, 0x3)
        .f(75, 3, 0x1)
        .f(84, 3, 0x2)
        .f(78, 2, 0x1)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "F2F.F16.F32.RM R1, R3");
    let t = B::op(0x03e, 1)
        .f(16, 8, 0x6)
        .f(24, 8, 0x9)
        .f(32, 8, 0x6)
        .f(75, 3, 0x0)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "F2FP.PACK_AB R6, R9, R6");
    let t = B::op(0x030, 1)
        .f(16, 8, 0x1)
        .f(24, 8, 0x2)
        .f(32, 8, 0x3)
        .f(77, 1, 0x1)
        .f(78, 1, 0x1)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "HADD2.F32.SAT R1, R2, R3");
    let t = B::op(0x030, 2)
        .f(16, 8, 0x25)
        .f(24, 8, 0x1b)
        .f(32, 32, 0xbc003c00)
        .f(64, 8, 0xff)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "HADD2 R37, R27, -1, 1");
    let t = B::op(0x031, 1)
        .f(16, 8, 0x1)
        .f(24, 8, 0x2)
        .f(32, 8, 0x3)
        .f(64, 8, 0x4)
        .f(79, 1, 0x1)
        .f(76, 1, 0x1)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "HFMA2.FMZ.RELU R1, R2, R3, R4");
    let t = B::op(0x032, 1)
        .f(16, 8, 0x1)
        .f(24, 8, 0x2)
        .f(32, 8, 0x3)
        .f(75, 2, 0x3)
        .f(77, 1, 0x1)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "HMUL2.FMZ.SAT R1, R2.H0_H0, R3");
    let t = B::op(0x033, 1)
        .f(16, 8, 0x1)
        .f(24, 8, 0x2)
        .f(32, 8, 0x3)
        .f(76, 4, 0x1)
        .f(71, 1, 0x1)
        .f(69, 2, 0x1)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "HSET2.BF.LT.OR R1, R2, R3, PT");
    let t = B::op(0x034, 1)
        .f(81, 3, 0x1)
        .f(84, 3, 0x7)
        .f(24, 8, 0x2)
        .f(32, 8, 0x3)
        .f(76, 4, 0x4)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "HSETP2.GT.AND P1, PT, R2, R3, PT");
    let t = B::op(0x035, 4)
        .f(16, 8, 0x4)
        .f(24, 8, 0xff)
        .f(32, 32, 0x1000)
        .f(64, 8, 0xff)
        .f(72, 1, 0x1)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "HFMA2.MMA R4, -RZ, 0, 0.00048828125, RZ");
    let t = B::op(0x040, 1)
        .f(16, 8, 0x1)
        .f(24, 8, 0x2)
        .f(32, 8, 0x3)
        .f(81, 1, 0x1)
        .f(87, 3, 0x7)
        .f(90, 1, 0x1);
    assert_eq!(t.text(0), "HMNMX2.NAN R1, R2, R3, !PT");
    let t = B::op(0x026, 1)
        .f(16, 8, 0x1)
        .f(24, 8, 0x2)
        .f(32, 8, 0x3)
        .f(64, 8, 0x4)
        .f(73, 2, 0x1)
        .f(76, 2, 0x3)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "IDP.2A.HI.S16.U8 R1, R2, R3, R4");
    let t = B::op(0x028, 1)
        .f(16, 8, 0x1)
        .f(24, 8, 0x2)
        .f(32, 8, 0x3)
        .f(78, 2, 0x1)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "DMUL.RM R1, R2, R3");
    let t = B::op(0x029, 1)
        .f(16, 8, 0x1)
        .f(24, 8, 0x2)
        .f(64, 8, 0x4)
        .f(75, 1, 0x1)
        .f(78, 2, 0x2)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "DADD.RP R1, R2, -R4");
    let t = B::op(0x02a, 1)
        .f(81, 3, 0x1)
        .f(84, 3, 0x7)
        .f(24, 8, 0x2)
        .f(32, 8, 0x3)
        .f(76, 4, 0x4)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "DSETP.GT.AND P1, PT, R2, R3, PT");
    let t = B::op(0x02b, 1)
        .f(16, 8, 0x1)
        .f(24, 8, 0x2)
        .f(32, 8, 0x3)
        .f(64, 8, 0x4)
        .f(74, 1, 0x1)
        .f(87, 3, 0x7);
    assert_eq!(t.text(0), "DFMA R1, R2, R3, |R4|");
}

#[test]
fn memory() {
    let t = B::op(0x181, 4)
        .f(16, 3, 0x4)
        .f(24, 3, 0x5)
        .f(32, 3, 0x6)
        .f(40, 6, 0x20)
        .f(72, 1, 0x1)
        .f(73, 3, 0x6)
        .f(77, 3, 0x7)
        .f(84, 1, 0x1)
        .f(90, 1, 0x0)
        .f(91, 1, 0x1)
        .f(81, 3, 0x7);
    assert_eq!(t.text(0), "LDG.E.128.STRONG.GPU R4, [R5.U32+UR6+0x20]");
    let t = B::op(0x186, 4)
        .f(24, 3, 0x5)
        .f(32, 4, 0x8)
        .f(40, 24, 0xfffff0)
        .f(64, 4, 0xa)
        .f(72, 1, 0x1)
        .f(73, 3, 0x5)
        .f(77, 3, 0x5)
        .f(84, 1, 0x1)
        .f(90, 1, 0x1)
        .f(91, 1, 0x1);
    assert_eq!(t.text(0), "STG.E.64.STRONG.SM [R5.64+UR10+-0x10], R8");
    let t = B::op(0x181, 1)
        .f(16, 3, 0x4)
        .f(24, 3, 0x5)
        .f(40, 5, 0x10)
        .f(72, 1, 0x1)
        .f(73, 2, 0x2)
        .f(77, 3, 0x4)
        .f(84, 1, 0x1)
        .f(81, 3, 0x7);
    assert_eq!(t.text(0), "LDG.E.U16.CONSTANT R4, [R5+0x10]");
    let t = B::op(0x184, 4)
        .f(16, 3, 0x4)
        .f(24, 8, 0xff)
        .f(40, 11, 0x400)
        .f(73, 3, 0x6);
    assert_eq!(t.text(0), "LDS.128 R4, [0x400]");
    let t = B::op(0x184, 4)
        .f(16, 3, 0x4)
        .f(24, 3, 0x5)
        .f(40, 4, 0xc)
        .f(73, 3, 0x4)
        .f(78, 2, 0x2)
        .f(91, 1, 0x1)
        .f(32, 6, 0x3f);
    assert_eq!(t.text(0), "LDS R4, [R5.X8+URZ+0xc]");
    let t = B::op(0x188, 4)
        .f(24, 3, 0x5)
        .f(32, 4, 0x8)
        .f(40, 5, 0x10)
        .f(73, 3, 0x6)
        .f(78, 2, 0x3)
        .f(91, 1, 0x1)
        .f(64, 2, 0x3);
    assert_eq!(t.text(0), "STS.128 [R5.X16+UR3+0x10], R8");
    let t = B::op(0x187, 1)
        .f(24, 3, 0x5)
        .f(32, 4, 0x8)
        .f(40, 5, 0x10)
        .f(73, 3, 0x5)
        .f(84, 1, 0x1);
    assert_eq!(t.text(0), "STL.64 [R5+0x10], R8");
    let t = B::op(0x182, 5)
        .f(16, 3, 0x4)
        .f(24, 4, 0x8)
        .f(38, 5, 0x10)
        .f(54, 3, 0x5)
        .f(73, 3, 0x5);
    assert_eq!(t.text(0), "LDC.64 R4, c[0x5][R8+0x10]");
    let t = B::op(0x182, 2)
        .f(16, 3, 0x4)
        .f(24, 2, 0x3)
        .f(64, 4, 0x8)
        .f(38, 16, 0xfff0)
        .f(91, 1, 0x1)
        .f(73, 2, 0x2);
    assert_eq!(t.text(0), "LDC.U16 R4, cx[UR3][R8+-0x10]");
    let t = B::op(0x0b9, 5)
        .f(16, 3, 0x4)
        .f(38, 9, 0x130)
        .f(54, 1, 0x0)
        .f(73, 3, 0x5);
    assert_eq!(t.text(0), "ULDC.64 UR4, c[0x0][0x130]");
    let t = B::op(0x0bb, 5)
        .f(16, 3, 0x4)
        .f(24, 3, 0x4)
        .f(38, 5, 0x10)
        .f(54, 3, 0x5)
        .f(73, 3, 0x5)
        .f(91, 1, 0x1);
    assert_eq!(t.text(0), "ULDC.64 UR4, c[0x5][UR4+0x10]");
    let t = B::op(0x18e, 4)
        .f(24, 3, 0x5)
        .f(32, 4, 0x8)
        .f(72, 1, 0x1)
        .f(87, 1, 0x1)
        .f(73, 1, 0x0)
        .f(77, 3, 0x7)
        .f(84, 1, 0x1);
    assert_eq!(t.text(0), "RED.E.MIN.STRONG.GPU [R5], R8");
    let t = B::op(0x1a8, 4)
        .f(16, 3, 0x4)
        .f(24, 8, 0xff)
        .f(32, 3, 0x5)
        .f(64, 3, 0x4)
        .f(72, 1, 0x1)
        .f(91, 1, 0x1)
        .f(87, 1, 0x0)
        .f(77, 3, 0x7)
        .f(81, 3, 0x7)
        .f(84, 1, 0x1);
    assert_eq!(t.text(0), "ATOMG.E.ADD.STRONG.GPU PT, R4, [UR4], R5");
    let t = B::op(0x1a9, 1)
        .f(16, 3, 0x4)
        .f(24, 3, 0x5)
        .f(32, 3, 0x6)
        .f(64, 3, 0x7)
        .f(72, 1, 0x1)
        .f(81, 3, 0x7)
        .f(84, 1, 0x1);
    assert_eq!(t.text(0), "ATOMG.E.CAS PT, R4, [R5], R6, R7");
    let t = B::op(0x189, 1)
        .f(16, 3, 0x4)
        .f(24, 3, 0x5)
        .f(32, 3, 0x6)
        .f(64, 3, 0x7)
        .f(58, 2, 0x3)
        .f(81, 3, 0x7);
    assert_eq!(t.text(0), "SHFL.BFLY PT, R4, R5, R6, R7");
    let t = B::op(0x189, 2)
        .f(16, 3, 0x4)
        .f(24, 3, 0x5)
        .f(32, 3, 0x6)
        .f(40, 13, 0x1c03)
        .f(58, 1, 0x0)
        .f(81, 2, 0x3);
    assert_eq!(t.text(0), "SHFL.IDX P3, R4, R5, R6, 0x1c03");
    let t = B::op(0x189, 4)
        .f(16, 3, 0x4)
        .f(24, 3, 0x5)
        .f(53, 1, 0x1)
        .f(64, 3, 0x7)
        .f(58, 2, 0x3)
        .f(81, 3, 0x7);
    assert_eq!(t.text(0), "SHFL.BFLY PT, R4, R5, 0x1, R7");
    let t = B::op(0x192, 4).f(76, 2, 0x2).f(79, 1, 0x0);
    assert_eq!(t.text(0), "MEMBAR.SC.GPU");
    let t = B::op(0x1ab, 4);
    assert_eq!(t.text(0), "ERRBAR");
    let t = B::op(0x18f, 4)
        .f(24, 2, 0x2)
        .f(32, 5, 0x10)
        .f(72, 1, 0x1)
        .f(87, 2, 0x2)
        .f(78, 2, 0x2);
    assert_eq!(t.text(0), "CCTL.E.C.WB [R2+0x10]");
    let t = B::op(0x126, 1)
        .f(16, 2, 0x2)
        .f(64, 6, 0x24)
        .f(76, 1, 0x1)
        .f(78, 1, 0x1)
        .f(81, 3, 0x7);
    assert_eq!(t.text(0), "IPA.CONSTANT.CENTROID R2, a[0x90]");
    let t = B::op(0x126, 2)
        .f(16, 2, 0x2)
        .f(24, 3, 0x4)
        .f(64, 6, 0x21)
        .f(81, 2, 0x2)
        .f(91, 1, 0x1);
    assert_eq!(t.text(0), "IPA.PASS R2, P2, a[UR4+0x84]");
    let t = B::op(0x123, 4)
        .f(16, 3, 0x5)
        .f(24, 8, 0xff)
        .f(40, 1, 0x0)
        .f(74, 2, 0x2)
        .f(76, 2, 0x3)
        .f(78, 1, 0x1);
    assert_eq!(t.text(0), "ISBERD.ATTR.SKEW.32 R5, [RZ]");
    let t = B::op(0x127, 4)
        .f(24, 1, 0x1)
        .f(32, 2, 0x2)
        .f(40, 5, 0x10)
        .f(74, 2, 0x2)
        .f(76, 2, 0x3)
        .f(79, 1, 0x1);
    assert_eq!(t.text(0), "ISBEWR.O.ATTR.32 [R1+0x10], R2");
    let t = B::op(0x120, 4)
        .f(16, 2, 0x2)
        .f(24, 2, 0x3)
        .f(40, 3, 0x5)
        .f(74, 2, 0x2)
        .f(79, 1, 0x1);
    assert_eq!(t.text(0), "AL2P.O.96 R2, R3, 0x5");
    let t = B::op(0x124, 1)
        .f(16, 2, 0x2)
        .f(24, 2, 0x3)
        .f(32, 3, 0x4)
        .f(78, 1, 0x1);
    assert_eq!(t.text(0), "OUT.EMIT R2, R3, R4");
    let t = B::op(0x125, 4).f(16, 2, 0x2).f(78, 2, 0x2).f(81, 3, 0x7);
    assert_eq!(t.text(0), "PIXLD.CENTROID_OFFSET R2");
    let t = B::op(0x1ad, 1)
        .f(16, 2, 0x2)
        .f(24, 6, 0x3f)
        .f(64, 4, 0x8)
        .f(91, 1, 0x1);
    assert_eq!(t.text(0), "LDTRAM.AB R2, a[0x20]");
    let t = B::op(0x18d, 1)
        .f(16, 3, 0x4)
        .f(24, 3, 0x5)
        .f(32, 3, 0x6)
        .f(64, 3, 0x7)
        .f(78, 2, 0x3)
        .f(73, 1, 0x1);
    assert_eq!(t.text(0), "ATOMS.CAS.S32 R4, [R5.X16], R6, R7");
    let t = B::op(0x18c, 4)
        .f(16, 3, 0x4)
        .f(24, 3, 0x5)
        .f(32, 3, 0x6)
        .f(64, 2, 0x3)
        .f(91, 1, 0x1)
        .f(87, 3, 0x5);
    assert_eq!(t.text(0), "ATOMS.AND R4, [R5+UR3], R6");
    let t = B::op(0x18b, 1)
        .f(16, 3, 0x4)
        .f(24, 3, 0x5)
        .f(32, 3, 0x6)
        .f(64, 3, 0x7)
        .f(81, 3, 0x7)
        .f(87, 2, 0x3);
    assert_eq!(t.text(0), "ATOM.CAST.SPIN.EF PT, R4, [R5], R6, R7");
}
