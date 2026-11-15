//! `relift-cpu-arm64-gen oracle`: the sail-arm model as a `relift-check`
//! oracle (the child-process protocol of `check/src/oracle.rs`), for the
//! pointer authentication forms.
//!
//! Each request is one instruction word, run once in the evaluator with
//! every word bit known, so every value is concrete: the word goes through
//! the Sail top-level decode clause that takes it (with the `Error_See`
//! retry), whose decode function calls the execute clause. Nothing of the
//! instruction's semantics is cut; `ComputePACQARMA`, `AddPAC`, `Auth`,
//! `Strip`, `CalculateBottomPACBit` and `AArch64_BranchAddr` all run from
//! the Sail sources. The registers come from the request: `R0`-`R30`,
//! `SP_EL0`, `_PC` and PSTATE (EL0, `SP` 0, NZCV); the key registers hold
//! `CHECK_PAC_KEYS`; every other register is the generator's fixed
//! configuration (`reg_config`). `IsFeatureImplemented` answers for the
//! Cortex-A78C, and `Mem_read` and `Mem_set` access the data window, a
//! fault outside it. The state after is read back from the registers: the
//! next PC is `_PC` when `__BranchTaken`, else the word's address plus 4.

use std::cell::Cell;
use std::io::{BufReader, BufWriter, Read, Write};
use std::rc::Rc;

use relift_cpu_arm64::decode::{Feature, Features, decode_for};
use relift_cpu_arm64::regs::{CHECK_PAC_KEYS, PacKey};
use relift_sail::eval::{Cpu, E, Gen, R, err, fail};
use relift_sail::model::Model;
use relift_sail::value::V;

use crate::cut::Arm64;

/// Where the word runs, as `check/src/emu.rs` maps it.
const CODE: u64 = 0x100_1000_0000;
/// The data window.
const DATA: u64 = 0x100_2000_0000;
/// Its size.
const DATA_SIZE: usize = 0x4000;

const REQ_MAGIC: u32 = 0x514f_4c52;
const RES_MAGIC: u32 = 0x524f_4c52;
const STATE_SIZE: usize = 800;

/// The CPU the oracle models.
const CPU: Features = Features::CORTEX_A78C;

/// The encodings the oracle runs: FEAT_PAuth and FEAT_PAuth2, without
/// PACM and the FEAT_PAuth_LR forms.
pub fn is_pauth(name: &str) -> bool {
    ["PAC", "AUT", "XPAC", "BRA", "BLRA", "RETA", "LDRA"]
        .iter()
        .any(|p| name.starts_with(p))
        && !name.starts_with("PACM")
        && !name.contains("SPPC")
        && !name.contains("171615")
}

thread_local! {
    static PC: Cell<u64> = const { Cell::new(0) };
}

/// The guest state a run reads and writes.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct State {
    /// `x0` to `x30`.
    pub x: [u64; 31],
    /// SP (`SP_EL0`).
    pub sp: u64,
    /// The program counter.
    pub pc: u64,
    /// N, Z, C, V.
    pub nzcv: [bool; 4],
    /// The data window.
    pub mem: Vec<u8>,
}

