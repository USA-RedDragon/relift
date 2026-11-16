//! Counts, over a directory of programs, what each host fallback has to
//! cover: warp operations per stage (quad-only or warp-wide, and whether
//! a compute program's warp operations sit in uniform control flow),
//! shared memory sizes, clock reads, 64-bit atomics, global memory and
//! kills; and, for every program with warp operations, the warp path each
//! host preset takes and whether the result is exact there.
//!
//! `hostscan <dir> [threads]`

use relift_gpu_sm86::Operand;
use relift_gpu_sm86::program::{Program, Stage};
use relift_gpu_sm86::translate::{HostProfile, Options, translate, translate_with};
use std::collections::BTreeMap;
use std::sync::Mutex;
use std::sync::atomic::{AtomicUsize, Ordering};

#[derive(Default)]
struct Tally {
    n: BTreeMap<String, u64>,
}

impl Tally {
    fn add(&mut self, k: impl Into<String>, v: u64) {
        *self.n.entry(k.into()).or_default() += v;
    }
}

fn stage_name(s: Stage) -> &'static str {
    match s {
        Stage::Vertex => "vertex",
        Stage::TessControl => "tess-control",
        Stage::TessEval => "tess-eval",
        Stage::Geometry => "geometry",
        Stage::Fragment => "fragment",
        Stage::Compute => "compute",
    }
}

fn imm(o: Option<&Operand>) -> Option<u64> {
    match o? {
        Operand::Int(v) => Some(*v as u64),
        Operand::Hex(v) => Some(*v),
        _ => None,
    }
}

/// `Some(true)` for a warp operation that only reads its own 2x2 quad,
/// `Some(false)` for one that reads the whole warp, `None` otherwise.
fn warp_kind(i: &relift_gpu_sm86::Inst) -> Option<bool> {
    match i.mnemonic {
        "FSWZADD" => Some(true),
        "SHFL" => {
            let b = imm(i.ops.get(3));
            let c = imm(i.ops.get(4));
            let quad = match (b, c) {
                (Some(b), Some(c)) => {
                    let seg = (c >> 8) & 0x1f;
                    let clamp = c & 0x1f;
                    if i.has("BFLY") {
                        b < 4
                    } else {
                        seg == 0x1c && clamp == 3
                    }
                }
                _ => false,
            };
            Some(quad)
        }
        "VOTE" | "VOTEU" | "REDUX" | "MATCH" => Some(false),
        "S2R" | "S2UR" | "CS2R" => match i.ops.get(1) {
            Some(Operand::Sym(s))
                if matches!(
                    s.as_str(),
                    "SR_LANEID"
                        | "SR_EQMASK"
                        | "SR_LTMASK"
                        | "SR_LEMASK"
                        | "SR_GTMASK"
                        | "SR_GEMASK"
                ) =>
            {
                Some(false)
            }
            _ => None,
        },
        _ => None,
    }
}

fn divergent_guard(i: &relift_gpu_sm86::Inst) -> bool {
    matches!(i.guard, Some(Operand::Pred(n, _)) if n != 7)
}

fn target(i: &relift_gpu_sm86::Inst) -> Option<i64> {
    i.ops.iter().find_map(|o| match o {
        Operand::Target(t) => Some(*t),
        _ => None,
    })
}

