//! `relift-cpu-arm32-gen`: generates the ARMv4T decode table and lifting
//! templates from sail-arm's AArch32 model plus the ARMv4T overlay, with
//! the shared Sail frontend (`relift-sail`).
//!
//! Usage: `relift-cpu-arm32-gen gen SAIL_LIB MODEL_SRC OVERLAY OUT_CRATE`,
//! or `try SAIL_LIB MODEL_SRC OVERLAY NAME...`.

mod cut;

use std::cell::RefCell;
use std::collections::{BTreeMap, HashSet};
use std::fmt::Write as _;
use std::process::ExitCode;

use relift_sail::model::Model;
use relift_sail::stage::{self, Form};

use cut::Arm32;

/// The model files the generator reads, in load order.
const SRC_FILES: &[&str] = &[
    "prelude.sail",
    "decode_start.sail",
    "builtins.sail",
    "v8_base.sail",
    "stubs.sail",
    "interrupts.sail",
    "interface.sail",
    "no_devices.sail",
    "impdefs.sail",
    "mem.sail",
    "sysregs_autogen.sail",
    "sysregs.sail",
    "reset.sail",
    "instrs32.sail",
    "decode_end.sail",
    "fetch.sail",
];

/// The A32 decode groups, in the Rust table's order, and the Thumb one.
const GROUPS: &[(&str, &str, bool)] = &[
    ("__DecodeA32_DataProMisc", "A32_DATAPROMISC", false),
    ("__DecodeA32_LoadStoreImmLit", "A32_LOADSTOREIMMLIT", false),
    ("__DecodeA32_LoadStoreReg", "A32_LOADSTOREREG", false),
    ("__DecodeA32_Media", "A32_MEDIA", false),
    ("__DecodeA32_BranchBlock", "A32_BRANCHBLOCK", false),
    ("__DecodeA32_SysASIMDFP", "A32_SYSASIMDFP", false),
    ("__DecodeT16", "T16", true),
];

/// One decode clause of a group.
#[derive(Clone)]
struct Clause {
    name: Option<String>,
    mask: u32,
    value: u32,
}

/// One encoding: an allowed clause.
#[derive(Clone)]
struct Enc {
    name: String,
    group: &'static str,
    mask: u32,
    value: u32,
    thumb: bool,
    /// Its position in the group's clause list.
    pos: usize,
}

thread_local! {
    static CLAUSES: RefCell<BTreeMap<&'static str, Vec<Clause>>> = const { RefCell::new(BTreeMap::new()) };
    static ALLOW: RefCell<HashSet<String>> = RefCell::new(HashSet::new());
}

fn first_match(group: &str, w: u32) -> Option<usize> {
    CLAUSES.with(|c| {
        c.borrow()[group]
            .iter()
            .position(|cl| w & cl.mask == cl.value)
    })
}

impl Form for Enc {
    fn name(&self) -> String {
        self.name.clone()
    }
    fn mask(&self) -> u32 {
        self.mask
    }
    fn value(&self) -> u32 {
        self.value
    }
    fn word_bits(&self) -> u32 {
        if self.thumb { 16 } else { 32 }
    }
    fn group(&self) -> Option<&'static str> {
        Some(self.group)
    }
    fn reachable(&self, known: u32, value: u32) -> bool {
        let mut x: u32 = 0x9e37_79b9 ^ value;
        let all = if self.thumb { 0xffff } else { u32::MAX };
        for _ in 0..512 {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            let w = (value | (x & !known)) & all;
            if first_match(self.group, w) == Some(self.pos) {
                return true;
            }
        }
        false
    }
    fn accept_clause(&self, raw: &str) -> Result<(), String> {
        let name = clause_name(raw).unwrap_or_default();
        if ALLOW.with(|a| a.borrow().contains(&name)) {
            Ok(())
        } else {
            Err(format!("clause {name} is not ARMv4T"))
        }
    }
}

fn clause_name(raw: &str) -> Option<String> {
    let p = raw.find("decode_aarch32_instrs_")?;
    let rest = &raw[p + "decode_aarch32_instrs_".len()..];
    let e = rest.find("enc_")?;
    Some(rest[..e].to_string())
}

