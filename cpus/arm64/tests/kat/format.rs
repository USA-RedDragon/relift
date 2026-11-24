//! The A64 known-answer file format, shared by the recorder
//! (`check/src/kat.rs`) and the two replays (`cpus/arm64/tests/kat.rs`,
//! `relift/tests/emit_a64_kat.rs`).
//!
//! One case per line:
//!
//! ```text
//! src=<where> w=<hex>[,<hex>...] <input fields> -> <output fields>
//! ```
//!
//! `src` names the machine the answers came from (`thor` is the AYN Thor's
//! Cortex-X3 cluster read over adb; the recorder refuses to write any
//! other). `w` is the word sequence run from the words' address (`CODE`); the last word is the
//! instruction under test and the earlier ones are setup (exclusive
//! monitors, pointer signing).
//!
//! Input fields: `x<n>=<hex>`, `v<n>=<hex128>`, `sp=<hex>`, `nzcv=<hex
//! nibble>`, `fpcr=<hex>`, `fpsr=<hex>` and `seed=<hex>`, the data window's
//! fill. A register left out holds its canary ([`canary_x`], [`canary_v`]);
//! the stack pointer defaults to [`DATA`] + 0x2000.
//!
//! Output fields: `fault=<signal>` when the run faulted (nothing else is
//! compared), else only what changed: registers as in the input,
//! `nzcv=`, `fpsr=` and `pc=<hex>` (the offset from the words' address (`CODE`)) and `m<hex
//! offset>=<hex bytes>` runs of the data window. `mask.x<n>=<hex>`,
//! `mask.v<n>=<hex>` and `mask.sp` name bits that differ from one run of the
//! machine to the next (the pointer authentication code field, whose keys
//! the kernel draws per process): only the other bits are compared.

use std::collections::BTreeMap;

/// The data window.
pub const DATA: u64 = 0x74_2000_0000;
/// The data window's size.
pub const DATA_SIZE: usize = 0x4000;

/// The Thor's CPU (Cortex-X3, A715, A710 and A510 clusters: the Snapdragon
/// 8 Gen 2), from its ID registers: `ID_AA64ISAR0_EL1` 0x0021111110212120,
/// `ISAR1` 0x0010101101211052, `PFR0` 0x0001000000110011, `PFR1` 1,
/// `MMFR2` 0x100000000. No SVE (`ZFR0` is absent), MTE, MOPS, CSSC, RNG,
/// LOR, LRCPC3, SPECRES, WFxT or FP8. FPAC and FPACCOMBINE are left out
/// too (the Thor has them): the authenticating templates refuse a CPU with
/// FPAC, so a failed authentication lifts as the poisoned pointer and the
/// Thor's fault is a named gap in the replays. BTI is left out of the set the lifter
/// is given: the Thor has it, but the harness's pages are not guarded and
/// the templates refuse a CPU with a feature they do not model.
pub const THOR_FEATURES: &str = "FP,AdvSIMD,AES,PMULL,SHA1,SHA256,SHA512,SHA3,SM3,SM4,CRC32,LSE,RDM,DotProd,FHM,FlagM,FlagM2,FP16,JSCVT,FCMA,LRCPC,LRCPC2,PAuth,PAuth2,PACQARMA5,FRINTTS,SB,BF16,I8MM,DPB,DPB2,DIT,LSE2";

/// The value of `x<n>` when a case does not name it.
pub fn canary_x(n: usize) -> u64 {
    0xC0DE_0000_0000_0000 | ((n as u64 + 1) * 0x0101_0101_0101)
}

/// The value of `v<n>` when a case does not name it.
pub fn canary_v(n: usize) -> u128 {
    (u128::from(canary_x(n + 40)) << 64) | u128::from(canary_x(n + 80))
}

/// The data window's fill from a seed: splitmix64, eight bytes a step, as
/// `check/oracle/proto.h`'s `proto_fill`.
pub fn fill_window(seed: u64) -> Vec<u8> {
    let mut s = seed;
    let mut out = Vec::with_capacity(DATA_SIZE);
    while out.len() < DATA_SIZE {
        s = s.wrapping_add(0x9e37_79b9_7f4a_7c15);
        let mut z = s;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        z ^= z >> 31;
        out.extend_from_slice(&z.to_le_bytes());
    }
    out
}

