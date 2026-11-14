//! The emitter's IR passes keep every lifted form's meaning: random words of
//! every form in `gen/forms.txt`, run in the reference interpreter on random
//! states before and after `simplify` and `dce`, give the same outcome,
//! registers, monitor and memory, faults included.

use std::collections::BTreeMap;

use relift_cpu_a64::decode::{Encoding, decode};
use relift_cpu_a64::lift::lift;
use relift_ir::interp::{FlatMemory, Monitor, NoIntrinsics, State, run};
use relift_ir::{Block, dce, simplify};

const BASE: u64 = 0x10_0000;
const WINDOW: usize = 0x2000;

struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.0 = x;
        x.wrapping_mul(0x2545_f491_4f6c_dd1d)
    }

    fn below(&mut self, n: u64) -> u64 {
        self.next() % n
    }
}

fn value(r: &mut Rng, width: u32) -> u128 {
    let v = match r.below(4) {
        0 => u128::from([0u64, 1, 0x7fff_ffff, 0x8000_0000, u64::MAX][r.below(5) as usize]),
        1 => u128::from(BASE + 0x800 + (r.below(0x1000) & !15)),
        _ => (u128::from(r.next()) << 64) | u128::from(r.next()),
    };
    if width >= 128 {
        v
    } else {
        v & ((1u128 << width) - 1)
    }
}

fn state(b: &Block, word: u32, r: &mut Rng) -> State {
    let mut s = State::new(b.regs.len());
    for (i, d) in b.regs.decls().iter().enumerate() {
        s.regs[i] = value(r, d.width.bits());
    }
    // FPCR zero, so the FP templates' guards do not leave.
    if let Some(fpcr) = b.regs.find("fpcr") {
        s.regs[fpcr.index()] = 0;
    }
    let rn = ((word >> 5) & 31) as usize;
    if r.below(4) != 0 {
        s.regs[rn] = u128::from(BASE + 0x800 + (r.below(0x1000) & !15));
    }
    if r.below(2) == 0 {
        s.monitor = Some(Monitor {
            addr: s.regs[rn] as u64,
            size: [1, 2, 4, 8, 16][r.below(5) as usize],
        });
    }
    s
}

#[test]
fn simplify_and_dce_keep_every_form_s_meaning() {
    let forms = include_str!("../gen/forms.txt");
    let by_name: BTreeMap<&str, Encoding> = Encoding::all().map(|e| (e.name(), e)).collect();
    let mut r = Rng(0x0e0e_5eed);
    let (mut words, mut runs, mut removed, mut total) = (0, 0, 0usize, 0usize);
    for name in forms.lines().map(str::trim).filter(|l| !l.is_empty()) {
        let enc = by_name[name];
        let mut got = 0;
        for _ in 0..400 {
            if got == 4 {
                break;
            }
            let w = enc.value() | (r.next() as u32 & !enc.mask());
            if decode(w).map(|i| i.encoding) != Some(enc) {
                continue;
            }
            let Ok(b) = lift(w, 0x7100_0000) else {
                continue;
            };
            got += 1;
            words += 1;
            let o = dce(&simplify(&b));
            o.verify()
                .unwrap_or_else(|e| panic!("{w:08x} ({name}): {e}"));
            total += b.insts.len();
            removed += b.insts.len() - o.insts.len();
            for _ in 0..16 {
                let s0 = state(&b, w, &mut r);
                let seed = r.next();
                let mut m = Rng(seed | 1);
                let bytes: Vec<u8> = (0..WINDOW).map(|_| m.next() as u8).collect();
                let (mut s1, mut s2) = (s0.clone(), s0.clone());
                let mut m1 = FlatMemory {
                    base: BASE,
                    bytes: bytes.clone(),
                };
                let mut m2 = m1.clone();
                let a = run(&b, &mut s1, &mut m1, &mut NoIntrinsics);
                let c = run(&o, &mut s2, &mut m2, &mut NoIntrinsics);
                assert_eq!(a, c, "{w:08x} ({name}) outcome");
                assert_eq!(s1, s2, "{w:08x} ({name}) state");
                assert!(m1 == m2, "{w:08x} ({name}) memory");
                runs += 1;
            }
        }
    }
    eprintln!("{words} words, {runs} runs, {removed} of {total} instructions removed");
    assert!(
        removed * 10 > total,
        "the passes remove little: {removed} of {total}"
    );
}