fn scan(p: &Program, t: &mut Tally) {
    let stage = p.stage();
    let sn = stage_name(stage);
    t.add(format!("programs {sn}"), 1);
    let insts: Vec<_> = p.decode().into_iter().map(|d| d.ok()).collect();
    let n = insts.len();
    let mut div = vec![false; n];
    for (k, i) in insts.iter().enumerate() {
        let Some(i) = i else { continue };
        let guarded = divergent_guard(i);
        match i.mnemonic {
            "BSSY" => {
                if let Some(tg) = target(i) {
                    let end = ((tg / 16) as usize).min(n);
                    for d in div.iter_mut().take(end).skip(k + 1) {
                        *d = true;
                    }
                }
            }
            "EXIT" | "RET" | "KILL" if guarded => {
                for d in div.iter_mut().skip(k + 1) {
                    *d = true;
                }
            }
            "BRA" | "BRX" if guarded || i.has("DIV") => {
                if let Some(tg) = target(i) {
                    let to = ((tg / 16) as usize).min(n);
                    let (lo, hi) = if to <= k { (to, k + 1) } else { (k + 1, to) };
                    for d in div.iter_mut().take(hi).skip(lo) {
                        *d = true;
                    }
                }
            }
            _ => {}
        }
    }
    let mut warp_quad = 0u64;
    let mut warp_full = 0u64;
    let mut after_div = 0u64;
    let mut guarded = 0u64;
    let mut clock = false;
    let mut a64g = false;
    let mut a64s = false;
    let mut global = false;
    let mut kill = false;
    for (k, i) in insts.iter().enumerate() {
        let Some(i) = i else { continue };
        if let Some(q) = warp_kind(i) {
            if q {
                warp_quad += 1;
            } else {
                warp_full += 1;
            }
            if div[k] {
                after_div += 1;
            } else if divergent_guard(i) {
                guarded += 1;
            }
        }
        if matches!(i.mnemonic, "S2R" | "CS2R" | "S2UR")
            && matches!(i.ops.get(1), Some(Operand::Sym(s)) if s.starts_with("SR_CLOCK"))
        {
            clock = true;
        }
        let wide = i.has("64") || i.has("S64");
        match i.mnemonic {
            "ATOM" | "ATOMG" | "RED" => {
                global = true;
                a64g |= wide;
            }
            "ATOMS" => a64s |= wide,
            "LDG" | "STG" | "LD" | "ST" => global = true,
            "KILL" => kill = true,
            _ => {}
        }
    }
    if warp_quad + warp_full > 0 {
        t.add(format!("{sn} programs with warp ops"), 1);
        t.add(format!("{sn} warp ops quad-only"), warp_quad);
        t.add(format!("{sn} warp ops warp-wide"), warp_full);
        if warp_full > 0 {
            t.add(format!("{sn} programs with warp-wide ops"), 1);
        } else {
            t.add(format!("{sn} programs with quad-only warp ops"), 1);
        }
        if stage == Stage::Compute {
            if after_div > 0 {
                t.add("compute programs with warp ops in divergent flow", 1);
                t.add("compute warp ops in divergent flow", after_div);
            } else if guarded > 0 {
                t.add("compute programs with guarded warp ops only", 1);
            } else {
                t.add("compute programs with warp ops in uniform flow", 1);
            }
        }
    }
    if clock {
        t.add(format!("{sn} programs reading the clock"), 1);
    }
    if a64g {
        t.add(format!("{sn} programs with 64-bit global atomics"), 1);
    }
    if a64s {
        t.add(format!("{sn} programs with 64-bit shared atomics"), 1);
    }
    if global {
        t.add(format!("{sn} programs with global memory"), 1);
    }
    if kill {
        t.add(format!("{sn} programs with KILL"), 1);
    }
    if warp_quad + warp_full > 0 {
        for h in HostProfile::presets() {
            let name = h.name;
            match translate_with(
                p,
                &Options {
                    host: h,
                    ..Options::default()
                },
            ) {
                Ok(sh) => {
                    let r = &sh.interface.host;
                    t.add(format!("host {name} {sn} warp path {:?}", r.warp), 1);
                    if !r.inexact.is_empty() {
                        t.add(format!("host {name} {sn} inexact programs"), 1);
                    }
                }
                Err(_) => t.add(format!("host {name} {sn} untranslated"), 1),
            }
        }
    }
    if stage == Stage::Compute
        && let Ok(sh) = translate(p)
    {
        let b = sh.interface.shared_bytes;
        let bin = match b {
            0 => "0",
            1..=16384 => "1..16K",
            16385..=32768 => "16K..32K",
            32769..=49152 => "32K..48K",
            _ => ">48K",
        };
        t.add(format!("compute shared bytes {bin}"), 1);
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let dir = args.get(1).cloned().unwrap_or_default();
    let threads: usize = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(4);
    let mut entries: Vec<_> = std::fs::read_dir(&dir)
        .unwrap_or_else(|e| panic!("{dir}: {e}"))
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.extension().is_some_and(|x| x == "bin"))
        .collect();
    entries.sort();
    let next = AtomicUsize::new(0);
    let all = Mutex::new(Tally::default());
    std::thread::scope(|s| {
        for _ in 0..threads {
            s.spawn(|| {
                let mut t = Tally::default();
                loop {
                    let k = next.fetch_add(1, Ordering::Relaxed);
                    let Some(p) = entries.get(k) else { break };
                    let Ok(b) = std::fs::read(p) else { continue };
                    scan(&Program::parse(&b), &mut t);
                }
                if let Ok(mut a) = all.lock() {
                    for (k, v) in t.n {
                        a.add(k, v);
                    }
                }
            });
        }
    });
    let all = all.into_inner().unwrap_or_default();
    for (k, v) in &all.n {
        println!("{v:8} {k}");
    }
}