/// A full machine state.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct State {
    /// `x0` to `x30`.
    pub x: [u64; 31],
    /// The stack pointer.
    pub sp: u64,
    /// `v0` to `v31`.
    pub v: [u128; 32],
    /// N, Z, C and V in bits 3 to 0.
    pub nzcv: u8,
    /// The floating-point control register.
    pub fpcr: u32,
    /// The floating-point status register.
    pub fpsr: u32,
    /// The data window.
    pub mem: Vec<u8>,
    /// The offset of the program counter from the words' address (`CODE`) after the run.
    pub pc: u64,
}

impl State {
    /// The state a case's input fields name, from the canaries.
    pub fn base() -> State {
        State {
            x: std::array::from_fn(canary_x),
            sp: DATA + 0x2000,
            v: std::array::from_fn(canary_v),
            nzcv: 0,
            fpcr: 0,
            fpsr: 0,
            mem: Vec::new(),
            pc: 0,
        }
    }
}

/// What a run gave.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Result {
    /// Faulted with this signal.
    Fault(u32),
    /// Ran to the end, in this state.
    Done(Box<State>),
}

/// One known answer.
#[derive(Clone, Debug)]
pub struct Case {
    /// The machine the answer came from.
    pub src: String,
    /// The words run from the words' address (`CODE`).
    pub words: Vec<u32>,
    /// The input state.
    pub input: State,
    /// The seed the input's data window was filled from.
    pub seed: u64,
    /// The answer.
    pub out: Result,
    /// Bits not to compare, by register name (`x3`, `v7`, `sp`).
    pub mask: BTreeMap<String, u128>,
}

fn reg_names() -> impl Iterator<Item = String> {
    (0..31)
        .map(|i| format!("x{i}"))
        .chain(std::iter::once("sp".to_string()))
        .chain((0..32).map(|i| format!("v{i}")))
}

fn put_state(s: &mut String, st: &State, base: &State, full: bool) {
    use std::fmt::Write as _;
    for i in 0..31 {
        if full || st.x[i] != base.x[i] {
            let _ = write!(s, " x{i}={:x}", st.x[i]);
        }
    }
    if full || st.sp != base.sp {
        let _ = write!(s, " sp={:x}", st.sp);
    }
    for i in 0..32 {
        if full || st.v[i] != base.v[i] {
            let _ = write!(s, " v{i}={:x}", st.v[i]);
        }
    }
    if st.nzcv != base.nzcv {
        let _ = write!(s, " nzcv={:x}", st.nzcv);
    }
    if st.fpcr != base.fpcr {
        let _ = write!(s, " fpcr={:x}", st.fpcr);
    }
    if st.fpsr != base.fpsr {
        let _ = write!(s, " fpsr={:x}", st.fpsr);
    }
}

/// The line of a case.
pub fn format(c: &Case) -> String {
    use std::fmt::Write as _;
    let mut s = format!("src={} w=", c.src);
    for (i, w) in c.words.iter().enumerate() {
        if i > 0 {
            s.push(',');
        }
        let _ = write!(s, "{w:08x}");
    }
    let base = State::base();
    put_state(&mut s, &c.input, &base, false);
    let _ = write!(s, " seed={:x} ->", c.seed);
    match &c.out {
        Result::Fault(sig) => {
            let _ = write!(s, " fault={sig}");
        }
        Result::Done(o) => {
            put_state(&mut s, o, &c.input, false);
            if o.pc != 4 * c.words.len() as u64 {
                let _ = write!(s, " pc={:x}", o.pc);
            }
            let seed_mem = fill_window(c.seed);
            let mut i = 0;
            while i < DATA_SIZE {
                if o.mem[i] == seed_mem[i] {
                    i += 1;
                    continue;
                }
                let mut j = i;
                while j < DATA_SIZE && o.mem[j] != seed_mem[j] {
                    j += 1;
                }
                let _ = write!(s, " m{i:x}=");
                for b in &o.mem[i..j] {
                    let _ = write!(s, "{b:02x}");
                }
                i = j;
            }
        }
    }
    for (r, m) in &c.mask {
        let _ = write!(s, " mask.{r}={m:x}");
    }
    s
}

fn hex(v: &str) -> u128 {
    u128::from_str_radix(v, 16).unwrap_or_else(|e| panic!("bad hex {v}: {e}"))
}

fn set_reg(st: &mut State, name: &str, v: u128) -> bool {
    if let Some(n) = name.strip_prefix('x').and_then(|n| n.parse::<usize>().ok()) {
        st.x[n] = v as u64;
    } else if name == "sp" {
        st.sp = v as u64;
    } else if let Some(n) = name.strip_prefix('v').and_then(|n| n.parse::<usize>().ok()) {
        st.v[n] = v;
    } else {
        return false;
    }
    true
}

