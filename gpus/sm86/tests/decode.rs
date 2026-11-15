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
