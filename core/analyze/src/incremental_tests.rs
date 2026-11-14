use relift_guest::toy::{Toy, branch, word};
use relift_guest::{Frontend, Module, Root, RootKind, Section, SectionKind};

use super::*;

const BASE: u64 = 0x1000;

fn module(code: &[[u8; 4]]) -> Module {
    let image: Vec<u8> = code.iter().flatten().copied().collect();
    Module {
        name: "t".into(),
        base: BASE,
        sections: vec![Section {
            addr: BASE,
            size: image.len() as u64,
            kind: SectionKind::Code,
        }],
        image,
        roots: vec![Root {
            addr: BASE,
            kind: RootKind::Entry,
            name: Some("main".into()),
        }],
        pads: Vec::new(),
    }
}

/// main calls f at +16 and g at +28; f and g each load a constant.
fn program(f_imm: u16, g_call: bool) -> Vec<[u8; 4]> {
    vec![
        branch(0x02, 0, 4),
        branch(0x02, 0, 6),
        word(0x00, 0, 0),
        word(0x03, 0, 0),
        word(0x04, 1, f_imm),
        word(0x00, 0, 0),
        word(0x03, 0, 0),
        word(0x04, 1, 9),
        if g_call {
            branch(0x02, 0, 2)
        } else {
            word(0x00, 0, 0)
        },
        word(0x03, 0, 0),
        word(0x04, 2, 1),
        word(0x03, 0, 0),
    ]
}

#[test]
fn only_the_changed_function_is_lifted_again() {
    let old = module(&program(7, false));
    let base = discover(&Toy, &old);
    let new = module(&program(8, false));
    let changed = changed_bytes(&old.image, &new.image);
    let (a, reuse) = rediscover(&Toy, &new, &base, &changed, &mut |_| {});
    assert_eq!(reuse.lifted, [BASE + 16].into());
    assert_eq!(reuse.reused, [BASE, BASE + 28].into());
    assert_eq!(a, discover(&Toy, &new));
}

#[test]
fn a_new_call_in_changed_code_adds_its_callee() {
    let old = module(&program(7, false));
    let base = discover(&Toy, &old);
    let new = module(&program(7, true));
    let changed = changed_bytes(&old.image, &new.image);
    let (a, reuse) = rediscover(&Toy, &new, &base, &changed, &mut |_| {});
    assert_eq!(reuse.lifted, [BASE + 28, BASE + 40].into());
    assert_eq!(a, discover(&Toy, &new));
}

#[test]
fn the_binary_form_round_trips() {
    let m = module(&program(7, true));
    let mut a = discover(&Toy, &m);
    a.functions
        .get_mut(&BASE)
        .expect("main")
        .tables
        .insert(BASE + 4, vec![BASE + 8]);
    let t = to_bytes(&a, "k 1", &[Toy.regs()]);
    let (key, back) = from_bytes(&t, &[Toy.regs()]).expect("reads");
    assert_eq!(key, "k 1");
    assert_eq!(back, a);
    assert!(from_bytes(b"nope", &[Toy.regs()]).is_err());
    for cut in 0..t.len() {
        assert!(from_bytes(&t[..cut], &[Toy.regs()]).is_err());
    }
}

fn changed_bytes(a: &[u8], b: &[u8]) -> Vec<std::ops::Range<u64>> {
    let mut out = Vec::new();
    for (i, (x, y)) in a.iter().zip(b).enumerate() {
        if x != y {
            let at = BASE + i as u64;
            out.push(at..at + 1);
        }
    }
    out
}
