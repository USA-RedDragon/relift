//! Structured generators: instruction words of one encoding, and input
//! states with register patterns, flag sweeps and mapped memory windows.

use crate::emu::lift;
use relift_cpu_arm64::decode::{Encoding, Features, Insn, decode_for};

use crate::emu::{CODE, CODE_MAP, CODE_SIZE, DATA, Guest, PAUTH_DATA};
use crate::oracle::fill_window;
use std::sync::Mutex;

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
        || n.ends_with("_ldst_pac")
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
    } else if n.ends_with("_ldst_pac") {
        let w = insn.word;
        let imm10 = ((w >> 22) & 1) << 9 | (w >> 12) & 0x1ff;
        Some(sext(imm10, 10) << 3)
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
    if let Some(words) = pauth_prelude(insn, g, r) {
        return words;
    }
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

/// How many states of each pointer authentication scenario were built since
/// the last [`take_pauth_stats`].
static PAUTH_STATS: Mutex<[u64; Scenario::COUNT]> = Mutex::new([0; Scenario::COUNT]);

/// What a pointer authentication state does to the pointer it checks.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Scenario {
    /// Signed with the key and modifier the instruction checks.
    Valid,
    /// Signed, then one bit of the PAC field flipped.
    PacTampered,
    /// Signed, then one address bit flipped.
    AddrTampered,
    /// Signed, then the modifier changed.
    WrongModifier,
    /// Signed with the other key of the same kind.
    WrongKey,
    /// Not signed: canonical and non-canonical values as they come.
    Unsigned,
}

impl Scenario {
    const COUNT: usize = 6;
    const NAMES: [&'static str; Self::COUNT] = [
        "signed",
        "pac-tampered",
        "address-tampered",
        "wrong-modifier",
        "wrong-key",
        "unsigned",
    ];

    fn pick(r: &mut Rng) -> Scenario {
        match r.below(100) {
            0..35 => Scenario::Valid,
            35..50 => Scenario::PacTampered,
            50..60 => Scenario::AddrTampered,
            60..68 => Scenario::WrongModifier,
            68..76 => Scenario::WrongKey,
            _ => Scenario::Unsigned,
        }
    }
}

/// The scenario counts since the last call, as a report fragment, or `None`
/// when no pointer authentication state was built.
pub fn take_pauth_stats() -> Option<String> {
    let mut s = PAUTH_STATS.lock().expect("stats lock");
    let counts = std::mem::take(&mut *s);
    if counts.iter().all(|c| *c == 0) {
        return None;
    }
    let parts: Vec<String> = Scenario::NAMES
        .iter()
        .zip(counts)
        .map(|(n, c)| format!("{c} {n}"))
        .collect();
    Some(parts.join(", "))
}

const VA_MASK: u64 = (1 << 39) - 1;

/// A pointer: canonical in the 39-bit layout (upper bits zero, or all ones),
/// or one of the non-canonical shapes (bit 55 against the upper bits, random
/// upper bits).
fn pauth_pointer(r: &mut Rng, canonical: bool) -> u64 {
    let low = r.next() & VA_MASK;
    let pick = if canonical { r.below(60) } else { r.below(100) };
    match pick {
        0..30 => low,
        30..60 => low | !VA_MASK,
        60..68 => low | 1 << 55,
        68..76 => low | (r.next() & 0xff7f_ff80_0000_0000),
        76..84 => reg_value(r),
        _ => r.next(),
    }
}

/// The register an authentication instruction takes a modifier from.
#[derive(Clone, Copy)]
enum Modifier {
    Zero,
    /// `x0` to `x30`, or SP for 31.
    Reg(u32),
}

/// The pointer register, modifier and key (0 IA, 1 IB, 2 DA, 3 DB) of a
/// pointer authentication instruction, and whether it is one that checks a
/// signature (`AUT*`, `BRA*`, `BLRA*`, `RETA*`, `LDRA*`) or strips one.
struct PauthForm {
    ptr: u32,
    modifier: Modifier,
    key: u32,
    checks: bool,
    branches: bool,
    loads: bool,
    hint: bool,
}

fn pauth_form(insn: &Insn) -> Option<PauthForm> {
    let name = insn.encoding.name();
    let w = insn.word;
    let mnemonic = name.split('_').next()?;
    let b = key_b(mnemonic);
    let mut f = PauthForm {
        ptr: 30,
        modifier: Modifier::Zero,
        key: 0,
        checks: true,
        branches: false,
        loads: false,
        hint: false,
    };
    if name.ends_with("_hints") {
        f.hint = true;
        f.key = u32::from(b);
        if mnemonic.starts_with("XPAC") {
            f.checks = false;
        } else if mnemonic.ends_with("1716") {
            f.ptr = 17;
            f.modifier = Modifier::Reg(16);
        } else if mnemonic.ends_with("SP") {
            f.modifier = Modifier::Reg(31);
        } else if !mnemonic.ends_with('Z') {
            return None;
        }
        return Some(f);
    }
    if name.ends_with("_dp_1src") && !name.starts_with("PACGA") {
        f.ptr = w & 31;
        let data = mnemonic.contains('D');
        f.key = u32::from(data) * 2 + u32::from(b);
        if mnemonic.starts_with("XPAC") {
            f.checks = false;
            f.key = u32::from(data) * 2;
        } else if !name.contains("_64Z_") {
            f.modifier = Modifier::Reg((w >> 5) & 31);
        }
        return Some(f);
    }
    if name.ends_with("_branch_reg")
        && ["BRA", "BLRA", "RETA"]
            .iter()
            .any(|p| mnemonic.starts_with(p))
    {
        f.key = u32::from(b);
        f.branches = true;
        if mnemonic.starts_with("RET") {
            f.modifier = Modifier::Reg(31);
        } else {
            f.ptr = (w >> 5) & 31;
            if !name.contains("_64_") {
                f.modifier = Modifier::Reg(w & 31);
            }
        }
        return Some(f);
    }
    if name.ends_with("_ldst_pac") {
        f.ptr = (w >> 5) & 31;
        f.key = 2 + u32::from(b);
        f.loads = true;
        return Some(f);
    }
    None
}