/// How a run ended.
#[derive(Debug, PartialEq, Eq)]
pub enum Outcome {
    /// It ran; the state after.
    Done(Box<State>),
    /// An access outside the data window, at this address.
    Abort(u64),
    /// An exception the instruction raises (undefined, unpredictable).
    Exception(&'static str),
}

/// The concrete layer: the request's state and the window accesses.
#[derive(Default)]
pub struct Concrete {
    /// The state before the run, and the window as the run leaves it.
    pub input: State,
    /// The address of an access outside the window.
    pub abort: Option<u64>,
}

fn default_pstate(g: &Gen<'_, Concrete>, nzcv: [bool; 4]) -> R<V> {
    let fields =
        g.m.structs
            .get("ProcState")
            .ok_or_else(|| err("no ProcState"))?;
    let mut f = Vec::new();
    for (n, _) in fields {
        let v = match n.as_str() {
            "EL" | "BTYPE" => V::Bits(2, 0),
            "GE" => V::Bits(4, 0),
            "IT" => V::Bits(8, 0),
            "M" => V::Bits(5, 0),
            "N" => V::Bits(1, u128::from(nzcv[0])),
            "Z" => V::Bits(1, u128::from(nzcv[1])),
            "C" => V::Bits(1, u128::from(nzcv[2])),
            "V" => V::Bits(1, u128::from(nzcv[3])),
            _ => V::Bits(1, 0),
        };
        f.push((n.clone(), v));
    }
    Ok(V::Struct("ProcState".into(), Rc::new(f)))
}

fn bits(v: &V) -> R<u128> {
    match v {
        V::Bits(_, x) => Ok(*x),
        V::Bf(_, b) => bits(b),
        V::Undef(Some(_)) => Ok(0),
        _ => fail(format!("not concrete bits: {v:?}")),
    }
}

impl Cpu for Concrete {
    fn cut(g: &mut Gen<'_, Concrete>, name: &str, a: &[V], _hint: Option<u32>) -> R<Option<V>> {
        match name {
            "IsFeatureImplemented" => {
                let V::Enum(t, i) = &a[0] else {
                    return fail("symbolic feature");
                };
                let f = g.m.enums[t.as_ref()][*i].as_str();
                let has = crate::cut::feature(f) == Some(true)
                    || Feature::from_name(f).is_some_and(|f| CPU.has(f));
                Ok(Some(V::Bool(has)))
            }
            "Mem_read" | "Mem_read__1" | "Mem_read__2" => {
                let addr = bits(&a[0])? as u64;
                let size = g.cint(&a[1])? as usize;
                let Some(off) = window(addr, size) else {
                    g.cpu.abort = Some(addr);
                    return Err(E::Throw("Abort"));
                };
                let mut x = 0u128;
                for (i, b) in g.cpu.input.mem[off..off + size].iter().enumerate() {
                    x |= u128::from(*b) << (8 * i);
                }
                Ok(Some(V::Bits(8 * size as u32, x)))
            }
            "Mem_set" | "Mem_set__1" | "Mem_set__2" => {
                let addr = bits(&a[0])? as u64;
                let size = g.cint(&a[1])? as usize;
                let x = bits(a.last().expect("data"))?;
                let Some(off) = window(addr, size) else {
                    g.cpu.abort = Some(addr);
                    return Err(E::Throw("Abort"));
                };
                for i in 0..size {
                    g.cpu.input.mem[off + i] = (x >> (8 * i)) as u8;
                }
                Ok(Some(V::Unit))
            }
            "sail_branch_announce" => Ok(Some(V::Unit)),
            _ => Ok(None),
        }
    }

    fn init_reg(g: &mut Gen<'_, Concrete>, name: &str) -> Option<R<V>> {
        if let Some(n) = name.strip_prefix('R').and_then(|n| n.parse::<usize>().ok())
            && n < 31
        {
            return Some(Ok(V::Bits(64, u128::from(g.cpu.input.x[n]))));
        }
        Some(match name {
            "SP_EL0" => Ok(V::Bits(64, u128::from(g.cpu.input.sp))),
            "_PC" => Ok(V::Bits(64, u128::from(g.cpu.input.pc))),
            "PSTATE" => default_pstate(g, g.cpu.input.nzcv),
            "__ThisInstrEnc" => match g.m.enum_of.get("__A64") {
                Some((t, i)) => Ok(V::Enum(t.as_str().into(), *i)),
                None => fail("no __A64"),
            },
            "SEE" => Ok(V::Int(-1)),
            "__BranchTaken" | "SPESampleInFlight" | "InGuardedPage" => Ok(V::Bool(false)),
            "BTypeNext" => Ok(V::Bits(2, 0)),
            n => {
                let (key, hi) = PacKey::of_register(n)?;
                let i = 2 * key as usize + usize::from(!hi);
                Ok(V::Bits(64, u128::from(CHECK_PAC_KEYS[i])))
            }
        })
    }

    fn is_register(name: &str) -> bool {
        Arm64::is_register(name)
    }

