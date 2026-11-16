//! `relift-cpu-mips-gen`: generates the VR4300 decode table and lifting
//! templates from sail-cheri-mips's MIPS model plus the VR4300 overlay, with
//! the shared Sail frontend (`relift-sail`).
//!
//! Usage: `relift-cpu-mips-gen gen SAIL_LIB MODEL_SRC OVERLAY OUT`, where
//! OUT is `decode` or a template class (`int`, `ldst`, `branch`, `system`,
//! `cop1`), or `try SAIL_LIB MODEL_SRC OVERLAY NAME...`.

mod cut;

use std::collections::HashMap;
use std::fmt::Write as _;
use std::process::ExitCode;

use relift_sail::model::Model;
use relift_sail::stage::{self, Form};

use cut::Mips;

/// The model files the generator reads, in load order.
const SRC_FILES: &[&str] = &[
    "notrace.sail",
    "prelude.sail",
    "mips_prelude.sail",
    "mips_tlb.sail",
    "mips_wrappers.sail",
    "mips_ast_decl.sail",
    "mips_insts.sail",
    "mips_ri.sail",
    "mips_epilogue.sail",
];

/// The `$ifdef` names the model is read with: the implicit-width
/// `sign_extend` and `zero_extend` signatures.
const DEFINES: &[&str] = &["FEATURE_IMPLICITS"];

/// The decode group the generator builds from the model's `decode` clauses.
const GROUP: &str = "__DecodeMIPS";

/// The template classes, one generated file each.
const CLASSES: &[&str] = &["int", "ldst", "branch", "system", "cop1"];

/// The VR4300 mnemonic of every decode clause, by the clause's AST
/// constructor and in clause order, with the class of its template. A
/// `COP1.fmt` encoding is every arithmetic, conversion and compare
/// operation of one format.
const MNEMONICS: &[(&str, &[&str], &str)] = &[
    (
        "COP1_Move",
        &["MFC1", "DMFC1", "CFC1", "MTC1", "DMTC1", "CTC1"],
        "cop1",
    ),
    ("COP1_BC", &["BC1F", "BC1T", "BC1FL", "BC1TL"], "branch"),
    (
        "COP1_Arith",
        &["COP1.S", "COP1.D", "COP1.W", "COP1.L"],
        "cop1",
    ),
    ("COP1_Mem", &["LWC1", "LDC1", "SWC1", "SDC1"], "cop1"),
    ("DADDIU", &["DADDIU"], "int"),
    ("DADDU", &["DADDU"], "int"),
    ("DADDI", &["DADDI"], "int"),
    ("DADD", &["DADD"], "int"),
    ("ADD", &["ADD"], "int"),
    ("ADDI", &["ADDI"], "int"),
    ("ADDU", &["ADDU"], "int"),
    ("ADDIU", &["ADDIU"], "int"),
    ("DSUBU", &["DSUBU"], "int"),
    ("DSUB", &["DSUB"], "int"),
    ("SUB", &["SUB"], "int"),
    ("SUBU", &["SUBU"], "int"),
    ("AND", &["AND"], "int"),
    ("ANDI", &["ANDI"], "int"),
    ("OR", &["OR"], "int"),
    ("ORI", &["ORI"], "int"),
    ("NOR", &["NOR"], "int"),
    ("XOR", &["XOR"], "int"),
    ("XORI", &["XORI"], "int"),
    ("LUI", &["LUI"], "int"),
    ("DSLL", &["DSLL"], "int"),
    ("DSLL32", &["DSLL32"], "int"),
    ("DSLLV", &["DSLLV"], "int"),
    ("DSRA", &["DSRA"], "int"),
    ("DSRA32", &["DSRA32"], "int"),
    ("DSRAV", &["DSRAV"], "int"),
    ("DSRL", &["DSRL"], "int"),
    ("DSRL32", &["DSRL32"], "int"),
    ("DSRLV", &["DSRLV"], "int"),
    ("SLL", &["SLL"], "int"),
    ("SLLV", &["SLLV"], "int"),
    ("SRA", &["SRA"], "int"),
    ("SRAV", &["SRAV"], "int"),
    ("SRL", &["SRL"], "int"),
    ("SRLV", &["SRLV"], "int"),
    ("SLT", &["SLT"], "int"),
    ("SLTI", &["SLTI"], "int"),
    ("SLTU", &["SLTU"], "int"),
    ("SLTIU", &["SLTIU"], "int"),
    ("MFHI", &["MFHI"], "int"),
    ("MFLO", &["MFLO"], "int"),
    ("MTHI", &["MTHI"], "int"),
    ("MTLO", &["MTLO"], "int"),
    ("MULT", &["MULT"], "int"),
    ("MULTU", &["MULTU"], "int"),
    ("DMULT", &["DMULT"], "int"),
    ("DMULTU", &["DMULTU"], "int"),
    ("DIV", &["DIV"], "int"),
    ("DIVU", &["DIVU"], "int"),
    ("DDIV", &["DDIV"], "int"),
    ("DDIVU", &["DDIVU"], "int"),
    ("J", &["J"], "branch"),
    ("JAL", &["JAL"], "branch"),
    ("JR", &["JR"], "branch"),
    ("JALR", &["JALR"], "branch"),
    ("BEQ", &["BEQ", "BEQL", "BNE", "BNEL"], "branch"),
    (
        "BCMPZ",
        &[
            "BLTZ", "BLTZAL", "BLTZL", "BLTZALL", "BGEZ", "BGEZAL", "BGEZL", "BGEZALL", "BGTZ",
            "BGTZL", "BLEZ", "BLEZL",
        ],
        "branch",
    ),
    ("SYSCALL", &["SYSCALL"], "system"),
    ("BREAK", &["BREAK"], "system"),
    (
        "TRAPREG",
        &["TGE", "TGEU", "TLT", "TLTU", "TEQ", "TNE"],
        "system",
    ),
    (
        "TRAPIMM",
        &["TEQI", "TNEI", "TGEI", "TGEIU", "TLTI", "TLTIU"],
        "system",
    ),
    (
        "Load",
        &["LB", "LBU", "LH", "LHU", "LW", "LWU", "LD", "LL", "LLD"],
        "ldst",
    ),
    ("Store", &["SB", "SH", "SW", "SD", "SC", "SCD"], "ldst"),
    ("LWL", &["LWL"], "ldst"),
    ("LWR", &["LWR"], "ldst"),
    ("SWL", &["SWL"], "ldst"),
    ("SWR", &["SWR"], "ldst"),
    ("LDL", &["LDL"], "ldst"),
    ("LDR", &["LDR"], "ldst"),
    ("SDL", &["SDL"], "ldst"),
    ("SDR", &["SDR"], "ldst"),
    ("CACHE", &["CACHE"], "system"),
    ("SYNC", &["SYNC"], "system"),
    ("MFC0", &["MFC0", "DMFC0"], "system"),
    ("MTC0", &["MTC0", "DMTC0"], "system"),
    ("TLBWI", &["TLBWI"], "system"),
    ("TLBWR", &["TLBWR"], "system"),
    ("TLBR", &["TLBR"], "system"),
    ("TLBP", &["TLBP"], "system"),
    ("ERET", &["ERET"], "system"),
];