/// Whether the mnemonic names the B key.
fn key_b(m: &str) -> bool {
    if ["BRAB", "BLRAB", "RETAB", "LDRAB"]
        .iter()
        .any(|p| m.starts_with(p))
    {
        return true;
    }
    let rest = m
        .strip_prefix("PAC")
        .or_else(|| m.strip_prefix("AUT"))
        .unwrap_or("");
    let rest = rest.get(1..).unwrap_or("");
    let rest = rest.strip_prefix('Z').unwrap_or(rest);
    rest.starts_with('B')
}

fn pac_word(f: &PauthForm, insn: &Insn, key: u32) -> u32 {
    if f.hint {
        return 0xd503_201f | (pac_hint(insn, key) << 5);
    }
    match f.modifier {
        Modifier::Reg(m) => 0xdac1_0000 | key << 10 | m << 5 | f.ptr,
        Modifier::Zero => 0xdac1_23e0 | key << 10 | f.ptr,
    }
}

/// The `HINT` number of the `PAC*` instruction that signs for the hint-space
/// instruction `insn` with `key`.
fn pac_hint(insn: &Insn, key: u32) -> u32 {
    let m = insn.encoding.name();
    let base = if m.contains("1716") {
        8
    } else if m.contains("SP") {
        25
    } else {
        24
    };
    base + 2 * key
}

fn eor_bit(reg: u32, bit: u32) -> u32 {
    0xd240_0000 | ((64 - bit) & 63) << 16 | reg << 5 | reg
}

/// The setup of a pointer authentication instruction: the pointer register
/// (and the modifier) set to values that exercise the interesting paths, and
/// the words that sign the pointer, with the key and modifier of the
/// instruction or others, and tamper with it; unsigned on a CPU whose
/// signing words do not lift. `None` for any other form.
fn pauth_prelude(insn: &Insn, g: &mut Guest, r: &mut Rng) -> Option<Vec<u32>> {
    let name = insn.encoding.name();
    let is = ["PAC", "AUT", "XPAC", "BRA", "BLRA", "RETA", "LDRA"]
        .iter()
        .any(|p| name.starts_with(p));
    if !is {
        return None;
    }
    let Some(f) = pauth_form(insn) else {
        if name.starts_with("PACGA") && insn.get("Rn") != 31 {
            g.x[insn.get("Rn") as usize] = pauth_pointer(r, false);
        }
        return Some(Vec::new());
    };
    let align = if f.branches { !3u64 } else { !0u64 };
    let signer = name.starts_with("PAC");
    let scenario = if signer || lift(pac_word(&f, insn, f.key), CODE).is_err() {
        Scenario::Unsigned
    } else {
        Scenario::pick(r)
    };
    let signs = !signer;
    let set = |g: &mut Guest, reg: u32, v: u64| {
        if reg == 31 {
            g.sp = v & !15;
        } else {
            g.x[reg as usize] = v;
        }
    };
    if let Modifier::Reg(m) = f.modifier {
        let v = reg_value(r);
        set(g, m, v);
    }
    let ptr_is_sp = f.ptr == 31 && (f.loads || f.branches);
    if f.ptr == 31 && !ptr_is_sp {
        PAUTH_STATS.lock().expect("stats lock")[Scenario::Unsigned as usize] += 1;
        return Some(Vec::new());
    }
    let canonical = scenario != Scenario::Unsigned || signer;
    let mut p = pauth_pointer(r, canonical) & align;
    if f.loads {
        // Unicorn's LDRAA and LDRAB take SP as the modifier where the
        // architecture says zero: the two agree with SP zero.
        if f.ptr != 31 {
            g.sp = 0;
        }
        let base = if f.ptr == 31 {
            g.sp
        } else {
            g.x[f.ptr as usize]
        };
        let alias = base.wrapping_sub(DATA).wrapping_add(PAUTH_DATA);
        if scenario != Scenario::Unsigned {
            p = alias;
        }
    }
    if f.ptr == 31 {
        g.sp = p & !15;
        PAUTH_STATS.lock().expect("stats lock")[Scenario::Unsigned as usize] += 1;
        return Some(Vec::new());
    }
    g.x[f.ptr as usize] = p;
    let mut words = Vec::new();
    let mut counted = scenario;
    if signs && scenario != Scenario::Unsigned {
        let key = if scenario == Scenario::WrongKey {
            f.key ^ 1
        } else {
            f.key
        };
        words.push(pac_word(&f, insn, key));
        match scenario {
            Scenario::PacTampered => {
                let bits: Vec<u32> = (39..=54).chain(56..64).collect();
                words.push(eor_bit(f.ptr, r.pick(&bits)));
            }
            Scenario::AddrTampered => {
                let lo = if f.branches { 2 } else { 0 };
                words.push(eor_bit(f.ptr, lo + r.below(39 - u64::from(lo)) as u32));
            }
            Scenario::WrongModifier => match f.modifier {
                Modifier::Reg(31) => words.push(0xd100_43ff),
                Modifier::Reg(m) if m != f.ptr => words.push(eor_bit(m, 0)),
                _ => counted = Scenario::Valid,
            },
            _ => {}
        }
    }
    PAUTH_STATS.lock().expect("stats lock")[counted as usize] += 1;
    Some(words)
}
