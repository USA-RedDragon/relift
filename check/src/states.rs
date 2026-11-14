//! Structured generators: instruction words of one encoding, and input
//! states with register patterns, flag sweeps and mapped memory windows.

use crate::emu::lift;
use relift_cpu_a64::decode::{Encoding, Features, Insn, decode_for};

use crate::emu::{CODE, CODE_MAP, CODE_SIZE, DATA, Guest};
use crate::oracle::fill_window;

/// FPSR's cumulative bits: QC (27), IDC (7), IXC, UFC, OFC, DZC and IOC
/// (4 to 0).
pub const FPSR_CUMULATIVE: u32 = 0x0800_009f;

/// A small xorshift generator, seeded from a string and a number.
pub struct Rng(u64);

impl Rng {
    /// A generator seeded from `label` and `seed`.
    pub fn new(label: &str, seed: u64) -> Rng {
        let mut h = 0xcbf2_9ce4_8422_2325u64 ^ seed.wrapping_mul(0x9e37_79b9_7f4a_7c15);
        for b in label.bytes() {
            h = (h ^ u64::from(b)).wrapping_mul(0x0100_0000_01b3);
        }
        Rng(h | 1)
    }

    /// The next 64 random bits.
    pub fn next(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.0 = x;
        x.wrapping_mul(0x2545_f491_4f6c_dd1d)
    }

    /// A number below `n`.
    pub fn below(&mut self, n: u64) -> u64 {
        self.next() % n
    }

    /// A number in `lo..hi`.
    pub fn range(&mut self, lo: i64, hi: i64) -> i64 {
        lo + self.below((hi - lo) as u64) as i64
    }

    /// True with probability `p`.
    pub fn chance(&mut self, p: f64) -> bool {
        (self.next() >> 11) as f64 / (1u64 << 53) as f64 <= p
    }

    /// One of `xs`.
    pub fn pick<T: Copy>(&mut self, xs: &[T]) -> T {
        xs[self.below(xs.len() as u64) as usize]
    }
}

const SPECIAL: [u64; 17] = [
    0,
    1,
    2,
    0x7f,
    0x80,
    0xff,
    0x7fff,
    0x8000,
    0xffff,
    0x7fff_ffff,
    0x8000_0000,
    0xffff_ffff,
    0x1_0000_0000,
    0x7fff_ffff_ffff_ffff,
    0x8000_0000_0000_0000,
    u64::MAX,
    u64::MAX - 1,
];

/// A register value: boundary values, small and negative numbers, 32-bit
/// patterns with and without sign-extended highs, and random bits.
pub fn reg_value(r: &mut Rng) -> u64 {
    let p = r.below(100);
    match p {
        0..15 => r.pick(&SPECIAL),
        15..25 => r.below(64),
        25..35 => (-(r.range(1, 64))) as u64,
        35..45 => r.next() & 0xffff_ffff,
        45..50 => {
            if r.chance(0.5) {
                r.next() | 0xffff_ffff_0000_0000
            } else {
                r.next() & 0x7fff_ffff
            }
        }
        _ => r.next(),
    }
}

/// Double-precision values the FP rules treat specially: zeros, infinities,
/// quiet and signaling NaNs with payloads, subnormals, ones and extremes.
const F64_SPECIAL: [u64; 14] = [
    0,
    0x8000_0000_0000_0000,
    0x7ff0_0000_0000_0000,
    0xfff0_0000_0000_0000,
    0x7ff8_0000_0000_0000,
    0xfff8_0000_dead_beef,
    0x7ff0_0000_0000_0001,
    0xfff4_0000_0000_0000,
    0x0000_0000_0000_0001,
    0x800f_ffff_ffff_ffff,
    0x3ff0_0000_0000_0000,
    0xbff0_0000_0000_0000,
    0x7fef_ffff_ffff_ffff,
    0x4330_0000_0000_0000,
];

/// The single-precision counterparts of [`F64_SPECIAL`].
const F32_SPECIAL: [u32; 14] = [
    0,
    0x8000_0000,
    0x7f80_0000,
    0xff80_0000,
    0x7fc0_0000,
    0xffc0_beef,
    0x7f80_0001,
    0xffa0_0000,
    0x0000_0001,
    0x807f_ffff,
    0x3f80_0000,
    0xbf80_0000,
    0x7f7f_ffff,
    0x4b00_0000,
];