/// Reads a case from its line.
pub fn parse(line: &str) -> Case {
    let (i, o) = line
        .split_once(" -> ")
        .unwrap_or_else(|| panic!("no arrow: {line}"));
    let mut input = State::base();
    let (mut src, mut words, mut seed) = (String::new(), Vec::new(), 0u64);
    for t in i.split(' ') {
        let (k, v) = t.split_once('=').unwrap_or_else(|| panic!("bad field {t}"));
        match k {
            "src" => src = v.to_string(),
            "w" => words = v.split(',').map(|w| hex(w) as u32).collect(),
            "seed" => seed = hex(v) as u64,
            "nzcv" => input.nzcv = hex(v) as u8,
            "fpcr" => input.fpcr = hex(v) as u32,
            "fpsr" => input.fpsr = hex(v) as u32,
            _ => assert!(set_reg(&mut input, k, hex(v)), "bad input field {t}"),
        }
    }
    input.mem = fill_window(seed);
    let mut mask = BTreeMap::new();
    let mut out = input.clone();
    out.pc = 4 * words.len() as u64;
    let mut fault = None;
    for t in o.split(' ').filter(|t| !t.is_empty()) {
        let (k, v) = t.split_once('=').unwrap_or_else(|| panic!("bad field {t}"));
        if let Some(r) = k.strip_prefix("mask.") {
            mask.insert(r.to_string(), hex(v));
        } else if k == "fault" {
            fault = Some(v.parse().expect("signal"));
        } else if k == "nzcv" {
            out.nzcv = hex(v) as u8;
        } else if k == "fpcr" {
            out.fpcr = hex(v) as u32;
        } else if k == "fpsr" {
            out.fpsr = hex(v) as u32;
        } else if k == "pc" {
            out.pc = hex(v) as u64;
        } else if let Some(off) = k.strip_prefix('m') {
            let off = hex(off) as usize;
            for (j, b) in (0..v.len()).step_by(2).enumerate() {
                out.mem[off + j] = hex(&v[b..b + 2]) as u8;
            }
        } else {
            assert!(set_reg(&mut out, k, hex(v)), "bad output field {t}");
        }
    }
    debug_assert!(reg_names().count() == 64);
    Case {
        src,
        words,
        input,
        seed,
        out: match fault {
            Some(s) => Result::Fault(s),
            None => Result::Done(Box::new(out)),
        },
        mask,
    }
}

/// The differences between a result and the answer, empty when they agree
/// (two faults agree). `fpsr` selects whether FPSR is compared.
pub fn diff(got: &Result, want: &Case, fpsr: bool) -> Vec<String> {
    let (g, w) = match (got, &want.out) {
        (Result::Fault(_), Result::Fault(_)) => return Vec::new(),
        (Result::Fault(s), Result::Done(_)) => {
            return vec![format!("faulted ({s}), the CPU did not")];
        }
        (Result::Done(_), Result::Fault(s)) => return vec![format!("ran, the CPU faulted ({s})")],
        (Result::Done(g), Result::Done(w)) => (g, w),
    };
    let mut d = Vec::new();
    let m = |r: &str| want.mask.get(r).copied().unwrap_or(0);
    for i in 0..31 {
        let r = format!("x{i}");
        if (g.x[i] ^ w.x[i]) & !(m(&r) as u64) != 0 {
            d.push(format!("{r} got={:#x} want={:#x}", g.x[i], w.x[i]));
        }
    }
    if (g.sp ^ w.sp) & !(m("sp") as u64) != 0 {
        d.push(format!("sp got={:#x} want={:#x}", g.sp, w.sp));
    }
    for i in 0..32 {
        let r = format!("v{i}");
        if (g.v[i] ^ w.v[i]) & !m(&r) != 0 {
            d.push(format!("{r} got={:#x} want={:#x}", g.v[i], w.v[i]));
        }
    }
    if g.nzcv != w.nzcv {
        d.push(format!("nzcv got={:x} want={:x}", g.nzcv, w.nzcv));
    }
    if fpsr && g.fpsr != w.fpsr {
        d.push(format!("fpsr got={:#x} want={:#x}", g.fpsr, w.fpsr));
    }
    if g.pc != w.pc {
        d.push(format!("pc got={:#x} want={:#x}", g.pc, w.pc));
    }
    if let Some(o) = (0..DATA_SIZE).find(|i| g.mem[*i] != w.mem[*i]) {
        d.push(format!(
            "mem+{o:#x} got={:02x} want={:02x}",
            g.mem[o], w.mem[o]
        ));
    }
    d
}
