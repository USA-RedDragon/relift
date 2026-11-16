//! Selections ahead of a loop that only EXIT leaves still merge, so the
//! code after them is emitted once and not once per path (twelve in a row
//! would otherwise need 4096 copies of the loop).

use relift_gpu_sm86::program::Program;
use relift_gpu_sm86::translate::translate;

fn word(op: u32, form: u32, fields: &[(u32, u32, u64)]) -> [u8; 16] {
    let mut w: u128 =
        u128::from(op) | (u128::from(form) << 9) | (7u128 << 12) | (7u128 << 110) | (7u128 << 113);
    for &(lo, len, v) in fields {
        let m = ((1u128 << len) - 1) << lo;
        w = (w & !m) | ((u128::from(v) << lo) & m);
    }
    w.to_le_bytes()
}

fn bra(at: usize, to: usize, pred: Option<u64>) -> [u8; 16] {
    let rel = (to as i64 * 16 - (at as i64 * 16 + 16)) / 4;
    let mut f = vec![(34, 48, (rel as u64) & 0xffff_ffff_ffff), (87, 3, 7)];
    if let Some(p) = pred {
        f.push((12, 3, p));
    }
    word(0x147, 4, &f)
}

fn iadd(v: u32) -> [u8; 16] {
    word(
        0x010,
        4,
        &[
            (16, 8, 1),
            (24, 8, 1),
            (32, 32, u64::from(v)),
            (64, 8, 255),
            (81, 3, 7),
            (84, 3, 7),
        ],
    )
}

fn isetp() -> [u8; 16] {
    word(
        0x00c,
        1,
        &[
            (24, 8, 1),
            (32, 8, 0),
            (76, 3, 2),
            (81, 3, 0),
            (84, 3, 7),
            (87, 3, 7),
        ],
    )
}

#[test]
fn selections_before_a_loop_left_only_through_exit_merge() {
    let mut b = Vec::new();
    b.extend(isetp());
    for i in 0..12 {
        let at = b.len() / 16;
        b.extend(bra(at, at + 2, Some(0)));
        b.extend(iadd(i + 1));
    }
    let top = b.len() / 16;
    b.extend(iadd(100));
    b.extend(isetp());
    b.extend(word(0x14d, 4, &[(12, 3, 0), (87, 3, 7)]));
    b.extend(bra(top + 3, top, None));
    let end = b.len() / 16;
    b.extend(bra(end, end, None));
    translate(&Program::parse(&b)).unwrap_or_else(|e| panic!("{e}"));
}