    fn reg_config(name: &str) -> u128 {
        Arm64::reg_config(name)
    }

    fn stub_ok(name: &str) -> bool {
        Arm64::stub_ok(name) || name == "Hint_Branch"
    }

    fn decode_args(word: V) -> Vec<V> {
        vec![V::Bits(64, u128::from(PC.with(Cell::get))), word]
    }
}

/// The window offset of `size` bytes at `addr`, if they are all inside.
fn window(addr: u64, size: usize) -> Option<usize> {
    let off = addr.checked_sub(DATA)? as usize;
    (off.checked_add(size)? <= DATA_SIZE).then_some(off)
}

/// Runs `word` at `input.pc` in the model.
pub fn run(m: &Model, word: u32, input: &State) -> Result<Outcome, String> {
    let group = crate::template::group_of(word).ok_or("no decode group")?;
    let pats = relift_sail::stage::clauses(m, group, 32);
    let cls = match m.func(group) {
        Some(Ok(c)) => c,
        Some(Err(e)) => return Err(e),
        None => return Err(format!("no {group}")),
    };
    PC.with(|p| p.set(input.pc));
    let mut from = 0;
    loop {
        let Some(c) = pats
            .iter()
            .skip(from)
            .position(|c| word & c.mask == c.value)
            .map(|i| i + from)
        else {
            return Err("no decode clause takes the word".into());
        };
        let mut g: Gen<'_, Concrete> = Gen::new(m, u32::MAX, word);
        g.cpu.input = input.clone();
        match g.run_decode(&cls[pats[c].index], 32) {
            Ok(()) => return finish(&mut g, input).map(|s| Outcome::Done(Box::new(s))),
            Err(E::See) if from == 0 => from = c + 1,
            Err(E::See) => return Err("Error_See on the retry".into()),
            Err(E::Throw("Abort")) => {
                return Ok(Outcome::Abort(g.cpu.abort.unwrap_or(0)));
            }
            Err(E::Throw(k)) => return Ok(Outcome::Exception(k)),
            Err(E::Fail(e)) => return Err(e),
            Err(e) => return Err(format!("{e:?}")),
        }
    }
}

fn finish(g: &mut Gen<'_, Concrete>, input: &State) -> Result<State, String> {
    let f = |e: E| format!("{e:?}");
    let mut out = State {
        mem: std::mem::take(&mut g.cpu.input.mem),
        ..input.clone()
    };
    for i in 0..31 {
        out.x[i] = bits(&g.reg(&format!("R{i}")).map_err(f)?).map_err(f)? as u64;
    }
    out.sp = bits(&g.reg("SP_EL0").map_err(f)?).map_err(f)? as u64;
    let V::Struct(_, ps) = g.reg("PSTATE").map_err(f)? else {
        return Err("PSTATE is not a struct".into());
    };
    for (i, n) in ["N", "Z", "C", "V"].iter().enumerate() {
        let v = ps.iter().find(|(k, _)| k == n).ok_or("no flag")?;
        out.nzcv[i] = bits(&v.1).map_err(f)? == 1;
    }
    let el = ps.iter().find(|(k, _)| k == "EL").ok_or("no EL")?;
    if bits(&el.1).map_err(f)? != 0 {
        return Err("the instruction left EL0".into());
    }
    out.pc = match g.reg("__BranchTaken").map_err(f)? {
        V::Bool(true) => bits(&g.reg("_PC").map_err(f)?).map_err(f)? as u64,
        V::Bool(false) => input.pc.wrapping_add(4),
        v => return Err(format!("__BranchTaken {v:?}")),
    };
    Ok(out)
}

/// The data window for `seed`, as `check/src/oracle.rs` fills it.
fn fill_window(seed: u64) -> Vec<u8> {
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

fn u32_at(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().expect("4 bytes"))
}

fn u64_at(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().expect("8 bytes"))
}