/// A random 64 bits of a vector register: as [`reg_value`], or one or two
/// special floating-point values.
fn vec_half(r: &mut Rng) -> u64 {
    match r.below(5) {
        // Integer lanes at the most negative value of some element size,
        // where saturating absolute value and negation set QC.
        4 => {
            let p = r.pick(&[
                0x8080_8080_8080_8080u64,
                0x8000_8000_8000_8000,
                0x8000_0000_8000_0000,
                0x8000_0000_0000_0000,
            ]);
            // All lanes, or some.
            if r.chance(0.5) {
                p
            } else {
                p & (r.next() | r.next())
            }
        }
        0 => r.pick(&F64_SPECIAL),
        1 => {
            let lane = |r: &mut Rng| {
                if r.chance(0.6) {
                    u64::from(r.pick(&F32_SPECIAL))
                } else {
                    r.next() & 0xffff_ffff
                }
            };
            (lane(r) << 32) | lane(r)
        }
        _ => reg_value(r),
    }
}

/// The features words are decoded for: the oracle's, or every feature
/// under `--undef` (whose words exist only on newer CPUs).
pub fn decode_features() -> Features {
    if crate::emu::UNDEF.load(std::sync::atomic::Ordering::Relaxed) {
        Features::ALL
    } else {
        crate::emu::features()
    }
}

/// Decodes `w` for [`decode_features`].
pub fn decode(w: u32) -> Option<Insn> {
    decode_for(w, &decode_features())
}

/// A random word of `enc` whose don't-care bits are random, that decodes to
/// `enc` and lifts (under `--undef`: that the oracle's CPU does not
/// implement), or `None` after many tries.
pub fn word_of(enc: Encoding, r: &mut Rng) -> Option<u32> {
    let undef = crate::emu::UNDEF.load(std::sync::atomic::Ordering::Relaxed);
    for _ in 0..1000 {
        let w = enc.value() | (r.next() as u32 & !enc.mask());
        if decode(w).map(|i| i.encoding) != Some(enc) {
            continue;
        }
        let ok = if undef {
            matches!(crate::emu::lift_plain(w, CODE), Err(e) if crate::emu::is_undefined(&e))
        } else {
            lift(w, CODE).is_ok()
        };
        if ok {
            return Some(w);
        }
    }
    None
}

fn sext(v: u32, bits: u32) -> i64 {
    let s = 64 - bits;
    ((u64::from(v) << s) as i64) >> s
}

/// The guest-memory access kind of an encoding, for placing base registers.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    /// No memory access and no register branch.
    Plain,
    /// A load or store through `Rn`; exclusive and ordered forms need alignment.
    Mem {
        /// The base needs 8-byte alignment.
        aligned: bool,
    },
    /// A branch to `Rn`.
    BranchReg,
}

/// The kind of `enc`.
pub fn kind(enc: Encoding) -> Kind {
    let n = enc.name();
    // Ordered unscaled forms (`_ldapstl`) are aligned too: hardware faults
    // an unaligned one that crosses 16 bytes, which Unicorn and the lifted
    // code do not model.
    if n.contains("_ldstexcl")
        || n.contains("_ldstord")
        || n.ends_with("_memop")
        || n.contains("_ldapstl")
    {
        Kind::Mem { aligned: true }
    } else if n.contains("_ldst") || n.contains("_asisdls") {
        Kind::Mem { aligned: false }
    } else if n.ends_with("_branch_reg") {
        Kind::BranchReg
    } else {
        Kind::Plain
    }
}

/// The log2 access size of a SIMD and FP load or store (`LDR_Q_ldst_pos`),
/// `None` for the general-register forms.
fn simd_scale(name: &str) -> Option<u32> {
    let kind = name.split('_').nth(1)?;
    Some(match kind {
        "B" | "BL" => 0,
        "H" => 1,
        "S" => 2,
        "D" => 3,
        "Q" => 4,
        _ => return None,
    })
}

/// The offset an `Rn`-based access adds to the base, from the fields, after
/// placing any index register in `g`; `None` when the word cannot be placed.
fn mem_offset(insn: &Insn, g: &mut Guest, r: &mut Rng) -> Option<i64> {
    let n = insn.encoding.name();
    let f = |s: &str| insn.get(s);
    let scale = simd_scale(n);
    let size = || scale.unwrap_or_else(|| f("size"));
    if n.ends_with("_ldst_pos") {
        Some(i64::from(f("imm12")) << size())
    } else if n.ends_with("_ldst_immpre")
        || n.ends_with("_ldst_unscaled")
        || n.ends_with("_ldapstl_unscaled")
    {
        Some(sext(f("imm9"), 9))
    } else if n.ends_with("_ldstpair_off") || n.ends_with("_ldstpair_pre") {
        let pair = match scale {
            Some(_) => 2 + f("opc"),
            None => 2 + (f("opc") >> 1),
        };
        Some(sext(f("imm7"), 7) << pair)
    } else if n.ends_with("_ldst_regoff") {
        let m = f("Rm");
        if m == f("Rn") {
            return None;
        }
        let small = r.range(-40, 40);
        let wide = f("option") & 1 == 1;
        let v = if m == 31 {
            0
        } else if wide {
            small as u64
        } else {
            (small as u64 & 0xffff_ffff) | (r.next() << 32)
        };
        if m != 31 {
            g.x[m as usize] = v;
        }
        let ext = match f("option") {
            0b010 => i64::from(v as u32),
            0b110 => i64::from(v as u32 as i32),
            _ => v as i64,
        };
        let shift = if f("S") == 1 { size() } else { 0 };
        Some(ext << shift)
    } else {
        Some(0)
    }
}

