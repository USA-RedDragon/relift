//! Every lifted encoding prints, parses back, and survives dead-value
//! elimination with the same interpreter result.

use relift_cpu_arm64::decode::{Features, decode};
use relift_cpu_arm64::lift::{lift, supported};
use relift_cpu_arm64::regs;
use relift_ir::interp::{self, FlatMemory, NoIntrinsics, State};
use relift_ir::{Block, dce, parse};

const PC: u64 = 0x4000;
const WORDS: usize = 4;

struct Lcg(u64);

impl Lcg {
    fn next(&mut self) -> u32 {
        self.0 = self
            .0
            .wrapping_mul(6364136223846793005)
            .wrapping_add(1442695040888963407);
        (self.0 >> 32) as u32
    }
}

fn mem() -> FlatMemory {
    FlatMemory {
        base: 0x1_0000,
        bytes: (0..=255u8).cycle().take(0x1000).collect(),
    }
}

fn state(seed: u64) -> State {
    let mut rng = Lcg(seed);
    let mut s = State::new(regs::regs().len());
    for (i, d) in regs::regs().decls().iter().enumerate() {
        let v = (u128::from(rng.next()) << 96)
            | (u128::from(rng.next()) << 64)
            | (u128::from(rng.next()) << 32)
            | u128::from(rng.next());
        s.regs[i] = v & d.width.mask();
    }
    for n in 0..31 {
        s.regs[regs::x(n).index()] = 0x1_0000 + u128::from(rng.next() % 0x800);
    }
    s
}

fn exec(b: &Block, seed: u64) -> (Result<interp::Outcome, interp::Error>, State, Vec<u8>) {
    let mut s = state(seed);
    let mut m = mem();
    let r = interp::run(b, &mut s, &mut m, &mut NoIntrinsics);
    (r, s, m.bytes)
}

#[test]
fn print_parse_and_dce_agree_for_every_supported_encoding() {
    let files = [regs::regs()];
    let mut rng = Lcg(0x1234_5678_9abc_def0);
    let mut checked = 0;
    for enc in supported() {
        let mut got = 0;
        for _ in 0..64 {
            if got == WORDS {
                break;
            }
            let w = enc.value() | (rng.next() & !enc.mask());
            if decode(w).map(|i| i.encoding) != Some(enc) {
                continue;
            }
            let Ok(b) = lift(w, PC, &Features::ALL) else {
                continue;
            };
            got += 1;
            checked += 1;
            let text = b.to_string();
            let back = parse(&text, &files).unwrap_or_else(|e| panic!("{w:#010x}: {e}\n{text}"));
            assert_eq!(back, b, "{w:#010x}\n{text}");
            assert_eq!(back.to_string(), text);

            let d = dce(&b);
            d.verify().unwrap_or_else(|e| panic!("{w:#010x}: {e}"));
            assert!(d.insts.len() <= b.insts.len());
            let dt = d.to_string();
            let dback = parse(&dt, &files).unwrap_or_else(|e| panic!("{w:#010x}: {e}\n{dt}"));
            assert_eq!(dback, d);
            assert_eq!(dback.to_string(), dt);

            for seed in 1..=3u64 {
                let (ra, sa, ma) = exec(&b, seed);
                let (rb, sb, mb) = exec(&d, seed);
                assert_eq!(ra.is_ok(), rb.is_ok(), "{w:#010x}\n{text}");
                if ra.is_ok() {
                    assert_eq!(ra, rb, "{w:#010x}\n{text}");
                    assert_eq!(sa, sb, "{w:#010x}\n{text}");
                    assert_eq!(ma, mb, "{w:#010x}\n{text}");
                }
            }
        }
    }
    assert!(checked > 100, "only {checked} words checked");
}