/// Serves requests on stdin until it closes.
pub fn serve(m: &Model) -> Result<(), String> {
    let mut inp = BufReader::with_capacity(1 << 20, std::io::stdin().lock());
    let mut out = BufWriter::with_capacity(1 << 20, std::io::stdout().lock());
    let io = |e: std::io::Error| e.to_string();
    loop {
        let mut h = [0u8; 8];
        match inp.read_exact(&mut h) {
            Ok(()) => {}
            Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => return Ok(()),
            Err(e) => return Err(e.to_string()),
        }
        if u32_at(&h, 0) != REQ_MAGIC {
            return Err("bad request header".into());
        }
        let n = u32_at(&h, 4) as usize;
        let mut w = vec![0u8; 4 * n];
        inp.read_exact(&mut w).map_err(io)?;
        let mut s = vec![0u8; STATE_SIZE];
        inp.read_exact(&mut s).map_err(io)?;
        let mut k = [0u8; 4];
        inp.read_exact(&mut k).map_err(io)?;
        let mem = if u32_at(&k, 0) == 0 {
            let mut seed = [0u8; 8];
            inp.read_exact(&mut seed).map_err(io)?;
            fill_window(u64::from_le_bytes(seed))
        } else {
            let mut b = vec![0u8; DATA_SIZE];
            inp.read_exact(&mut b).map_err(io)?;
            b
        };
        let mut input = State {
            sp: u64_at(&s, 248),
            pc: CODE,
            mem,
            ..State::default()
        };
        for (i, x) in input.x.iter_mut().enumerate() {
            *x = u64_at(&s, 8 * i);
        }
        let f = u32_at(&s, 264);
        for (i, b) in input.nzcv.iter_mut().enumerate() {
            *b = (f >> (31 - i)) & 1 == 1;
        }
        let word = (n == 1).then(|| u32_at(&w, 0));
        let pauth = word
            .and_then(|w| decode_for(w, &CPU))
            .is_some_and(|i| is_pauth(i.encoding.name()));
        let (status, sig, addr, after) = match word {
            Some(word) if pauth => match run(m, word, &input) {
                Ok(Outcome::Done(st)) => (0, 0, 0, *st),
                Ok(Outcome::Abort(a)) => (1, 11, a, input.clone()),
                Ok(Outcome::Exception(_)) => (1, 4, 0, input.clone()),
                Err(e) => {
                    eprintln!("oracle: {word:08x}: {e}");
                    (2, 0, 0, input.clone())
                }
            },
            _ => (2, 0, 0, input.clone()),
        };
        let mut r = Vec::with_capacity(32 + STATE_SIZE);
        for v in [RES_MAGIC, status, sig, 0] {
            r.extend_from_slice(&v.to_le_bytes());
        }
        r.extend_from_slice(&addr.to_le_bytes());
        let mut st = s.clone();
        for (i, x) in after.x.iter().enumerate() {
            st[8 * i..8 * i + 8].copy_from_slice(&x.to_le_bytes());
        }
        st[248..256].copy_from_slice(&after.sp.to_le_bytes());
        st[256..264].copy_from_slice(&after.pc.to_le_bytes());
        let nz = after
            .nzcv
            .iter()
            .enumerate()
            .fold(f & 0x0fff_ffff, |a, (i, b)| a | (u32::from(*b) << (31 - i)));
        st[264..268].copy_from_slice(&nz.to_le_bytes());
        r.extend_from_slice(&st);
        let mut ranges = Vec::new();
        let mut i = 0;
        while i < DATA_SIZE {
            if after.mem[i] == input.mem[i] {
                i += 1;
                continue;
            }
            let j = (i..DATA_SIZE)
                .find(|j| after.mem[*j] == input.mem[*j])
                .unwrap_or(DATA_SIZE);
            ranges.push((i, j));
            i = j;
        }
        r.extend_from_slice(&(ranges.len() as u32).to_le_bytes());
        for (i, j) in ranges {
            r.extend_from_slice(&(i as u32).to_le_bytes());
            r.extend_from_slice(&((j - i) as u32).to_le_bytes());
            r.extend_from_slice(&after.mem[i..j]);
        }
        out.write_all(&r).map_err(io)?;
        out.flush().map_err(io)?;
    }
}