/// A prelude for exclusive stores: `clrex`, then half the time a
/// load-acquire exclusive (`ldaxr`) of the same size, through the same
/// address or one outside its reservation granule.
pub fn prelude(insn: &Insn, g: &mut Guest, r: &mut Rng) -> Vec<u32> {
    const CLREX: u32 = 0xd503_3f5f;
    let name = insn.encoding.name();
    if !name.contains("_ldstexclr") {
        return Vec::new();
    }
    let mut words = vec![CLREX];
    if (name.starts_with("STXR") || name.starts_with("STLXR")) && r.chance(0.5) {
        {
            let (s, t, n) = (insn.get("Rs"), insn.get("Rt"), insn.get("Rn"));
            // x18 is left out: an oracle may zero it (see `X18_ZERO`).
            let used = [s, t, n, 18, 31];
            let free: Vec<u32> = (0..31).filter(|x| !used.contains(x)).collect();
            let p = r.pick(&free);
            let q = r.pick(&free.iter().copied().filter(|x| *x != p).collect::<Vec<_>>());
            let base = if n == 31 { g.sp } else { g.x[n as usize] };
            // The other address is more than 2 KiB away, beyond the largest
            // exclusives reservation granule, where hardware may let a store
            // to another address in the granule succeed.
            let far = if base.wrapping_sub(DATA) < 0x2000 {
                base.wrapping_add(0x1000 + 8 * r.range(0, 8) as u64)
            } else {
                base.wrapping_sub(0x1000 + 8 * r.range(0, 8) as u64)
            };
            g.x[p as usize] = if r.chance(0.5) { base } else { far };
            let size = insn.get("size");
            words.push((size << 30) | 0x085f_fc00 | (p << 5) | q);
        }
    }
    words
}

/// A random input state for `insn`, or `None` when the word cannot be
/// placed (an index register that is also the base).
pub fn state(insn: &Insn, r: &mut Rng) -> Option<Guest> {
    let mut g = Guest {
        x: [0; 31],
        sp: DATA + 0x2000 - 16 * r.below(0x40),
        nzcv: [false; 4],
        v: [0; 32],
        fpcr: 0,
        fpsr: 0,
        pc: 0,
        mem: Vec::new(),
        mem_seed: None,
    };
    let seed = r.next();
    g.mem = fill_window(seed);
    g.mem_seed = Some(seed);
    // Half the states start with cumulative flags already set, so a form
    // that clears a flag it should only set is caught.
    if r.chance(0.5) {
        g.fpsr = r.next() as u32 & FPSR_CUMULATIVE;
    }
    for x in &mut g.x {
        *x = reg_value(r);
    }
    if crate::emu::X18_ZERO.load(std::sync::atomic::Ordering::Relaxed) {
        g.x[18] = 0;
    }
    for v in &mut g.v {
        *v = (u128::from(vec_half(r)) << 64) | u128::from(vec_half(r));
    }
    for f in &mut g.nzcv {
        *f = r.chance(0.5);
    }
    match kind(insn.encoding) {
        Kind::Plain => {}
        Kind::BranchReg => {
            let n = insn.get("Rn");
            if n != 31 {
                g.x[n as usize] = if r.chance(0.7) {
                    CODE_MAP + 4 * r.below(CODE_SIZE / 4)
                } else {
                    r.next() & 0x0000_ffff_ffff_fffc
                };
            }
        }
        Kind::Mem { aligned } => {
            let off = mem_offset(insn, &mut g, r)?;
            let mut target = DATA as i64 + 0x2000 + r.range(-0x800, 0x800);
            if aligned {
                let pair = insn.encoding.name().contains("_ldstexclp");
                target &= if pair { !15 } else { !7 };
            }
            let n = insn.get("Rn");
            let base = (target - off) as u64;
            if n == 31 {
                if aligned && base & 15 != 0 {
                    return None;
                }
                g.sp = base & !15;
            } else {
                g.x[n as usize] = base;
            }
        }
    }
    // A base or index register placed in x18 cannot reach an oracle that
    // zeroes x18.
    if crate::emu::X18_ZERO.load(std::sync::atomic::Ordering::Relaxed) && g.x[18] != 0 {
        return None;
    }
    Some(g)
}
