//! Absolute calls and returns, LEPC, JMP and BPT.TRAP, on hand-encoded
//! programs laid out the way discovery links a CUDA kernel: the kernel at
//! 0, each callee after the previous function's `BRA self`.

use relift_gpu_nv::program::Program;
use relift_gpu_nv::translate::translate;

fn word(op: u32, form: u32, fields: &[(u32, u32, u64)]) -> [u8; 16] {
    let mut w: u128 =
        u128::from(op) | (u128::from(form) << 9) | (7u128 << 12) | (7u128 << 110) | (7u128 << 113);
    for &(lo, len, v) in fields {
        let m = ((1u128 << len) - 1) << lo;
        w = (w & !m) | ((u128::from(v) << lo) & m);
    }
    w.to_le_bytes()
}

fn mov_imm(dst: u64, v: u32) -> [u8; 16] {
    word(
        0x002,
        4,
        &[(16, 8, dst), (32, 32, u64::from(v)), (72, 4, 0xf)],
    )
}

fn iadd_imm(dst: u64, a: u64, v: u32) -> [u8; 16] {
    word(
        0x010,
        4,
        &[
            (16, 8, dst),
            (24, 8, a),
            (32, 32, u64::from(v)),
            (64, 8, 255),
            (81, 3, 7),
            (84, 3, 7),
        ],
    )
}

fn call_abs(to: u64) -> [u8; 16] {
    word(0x143, 4, &[(34, 47, to / 4), (86, 1, 1), (87, 3, 7)])
}

fn call_abs_inc(to: u64) -> [u8; 16] {
    word(0x143, 4, &[(34, 47, to / 4), (87, 3, 7)])
}

fn call_reg(reg: u64) -> [u8; 16] {
    word(0x143, 1, &[(24, 8, reg), (86, 1, 1), (87, 3, 7)])
}

fn ret_abs(reg: u64, off: u64) -> [u8; 16] {
    word(
        0x150,
        4,
        &[
            (24, 8, reg),
            (34, 48, off / 4),
            (85, 1, 1),
            (86, 1, 1),
            (87, 3, 7),
        ],
    )
}

fn lepc(dst: u64) -> [u8; 16] {
    word(0x14e, 1, &[(16, 8, dst)])
}

fn jmp(to: u64) -> [u8; 16] {
    word(0x14a, 4, &[(34, 47, to / 4), (87, 3, 7)])
}

fn trap(guard: u64) -> [u8; 16] {
    word(0x15c, 4, &[(34, 30, 1), (84, 3, 3), (12, 3, guard)])
}

fn isetp(pdst: u64, a: u64, b: u64, cmp: u64) -> [u8; 16] {
    word(
        0x00c,
        1,
        &[
            (24, 8, a),
            (32, 8, b),
            (73, 1, 1),
            (76, 3, cmp),
            (81, 3, pdst),
            (84, 3, 7),
            (87, 3, 7),
        ],
    )
}

fn exit() -> [u8; 16] {
    word(0x14d, 4, &[(87, 3, 7)])
}

fn nop() -> [u8; 16] {
    word(0x118, 4, &[])
}

fn halt() -> [u8; 16] {
    word(0x147, 4, &[(34, 48, 0xffff_ffff_fffc), (87, 3, 7)])
}

/// Ends the function in `b` with `BRA self` and pads it with NOPs to
/// 0x80 bytes, as a cubin section is.
fn end_function(b: &mut Vec<u8>) {
    b.extend(halt());
    while !b.len().is_multiple_of(0x80) {
        b.extend(nop());
    }
}

fn text(p: &Program) -> Vec<String> {
    p.code
        .clone()
        .into_iter()
        .map(|d| {
            d.map(|i| i.to_string())
                .unwrap_or_else(|e| format!("?? {e}"))
        })
        .collect()
}

fn count_op(words: &[u32], opcode: u32) -> usize {
    let mut k = 5;
    let mut n = 0;
    while k < words.len() {
        if words[k] & 0xffff == opcode {
            n += 1;
        }
        k += ((words[k] >> 16) as usize).max(1);
    }
    n
}

fn err(b: &[u8]) -> Option<String> {
    translate(&relift_gpu_sm86::program::parse(b))
        .err()
        .map(|e| e.0)
}