fn load(lib: &str, src: &str, overlay: &str) -> Result<(Model, Vec<Enc>), String> {
    let mut m = Model::load(lib.as_ref(), src.as_ref(), SRC_FILES).map_err(|e| e.to_string())?;
    let replaced = m
        .load_overlay(overlay.as_ref())
        .map_err(|e| e.to_string())?;
    if replaced.is_empty() {
        return Err("the overlay replaces no function".into());
    }
    let stale: Vec<&str> = m
        .patched
        .iter()
        .filter(|(_, n)| *n == 0)
        .map(|(p, _)| p.as_str())
        .collect();
    if !stale.is_empty() {
        return Err(format!("overlay lines that change nothing: {stale:?}"));
    }
    let text = std::fs::read_to_string(overlay).map_err(|e| e.to_string())?;
    let mut allow = HashSet::new();
    for l in text.lines() {
        if let Some(rest) = l.trim().strip_prefix("$relift allow ") {
            allow.extend(rest.split_whitespace().map(str::to_string));
        }
    }
    let mut encs = Vec::new();
    let mut seen = HashSet::new();
    let mut table = BTreeMap::new();
    for &(group, _, thumb) in GROUPS {
        let raws = m.funcs.get(group).cloned().unwrap_or_default();
        let pats = stage::clauses(&m, group, if thumb { 16 } else { 32 });
        let mut cls = Vec::new();
        for (pos, p) in pats.iter().enumerate() {
            let name = clause_name(&raws[p.index]);
            let allowed = name.as_ref().is_some_and(|n| allow.contains(n));
            cls.push(Clause {
                name: name.clone(),
                mask: p.mask,
                value: p.value,
            });
            if let (true, Some(n)) = (allowed, name) {
                let mut n = n;
                while !seen.insert(n.clone()) {
                    n.push('_');
                }
                // The A32 condition field is not part of the encoding.
                // The A32 condition is tested at lift time (`st.cond_guard`),
                // so the templates are generated for AL.
                let (mask, value) = if thumb {
                    (p.mask, p.value)
                } else {
                    (p.mask | 0xf000_0000, (p.value & 0x0fff_ffff) | 0xe000_0000)
                };
                encs.push(Enc {
                    name: n,
                    group,
                    mask,
                    value,
                    thumb,
                    pos,
                });
            }
        }
        table.insert(group, cls);
    }
    let missing: Vec<_> = allow.iter().filter(|a| !seen.contains(*a)).collect();
    if !missing.is_empty() {
        eprintln!("overlay: allowed but no clause: {missing:?}");
    }
    CLAUSES.with(|c| *c.borrow_mut() = table);
    ALLOW.with(|a| *a.borrow_mut() = allow);
    Ok((m, encs))
}

fn configure(e: &Enc) {
    let cond = if !e.thumb {
        Some("((w >> 28) & 0xf)")
    } else if e.name == "B_T1" {
        Some("((w >> 8) & 0xf)")
    } else {
        None
    };
    cut::CONFIG.with(|c| {
        *c.borrow_mut() = Arm32 {
            thumb: e.thumb,
            cond,
            reg_shift: !e.thumb && e.name.contains("_rr_"),
        }
    });
}

fn template(m: &Model, e: &Enc) -> Result<stage::Template, String> {
    configure(e);
    stage::template::<Arm32>(m, e)
}

const HEADER: &str = "//! Generated by relift-cpu-arm32-gen from sail-arm 5416ef6b43e9527c38dc71fb752259c1d01a47e1,\n//! the Sail library at c44944f11b71e5c9bd4b30cdbf79ea32f6dcf456 and the ARMv4T overlay\n//! `cpus/arm32/gen/armv4t.sail`. Do not edit; see `cpus/arm32/gen/README.md` to regenerate.\n//! Derived from sail-arm (BSD-3-Clause-Clear, see `NOTICE-sail-arm` next to this crate's\n//! manifest); the Sail library parts are BSD-2-Clause.\n";