/// One encoding: a decode clause with a bit pattern.
struct Enc {
    /// The Rust name: the mnemonic in camel case.
    name: String,
    mnemonic: &'static str,
    class: &'static str,
    mask: u32,
    value: u32,
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
    fn group(&self) -> Option<&'static str> {
        Some(GROUP)
    }
    /// Every word with the encoding's fixed bits decodes to it: `load`
    /// checks that no two clauses share a word.
    fn reachable(&self, _known: u32, _value: u32) -> bool {
        true
    }
}

/// `COP1.S` as `Cop1S`, `BC1FL` as `Bc1fl`.
fn rust_name(mnemonic: &str) -> String {
    mnemonic
        .split('.')
        .map(|p| {
            let mut c = p.chars();
            c.next()
                .map(|f| f.to_ascii_uppercase().to_string() + &c.as_str().to_ascii_lowercase())
                .unwrap_or_default()
        })
        .collect()
}

/// The text after the first parenthesis group of `rest` at depth 0 and the
/// group itself: a decode clause's pattern and its body.
fn split_pattern(rest: &str) -> Option<(&str, &str)> {
    let mut depth = 0i32;
    for (i, c) in rest.bytes().enumerate() {
        match c {
            b'(' => depth += 1,
            b')' => {
                depth -= 1;
                if depth == 0 {
                    return Some((&rest[..=i], &rest[i + 1..]));
                }
            }
            _ => {}
        }
    }
    None
}

/// The AST constructor a decode clause body builds: `X` in `Some(X(..))`.
fn constructor(body: &str) -> String {
    let inner = body.strip_prefix("Some(").unwrap_or(body);
    inner
        .split(|c: char| c == '(' || c.is_whitespace())
        .next()
        .unwrap_or_default()
        .to_string()
}

/// Copies the model's `decode` clauses into one group whose bodies run
/// `execute` through the overlay's `mips_run`, with the model's synonym
/// field types spelled as bit vectors, and gives each clause's constructor.
fn build_group(m: &mut Model) -> Vec<String> {
    let raws = m.funcs.get("decode").cloned().unwrap_or_default();
    let mut text = String::new();
    let mut ctors = Vec::new();
    for raw in &raws {
        let Some(rest) = raw.strip_prefix("function clause decode") else {
            continue;
        };
        let Some((pat, after)) = split_pattern(rest) else {
            continue;
        };
        let mut pat = pat.to_string();
        for (syn, w) in [("regno", 5), ("IntRegEnc", 5), ("imm16", 16)] {
            pat = pat.replace(&format!(": {syn}"), &format!(": bits({w})"));
        }
        let body = after.trim_start().strip_prefix('=').unwrap_or("").trim();
        ctors.push(constructor(body));
        let _ = write!(text, "function clause {GROUP} {pat} = mips_run({body})\n\n");
    }
    m.load_text(&text);
    ctors
}

