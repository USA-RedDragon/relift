use relift_gpu_sm53::field::Kind;
use relift_gpu_sm53::form::ENCODINGS;
use relift_gpu_sm53::lower::flow::{flow, offset};
use relift_gpu_sm53::lower::{program, stops};
use relift_gpu_sm53::{Op, Program, Src};

/// One synthetic instruction: the encoding, its fields, and a branch
/// target index (for the forms with one).
struct I {
    op: Op,
    src: Src,
    fields: Vec<(&'static str, u64)>,
    to: Option<usize>,
}

fn i(op: Op, fields: &[(&'static str, u64)]) -> I {
    I {
        op,
        src: Src::None,
        fields: fields.to_vec(),
        to: None,
    }
}

fn to(op: Op, t: usize, fields: &[(&'static str, u64)]) -> I {
    I {
        to: Some(t),
        ..i(op, fields)
    }
}

fn mov(d: u64, v: u64) -> I {
    I {
        src: Src::Imm32,
        ..i(Op::Mov, &[("Dest", d), ("Imm32", v), ("QuadMask", 0xf)])
    }
}

const T: (&str, u64) = ("Ccc", 15);
const P0: (&str, u64) = ("Pred", 0);

/// The word of instruction `k`: its encoding with the fields set, the
/// guard PT unless the fields name one, and the target as an offset from
/// the instruction after it.
fn word(k: usize, x: &I) -> u64 {
    let e = ENCODINGS
        .iter()
        .find(|e| (e.op, e.src) == (x.op, x.src))
        .expect("an encoding");
    let mut fields = vec![("Pred", 7)];
    fields.extend(x.fields.iter().copied());
    if let Some(t) = x.to {
        let off = offset(t) as i64 - (offset(k) as i64 + 8);
        fields.push(("Imm24", (off as u64) & 0xff_ffff));
    }
    let mut w = e.bits;
    for (name, v) in fields {
        let Some(f) = e.form.field(name) else {
            continue;
        };
        let &[(shift, mask)] = f.parts else {
            panic!("{name} has several parts");
        };
        let v = if f.kind == Kind::Bool && v != 0 {
            mask
        } else {
            v & mask
        };
        w = (w & !(mask << shift)) | (v << shift);
    }
    w
}

/// A fragment program of `code` (bundles with zero scheduling words),
/// ending in `BRA` to itself.
fn fragment(code: &[I]) -> Program {
    let mut b = 0x1234_5678u32.to_le_bytes().to_vec();
    b.resize(0x30, 0);
    let mut sph = [0u32; 20];
    sph[0] = 2 | (3 << 5) | (5 << 10);
    sph[18] = 0xf;
    for w in sph {
        b.extend(w.to_le_bytes());
    }
    let mut words: Vec<u64> = code.iter().enumerate().map(|(k, x)| word(k, x)).collect();
    let k = words.len();
    words.push(word(k, &to(Op::Bra, k, &[T])));
    while !words.len().is_multiple_of(3) {
        words.push(word(0, &i(Op::Nop, &[])));
    }
    for c in words.chunks(3) {
        b.extend(0u64.to_le_bytes());
        for w in c {
            b.extend(w.to_le_bytes());
        }
    }
    Program::parse(&b).expect("parses")
}

/// The SM86 text of each lowered word.
fn lowered(p: &Program) -> Vec<String> {
    let l = program(p).expect("lowers");
    l.words
        .iter()
        .enumerate()
        .map(|(k, w)| {
            relift_gpu_sm86::decode(*w, 16 * k as u64)
                .map(|i| i.to_string())
                .unwrap_or_else(|e| format!("?? {e}"))
        })
        .collect()
}

fn translates(p: &Program) {
    let l = program(p).expect("lowers");
    if let Err(e) = relift_gpu_sm86::translate::translate(&l) {
        panic!("translate: {e:?}");
    }
}

fn reasons(p: &Program) -> Vec<&'static str> {
    stops(p).iter().map(|s| s.reason).collect()
}

#[test]
fn ssy_and_sync_become_bssy_and_branches() {
    let p = fragment(&[
        to(Op::Ssy, 6, &[]),
        to(Op::Bra, 4, &[T, P0]),
        mov(0, 1),
        i(Op::Sync, &[T]),
        mov(0, 2),
        i(Op::Sync, &[T]),
        mov(1, 3),
        i(Op::Exit, &[T]),
    ]);
    let f = flow(&p.code).expect("resolves");
    assert_eq!(f.pushes.into_iter().collect::<Vec<_>>(), [(0, (0, 6))]);
    assert_eq!(f.pops.into_iter().collect::<Vec<_>>(), [(3, 6), (5, 6)]);
    let t = lowered(&p);
    assert_eq!(t[0], "BSSY B0, 0x60");
    assert_eq!(t[1], "@P0 BRA 0x40");
    assert_eq!(t[3], "BRA 0x60");
    assert_eq!(t[5], "BRA 0x60");
    assert_eq!(t[7], "EXIT");
    translates(&p);
}

#[test]
fn a_guarded_sync_falls_through_when_its_guard_fails() {
    let p = fragment(&[
        to(Op::Ssy, 4, &[]),
        i(Op::Sync, &[T, P0, ("PredInv", 1)]),
        mov(0, 1),
        i(Op::Sync, &[T]),
        i(Op::Exit, &[T]),
    ]);
    let t = lowered(&p);
    assert_eq!(t[1], "@!P0 BRA 0x40");
    assert_eq!(t[3], "BRA 0x40");
    translates(&p);
}

#[test]
fn brk_unwinds_the_ssy_entries_above_its_pbk() {
    let p = fragment(&[
        to(Op::Pbk, 7, &[]),
        to(Op::Ssy, 5, &[]),
        i(Op::Brk, &[T, P0]),
        mov(0, 1),
        i(Op::Sync, &[T]),
        to(Op::Bra, 1, &[T, ("Pred", 1)]),
        i(Op::Brk, &[T]),
        i(Op::Exit, &[T]),
    ]);
    let f = flow(&p.code).expect("resolves");
    assert_eq!(
        f.pushes.into_iter().collect::<Vec<_>>(),
        [(0, (0, 7)), (1, (1, 5))]
    );
    assert_eq!(
        f.pops.into_iter().collect::<Vec<_>>(),
        [(2, 7), (4, 5), (6, 7)]
    );
    let t = lowered(&p);
    assert_eq!(t[0], "BSSY B0, 0x70");
    assert_eq!(t[1], "BSSY B1, 0x50");
    assert_eq!(t[2], "@P0 BRA 0x70");
    assert_eq!(t[5], "@P1 BRA 0x10");
    translates(&p);
}

#[test]
fn unreached_words_become_nops() {
    let p = fragment(&[to(Op::Bra, 2, &[T]), mov(0, 1), i(Op::Exit, &[T])]);
    let t = lowered(&p);
    assert_eq!(t[..3], ["BRA 0x20", "NOP", "EXIT"]);
    translates(&p);
}

#[test]
fn an_unguarded_kil_ends_the_invocation_after_it() {
    let p = fragment(&[
        to(Op::Bra, 3, &[T, P0]),
        i(Op::Kil, &[T]),
        mov(0, 1),
        i(Op::Exit, &[T]),
    ]);
    let f = flow(&p.code).expect("resolves");
    assert!(f.kil_exits.contains(&2));
    let t = lowered(&p);
    assert_eq!(t[1..3], ["KILL", "EXIT"]);
    translates(&p);
    let reached = fragment(&[
        to(Op::Bra, 2, &[T, P0]),
        i(Op::Kil, &[T]),
        mov(0, 1),
        i(Op::Exit, &[T]),
    ]);
    assert!(flow(&reached.code).expect("resolves").kil_exits.is_empty());
    assert_eq!(lowered(&reached)[2], "MOV R0, 0x1");
    translates(&reached);
}

#[test]
fn unresolved_control_stops_with_its_reason() {
    let two = fragment(&[
        to(Op::Bra, 3, &[T, P0]),
        to(Op::Ssy, 5, &[]),
        to(Op::Bra, 4, &[T]),
        to(Op::Ssy, 6, &[]),
        i(Op::Sync, &[T]),
        i(Op::Exit, &[T]),
        i(Op::Exit, &[T]),
    ]);
    assert_eq!(reasons(&two), ["pop that more than one push reaches"]);
    let depths = fragment(&[
        to(Op::Bra, 2, &[T, P0]),
        to(Op::Ssy, 5, &[]),
        to(Op::Ssy, 5, &[]),
        mov(0, 1),
        i(Op::Sync, &[T]),
        i(Op::Exit, &[T]),
    ]);
    assert_eq!(reasons(&depths), ["push at two control stack depths"]);
    let none = fragment(&[i(Op::Sync, &[T]), i(Op::Exit, &[T])]);
    assert_eq!(reasons(&none), ["pop with no push of its kind"]);
    let cc = fragment(&[to(Op::Bra, 2, &[("Ccc", 13)]), mov(0, 1), i(Op::Exit, &[T])]);
    assert_eq!(reasons(&cc), ["branch on a condition code test"]);
    let brx = fragment(&[i(Op::Brx, &[T, ("SrcA", 0)]), i(Op::Exit, &[T])]);
    assert_eq!(
        reasons(&brx),
        ["indirect jump (its targets are in a constant bank)"]
    );
}

#[test]
fn a_float_program_translates_whole() {
    let f = |op: Op, src: Src, fields: &[(&'static str, u64)]| I {
        src,
        ..i(op, fields)
    };
    let p = fragment(&[
        mov(0, 0x3f00_0000),
        f(Op::Rro, Src::Reg, &[("Dest", 1), ("SrcB", 0)]),
        f(
            Op::Mufu,
            Src::None,
            &[("Dest", 1), ("SrcA", 1), ("MufuOp", 1)],
        ),
        f(
            Op::Fmul,
            Src::Reg,
            &[
                ("Dest", 2),
                ("SrcA", 1),
                ("SrcB", 0),
                ("Fmz", 1),
                ("Scale", 6),
            ],
        ),
        f(
            Op::Ffma,
            Src::Reg,
            &[
                ("Dest", 3),
                ("SrcA", 2),
                ("SrcB", 2),
                ("SrcC", 1),
                ("Sat", 1),
            ],
        ),
        f(
            Op::Fadd,
            Src::Reg,
            &[
                ("Dest", 0),
                ("SrcA", 3),
                ("SrcB", 1),
                ("NegB", 1),
                ("Ftz", 1),
            ],
        ),
        f(
            Op::Mufu,
            Src::None,
            &[("Dest", 1), ("SrcA", 0), ("MufuOp", 2), ("Sat", 1)],
        ),
        i(Op::Exit, &[T]),
    ]);
    let t = lowered(&p);
    assert_eq!(t[2], "MUFU.SIN R1, R1");
    assert_eq!(t[3], "FMUL.FTZ.M2 R2, R1, R0");
    assert_eq!(t[4], "FFMA.SAT R3, R2, R2, R1");
    assert_eq!(t[5], "FADD.FTZ R0, R3, -R1");
    let l = program(&p).expect("lowers");
    assert_eq!(l.pseudo[&1].to_string(), "RRO.SINCOS R1, R0");
    assert_eq!(l.pseudo[&6].to_string(), "MUFU.EX2.SAT R1, R0");
    translates(&p);
}