/// The callee at 0x80: R1 += 1, then `RET.ABS.NODEC R20`.
fn callee(b: &mut Vec<u8>) {
    b.extend(iadd_imm(1, 1, 1));
    b.extend(ret_abs(20, 0));
    end_function(b);
}

#[test]
fn program_keeps_every_function() {
    let mut b = Vec::new();
    b.extend(exit());
    end_function(&mut b);
    callee(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    assert_eq!(p.code.len(), 0x80 / 16 + 3);
}

fn sites(n: usize) -> Vec<u8> {
    let mut b = Vec::new();
    let f = ((3 * n as u64 + 2) * 16).next_multiple_of(0x80);
    for _ in 0..n {
        let at = b.len() as u32;
        b.extend(mov_imm(20, at + 0x30));
        b.extend(mov_imm(21, 0));
        b.extend(call_abs(f));
    }
    b.extend(exit());
    end_function(&mut b);
    callee(&mut b);
    b
}

#[test]
fn absolute_call_returns_through_the_moved_address() {
    let p = relift_gpu_sm86::program::parse(&sites(2));
    let t = text(&p);
    assert_eq!(t[2], "CALL.ABS.NOINC 0x80");
    assert_eq!(t[9], "RET.ABS.NODEC R20 0x0");
    let n: Vec<usize> = (1..=3)
        .map(|k| {
            let s = translate(&relift_gpu_sm86::program::parse(&sites(k)))
                .unwrap_or_else(|e| panic!("{e}"));
            count_op(&s.spirv, 128)
        })
        .collect();
    assert!(
        n[1] > n[0] && n[2] - n[1] == n[1] - n[0],
        "callee not inlined at each call: {n:?}"
    );
}

#[test]
fn lepc_sets_the_return_address() {
    let mut b = Vec::new();
    b.extend(lepc(12));
    b.extend(call_abs_inc(0x80));
    b.extend(exit());
    end_function(&mut b);
    b.extend(iadd_imm(1, 1, 1));
    b.extend(ret_abs(12, 0x20));
    end_function(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    let t = text(&p);
    assert_eq!(t[0], "LEPC R12");
    assert_eq!(t[1], "CALL.ABS 0x80");
    assert_eq!(err(&b), None);
    for (add, ok) in [(0x30, true), (0x20, false)] {
        let mut b = Vec::new();
        b.extend(lepc(20));
        b.extend(iadd_imm(20, 20, add));
        b.extend(call_abs(0x80));
        b.extend(exit());
        end_function(&mut b);
        callee(&mut b);
        let want = (!ok).then_some("CALL without its return address");
        assert_eq!(err(&b).as_deref(), want, "LEPC plus {add:#x}");
    }
}

#[test]
fn indirect_call_switches_over_the_functions() {
    let mut b = Vec::new();
    b.extend(mov_imm(8, 0x100));
    b.extend(mov_imm(9, 0));
    b.extend(mov_imm(20, 0x40));
    b.extend(call_reg(8));
    b.extend(exit());
    end_function(&mut b);
    callee(&mut b);
    b.extend(iadd_imm(1, 1, 2));
    b.extend(ret_abs(20, 0));
    end_function(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    assert_eq!(text(&p)[3], "CALL.ABS.NOINC R8");
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(count_op(&s.spirv, 251), 1, "no OpSwitch");
    assert!(
        s.spirv.contains(&(251 | (7 << 16))),
        "not a switch over a default and two functions"
    );
}

#[test]
fn unfilled_targets_are_rejected() {
    let mut b = Vec::new();
    b.extend(mov_imm(20, 0x30));
    b.extend(mov_imm(21, 0));
    b.extend(call_abs(0));
    b.extend(exit());
    end_function(&mut b);
    assert_eq!(err(&b).as_deref(), Some("CALL to an unrelocated address"));
    let mut j = Vec::new();
    j.extend(jmp(0));
    end_function(&mut j);
    assert_eq!(err(&j).as_deref(), Some("JMP to an unrelocated address"));
}

#[test]
fn function_alone_returns_outside_a_call() {
    let mut b = Vec::new();
    callee(&mut b);
    assert_eq!(err(&b).as_deref(), Some("RET outside a call"));
}

#[test]
fn jmp_is_a_jump() {
    let mut b = Vec::new();
    b.extend(jmp(0x20));
    b.extend(iadd_imm(1, 1, 1));
    b.extend(exit());
    end_function(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    assert_eq!(text(&p)[0], "JMP 0x20");
    assert_eq!(err(&b), None);
}

#[test]
fn trap_ends_the_invocation() {
    let mut b = Vec::new();
    b.extend(isetp(0, 1, 2, 2));
    b.extend(trap(0));
    b.extend(iadd_imm(1, 1, 1));
    b.extend(exit());
    end_function(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    assert_eq!(text(&p)[1], "@P0 BPT.TRAP 0x1");
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    assert!(count_op(&s.spirv, 253) >= 2, "trap does not return");
}

fn imad_mov(dst: u64, src: u64) -> [u8; 16] {
    let w = 0x000f_d800_078e_0002_0000_00ff_ff00_7224u128;
    let w = (w & !(0xff << 16) & !(0xff << 64)) | (u128::from(dst) << 16) | (u128::from(src) << 64);
    w.to_le_bytes()
}

/// A call 20 lines after its return address `MOV`, with `fill` between
/// (four lines each), to a callee that copies the address with `IMAD.MOV`
/// and returns through the copy.
fn far_site(fill: &[[u8; 16]]) -> Vec<u8> {
    let mut b = Vec::new();
    let at = 16 * (2 + 4 * fill.len() as u32 + 16);
    b.extend(mov_imm(20, at + 0x10));
    b.extend(mov_imm(21, 0));
    for f in fill {
        for _ in 0..4 {
            b.extend(f);
        }
    }
    for _ in 0..16 {
        b.extend(iadd_imm(1, 1, 1));
    }
    let f = (b.len() as u64 + 0x30).next_multiple_of(0x80);
    b.extend(call_abs(f));
    b.extend(exit());
    end_function(&mut b);
    b.extend(imad_mov(4, 20));
    b.extend(iadd_imm(1, 1, 1));
    b.extend(ret_abs(4, 0));
    end_function(&mut b);
    b
}

#[test]
fn return_address_is_followed_through_straight_code_and_copies() {
    let b = far_site(&[iadd_imm(2, 2, 1)]);
    let p = relift_gpu_sm86::program::parse(&b);
    assert!(text(&p).iter().any(|t| t == "IMAD.MOV.U32 R4, RZ, RZ, R20"));
    assert_eq!(err(&b), None);
    let clobbered = far_site(&[iadd_imm(20, 20, 0x10)]);
    assert_eq!(
        err(&clobbered).as_deref(),
        Some("CALL without its return address"),
        "a later write of the register replaces the address"
    );
}

fn raw(lo: u64, hi: u64) -> [u8; 16] {
    (u128::from(lo) | (u128::from(hi) << 64)).to_le_bytes()
}

/// A CUDA kernel's `assert`: `LDC.64 R2, c[0x4][R0]` with R0 = 0, then
/// `CALL.ABS.NOINC R2` (words from nvcc's code for `__assertfail`).
fn assert_kernel() -> Program {
    let words = [
        mov_imm(0, 0),
        raw(0x0100_0000_0002_7b82, 0x0000_6200_0000_0a00),
        raw(0x0000_0000_0200_7343, 0x022f_ea00_03c0_0000),
        raw(0x0000_0000_0000_794d, 0x000f_ea00_0380_0000),
    ];
    relift_gpu_sm86::program::parse(&words.concat())
}

#[test]
fn assertfail_ends_the_invocation_and_is_recorded() {
    use relift_gpu_nv::translate::{ASSERT_BINDING, Options, translate_with};
    let p = assert_kernel();
    assert!(translate(&p).is_err());
    let mut o = Options::default();
    o.externs.insert((4, 0), "__assertfail".into());
    let s = translate_with(&p, &o).unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(s.interface.asserts, [0x20]);
    let w = &s.spirv;
    let binding = (5..w.len()).any(|k| {
        w[k] & 0xffff == 71 && w.get(k + 2) == Some(&33) && w.get(k + 3) == Some(&ASSERT_BINDING)
    });
    assert!(binding, "no assert record binding");
    o.externs.insert((4, 0), "vprintf".into());
    let e = translate_with(&p, &o)
        .err()
        .map(|e| e.to_string())
        .unwrap_or_default();
    assert!(e.contains("vprintf"), "{e}");
}