fn write_table(encs: &[Enc]) -> String {
    let mut s = String::new();
    s.push_str(HEADER);
    s.push_str("\n#![allow(non_camel_case_types, clippy::all)]\n\n");
    s.push_str("/// An ARMv4T encoding: a decode clause of sail-arm or the overlay.\n");
    s.push_str("#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]\npub enum Encoding {\n");
    for e in encs {
        let _ = writeln!(s, "    {},", e.name);
    }
    s.push_str("}\n\npub(crate) const NAMES: &[&str] = &[\n");
    for e in encs {
        let _ = writeln!(s, "    \"{}\",", e.name);
    }
    s.push_str("];\n\n");
    CLAUSES.with(|c| {
        let c = c.borrow();
        for &(group, rust, _) in GROUPS {
            let _ = writeln!(
                s,
                "pub(crate) static {rust}: &[(u32, u32, Option<Encoding>, &str)] = &["
            );
            for (pos, cl) in c[group].iter().enumerate() {
                let enc = encs
                    .iter()
                    .find(|e| e.group == group && e.pos == pos)
                    .map(|e| format!("Some(Encoding::{})", e.name))
                    .unwrap_or_else(|| "None".into());
                let _ = writeln!(
                    s,
                    "    ({:#010x}, {:#010x}, {enc}, \"{}\"),",
                    cl.mask,
                    cl.value,
                    cl.name.as_deref().unwrap_or("-"),
                );
            }
            s.push_str("];\n\n");
        }
    });
    s.push_str("pub(crate) static FORMS: &[(Encoding, u32, u32, bool, &str)] = &[\n");
    for e in encs {
        let _ = writeln!(
            s,
            "    (Encoding::{}, {:#010x}, {:#010x}, {}, \"{}\"),",
            e.name, e.mask, e.value, e.thumb, e.group
        );
    }
    s.push_str("];\n");
    s
}

fn write_class(m: &Model, encs: &[Enc], thumb: bool, fails: &mut Vec<(String, String)>) -> String {
    let mut s = String::new();
    s.push_str(HEADER);
    s.push_str("\n#![allow(non_snake_case, unused, clippy::all)]\n\n");
    s.push_str("use relift_ir::{BinOp, Builder, CmpOp, Endian, Order, UnOp, Width};\n\n");
    s.push_str("use crate::decode::Encoding;\nuse crate::lift::rt::{self, Kind, St, Target};\nuse crate::lift::{GenFn, LiftError};\nuse crate::regs;\n\n");
    let mut ok = Vec::new();
    for e in encs.iter().filter(|e| e.thumb == thumb) {
        match template(m, e) {
            Ok(t) => {
                s.push_str(&t.code);
                s.push('\n');
                ok.push(e.name.clone());
            }
            Err(err) => fails.push((e.name.clone(), err)),
        }
    }
    s.push_str("/// The generated template of `enc`, if there is one.\npub(crate) fn template(enc: Encoding) -> Option<GenFn> {\n    Some(match enc {\n");
    for n in &ok {
        let _ = writeln!(s, "        Encoding::{n} => {n},");
    }
    s.push_str("        _ => return None,\n    })\n}\n");
    s
}

fn real_main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.len() < 5 {
        eprintln!(
            "usage: relift-cpu-arm32-gen gen|try SAIL_LIB MODEL_SRC OVERLAY decode|a32|t16|NAME..."
        );
        return ExitCode::from(2);
    }
    let (m, encs) = match load(&args[1], &args[2], &args[3]) {
        Ok(x) => x,
        Err(e) => {
            eprintln!("{e}");
            return ExitCode::FAILURE;
        }
    };
    match args[0].as_str() {
        "try" => {
            for n in &args[4..] {
                match encs.iter().find(|e| &e.name == n) {
                    None => println!("// {n}: no such ARMv4T encoding"),
                    Some(e) => match template(&m, e) {
                        Ok(t) => println!("{}", t.code),
                        Err(err) => println!("// {n}: {err}"),
                    },
                }
            }
            ExitCode::SUCCESS
        }
        "gen" => {
            let mut fails = Vec::new();
            let text = match args[4].as_str() {
                "decode" => write_table(&encs),
                "a32" => write_class(&m, &encs, false, &mut fails),
                "t16" => write_class(&m, &encs, true, &mut fails),
                _ => return ExitCode::from(2),
            };
            for (n, e) in &fails {
                eprintln!("FAIL {n}: {e}");
            }
            if !fails.is_empty() {
                return ExitCode::FAILURE;
            }
            print!("{text}");
            ExitCode::SUCCESS
        }
        _ => ExitCode::from(2),
    }
}

fn main() -> ExitCode {
    std::thread::Builder::new()
        .stack_size(256 << 20)
        .spawn(real_main)
        .expect("spawns the worker thread")
        .join()
        .expect("worker finishes")
}