fn load(lib: &str, src: &str, overlay: &str) -> Result<(Model, Vec<Enc>), String> {
    let mut m = Model::load_defined(lib.as_ref(), src.as_ref(), SRC_FILES, DEFINES)
        .map_err(|e| e.to_string())?;
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
    let ctors = build_group(&mut m);
    let mut seen: HashMap<String, usize> = HashMap::new();
    let mut encs: Vec<Enc> = Vec::new();
    for p in stage::clauses(&m, GROUP, 32) {
        let ctor = ctors.get(p.index).cloned().unwrap_or_default();
        let k = seen.entry(ctor.clone()).or_default();
        let (mnemonic, class) = MNEMONICS
            .iter()
            .find(|(c, _, _)| *c == ctor)
            .and_then(|(_, ns, class)| ns.get(*k).map(|n| (*n, *class)))
            .ok_or_else(|| format!("decode clause {} of {ctor} has no mnemonic", *k + 1))?;
        *k += 1;
        if let Some(o) = encs
            .iter()
            .find(|o| (o.value ^ p.value) & o.mask & p.mask == 0)
        {
            return Err(format!("{mnemonic} and {} share words", o.mnemonic));
        }
        encs.push(Enc {
            name: rust_name(mnemonic),
            mnemonic,
            class,
            mask: p.mask,
            value: p.value,
        });
    }
    for (ctor, ns, _) in MNEMONICS {
        if seen.get(*ctor).copied().unwrap_or(0) != ns.len() {
            return Err(format!(
                "{ctor}: the model's clauses and the mnemonics differ"
            ));
        }
    }
    Ok((m, encs))
}

const HEADER: &str = "//! Generated by relift-cpu-mips-gen from sail-cheri-mips 13a9c3fb0c3f8d320d4f4650a63d23d3a8b12724,\n//! the Sail library at c44944f11b71e5c9bd4b30cdbf79ea32f6dcf456 and the VR4300 overlay\n//! `cpus/mips/gen/vr4300.sail`. Do not edit; see `cpus/mips/gen/README.md` to regenerate.\n//! Derived from sail-cheri-mips (BSD-2-Clause, see `NOTICE-sail-cheri-mips` next to this\n//! crate's manifest); the Sail library parts are BSD-2-Clause.\n";

fn write_table(encs: &[Enc]) -> String {
    let mut s = String::from(HEADER);
    s.push_str("\n/// A VR4300 encoding: a decode clause of sail-cheri-mips or the overlay.\n");
    s.push_str("#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]\npub enum Encoding {\n");
    for e in encs {
        let _ = writeln!(s, "    /// `{}`.", e.mnemonic);
        let _ = writeln!(s, "    {},", e.name);
    }
    s.push_str("}\n\n/// The mnemonic of every encoding, in `Encoding` order.\n");
    s.push_str("pub(crate) const NAMES: &[&str] = &[\n");
    for e in encs {
        let _ = writeln!(s, "    \"{}\",", e.mnemonic);
    }
    s.push_str("];\n\n/// Every encoding with its fixed bits and template class, in the\n/// model's decode order.\n");
    s.push_str("pub(crate) static FORMS: &[(Encoding, u32, u32, &str)] = &[\n");
    for e in encs {
        let _ = writeln!(
            s,
            "    (Encoding::{}, {:#010x}, {:#010x}, \"{}\"),",
            e.name, e.mask, e.value, e.class
        );
    }
    s.push_str("];\n");
    s
}

fn write_class(m: &Model, encs: &[Enc], class: &str, fails: &mut Vec<(String, String)>) -> String {
    let mut s = String::from(HEADER);
    s.push_str("\n#![allow(non_snake_case, unused, clippy::all)]\n\n");
    s.push_str("use relift_ir::{BinOp, Builder, CmpOp, UnOp, Width};\n\n");
    s.push_str("use crate::decode::Encoding;\nuse crate::lift::rt::{self, Access, Exc, St, Target};\nuse crate::lift::{GenFn, LiftError};\n\n");
    let mut ok = Vec::new();
    for e in encs.iter().filter(|e| e.class == class) {
        match stage::template::<Mips>(m, e) {
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
            "usage: relift-cpu-mips-gen gen|try SAIL_LIB MODEL_SRC OVERLAY decode|CLASS|NAME..."
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
                match encs.iter().find(|e| &e.name == n || e.mnemonic == n) {
                    None => println!("// {n}: no such VR4300 encoding"),
                    Some(e) => match stage::template::<Mips>(&m, e) {
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
                c if CLASSES.contains(&c) => write_class(&m, &encs, c, &mut fails),
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
