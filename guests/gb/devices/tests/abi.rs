use relift_gb_devices::{FRAME_DOTS, Rlgbd, RlgbdRegs};

fn rom() -> Vec<u8> {
    let mut r = vec![0u8; 0x8000];
    r[0x143] = 0x80;
    r[0x100..0x104].copy_from_slice(&[0x00, 0xC3, 0x50, 0x01]);
    let prog = [
        0x3E, 0x91, 0xE0, 0x40, 0x21, 0x00, 0xC0, 0x3C, 0x22, 0x18, 0xFB,
    ];
    r[0x150..0x150 + prog.len()].copy_from_slice(&prog);
    r
}

#[test]
fn rejects_a_short_image() {
    assert!(Rlgbd::new(1, &[0; 16]).is_none());
}

#[test]
fn native_fallback_matches_a_whole_native_step() {
    let rom = rom();
    let mut a = Rlgbd::new(1, &rom).unwrap();
    let mut b = Rlgbd::new(1, &rom).unwrap();
    while a.now() < 3 * FRAME_DOTS {
        a.step_native();
        if let Some(op) = b.begin() {
            b.exec_native(op);
        }
        assert_eq!(a.regs(), b.regs());
        assert_eq!(a.now(), b.now());
    }
    assert_eq!(a.state_hash(), b.state_hash());
    assert_eq!(a.frame_hash(), b.frame_hash());
}

#[test]
fn registers_round_trip() {
    let mut m = Rlgbd::new(1, &rom()).unwrap();
    let want = RlgbdRegs {
        a: 1,
        f: 0xB0,
        b: 2,
        c: 3,
        d: 4,
        e: 5,
        h: 6,
        l: 7,
        pc: 0x150,
        sp: 0xFFFC,
        ime: 1,
    };
    m.set_regs(&want);
    assert_eq!(m.regs(), want);
}

#[test]
fn advance_moves_the_clock_between_instructions() {
    let mut m = Rlgbd::new(1, &rom()).unwrap();
    let t = m.now() + 1000;
    m.advance(1000);
    assert_eq!(m.now(), t);
}
