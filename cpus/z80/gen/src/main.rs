//! Generates the Z80 lifting templates in `cpus/z80/src/lift/generated.rs`
//! from the Sail model in `cpus/z80/sail/z80.sail`: one template per opcode
//! of every page, one per interrupt entry and one for a lone prefix. Run it
//! only to regenerate; normal builds use the committed output.
//!
//! ```text
//! relift-cpu-z80-gen gen SAIL_LIB MODEL > OUT
//! relift-cpu-z80-gen try SAIL_LIB MODEL OPCODE...
//! ```
//!
//! `SAIL_LIB` is the `lib` directory of the Sail compiler sources, as
//! `cpus/arm64/gen/fetch-sail.sh` downloads it; `MODEL` is
//! `cpus/z80/sail/z80.sail`.
//!
//! `OPCODE` is a hex byte with an optional group prefix: `3c`, `cb7e`,
//! `ed b0`, `dd34`, `fd21`, `ddcb06`, `fdcbfe`, `irq0` to `irq3` or
//! `lonedd`.

mod cut;

use std::fmt::Write as _;
use std::path::Path;
use std::process::ExitCode;

use relift_cpu_z80::Group;
use relift_sail::model::Model;
use relift_sail::stage::{self, Form};

use cut::{GROUP, LIVE, Z80};

/// Every group, in the order of their numbers.
const ALL: [Group; 9] = [
    Group::Base,
    Group::Cb,
    Group::Ed,
    Group::Dd,
    Group::Fd,
    Group::Ddcb,
    Group::Fdcb,
    Group::Accept,
    Group::Lone,
];

/// The name prefix of a group's templates.
fn name(g: Group) -> &'static str {
    match g {
        Group::Base => "Op",
        Group::Cb => "Cb",
        Group::Ed => "Ed",
        Group::Dd => "Dd",
        Group::Fd => "Fd",
        Group::Ddcb => "Ddcb",
        Group::Fdcb => "Fdcb",
        Group::Accept => "Irq",
        Group::Lone => "Lone",
    }
}

/// The prefix `try` reads.
fn prefix(g: Group) -> &'static str {
    match g {
        Group::Base => "",
        Group::Accept => "irq",
        Group::Lone => "lone",
        g => name(g),
    }
}

/// The Sail decode function.
fn decoder(g: Group) -> &'static str {
    match g {
        Group::Base | Group::Dd | Group::Fd => "__DecodeZ80",
        Group::Cb | Group::Ddcb | Group::Fdcb => "__DecodeZ80_CB",
        Group::Ed => "__DecodeZ80_ED",
        Group::Accept => "__Z80_Accept",
        Group::Lone => "__Z80_Lone",
    }
}

/// The model's `Page`.
pub fn page(g: Group) -> u8 {
    match g {
        Group::Base | Group::Lone => 0,
        Group::Cb => 1,
        Group::Ed => 2,
        Group::Dd => 3,
        Group::Fd => 4,
        Group::Ddcb => 5,
        Group::Fdcb => 6,
        Group::Accept => 7,
    }
}

/// The codes of a group.
fn codes(g: Group) -> Vec<u8> {
    match g {
        Group::Base | Group::Dd | Group::Fd => (0..=255u8)
            .filter(|c| !matches!(c, 0xCB | 0xDD | 0xED | 0xFD))
            .collect(),
        Group::Accept => (0..4).collect(),
        Group::Lone => vec![0xDD, 0xFD],
        _ => (0..=255u8).collect(),
    }
}

/// One template: its group and code.
#[derive(Clone, Copy)]
struct Op {
    group: Group,
    code: u8,
}

impl Op {
    fn all() -> Vec<Op> {
        ALL.iter()
            .flat_map(|&group| codes(group).into_iter().map(move |code| Op { group, code }))
            .collect()
    }

    fn parse(s: &str) -> Option<Op> {
        let s = s.replace(' ', "").to_ascii_lowercase();
        let group = ALL
            .iter()
            .filter(|g| s.starts_with(&prefix(**g).to_ascii_lowercase()))
            .max_by_key(|g| prefix(**g).len())?;
        let code = u8::from_str_radix(&s[prefix(*group).len()..], 16).ok()?;
        codes(*group).contains(&code).then_some(Op {
            group: *group,
            code,
        })
    }
}

impl Form for Op {
    fn name(&self) -> String {
        format!("{}{:02X}", name(self.group), self.code)
    }

    fn mask(&self) -> u32 {
        if self.group == Group::Accept {
            0x3
        } else {
            0xFF
        }
    }

    fn value(&self) -> u32 {
        u32::from(self.code)
    }

    fn word_bits(&self) -> u32 {
        if self.group == Group::Accept { 2 } else { 8 }
    }

    fn group(&self) -> Option<&'static str> {
        Some(decoder(self.group))
    }

    fn reachable(&self, _known: u32, _value: u32) -> bool {
        true
    }
}

fn load(lib: &str, path: &str) -> Result<Model, String> {
    let model = Path::new(path);
    let dir = model.parent().unwrap_or(Path::new("."));
    let file = model
        .file_name()
        .and_then(|f| f.to_str())
        .ok_or_else(|| format!("{path}: not a file"))?;
    Model::load(Path::new(lib), dir, &[file]).map_err(|e| format!("{path}: {e}"))
}

fn one(m: &Model, o: &Op) -> Result<stage::Template, String> {
    GROUP.with(|g| g.set(o.group));
    LIVE.with(|l| l.set(false));
    stage::template::<Z80>(m, o)
}

/// The live form of `o`'s template (operands read when it runs), named
/// with an `L` suffix; `None` when the template reads no operand.
fn one_live(m: &Model, o: &Op, fixed: &stage::Template) -> Result<Option<String>, String> {
    if !fixed.code.contains("st.fetch(") {
        return Ok(None);
    }
    GROUP.with(|g| g.set(o.group));
    LIVE.with(|l| l.set(true));
    let t = stage::template::<Z80>(m, o);
    LIVE.with(|l| l.set(false));
    let name = o.name();
    Ok(Some(t?.code.replacen(
        &format!("fn {name}("),
        &format!("fn {name}L("),
        1,
    )))
}

const HEADER: &str = "\
//! Generated by relift-cpu-z80-gen from `cpus/z80/sail/z80.sail`: one
//! lifting template per opcode of every page, per interrupt entry and for
//! a lone prefix. Do not edit; see `cpus/z80/gen/README.md` to regenerate.

#![allow(non_snake_case, unused, clippy::all)]

use relift_ir::{BinOp, Builder, CmpOp, UnOp, Width};

use super::rt::{self, Kind, LiftError, St, Target};
use crate::regs;

";

fn generate(m: &Model) -> Result<String, String> {
    let mut out = String::from(HEADER);
    let ops = Op::all();
    let mut body = String::new();
    let mut failed = Vec::new();
    let mut live = Vec::new();
    for o in &ops {
        match one(m, o) {
            Ok(t) => {
                body.push_str(&t.code);
                body.push('\n');
                match one_live(m, o, &t) {
                    Ok(Some(code)) => {
                        body.push_str(&code);
                        body.push('\n');
                        live.push(*o);
                    }
                    Ok(None) => {}
                    Err(e) => failed.push(format!("{} (live): {e}", o.name())),
                }
            }
            Err(e) => failed.push(format!("{}: {e}", o.name())),
        }
    }
    for g in ALL {
        let n = ops.iter().filter(|o| o.group == g).count();
        eprintln!("{}: {n} templates", name(g));
    }
    eprintln!(
        "{} templates generated, {} live, {} failed",
        ops.len() - failed.len(),
        live.len(),
        failed.len()
    );
    if !failed.is_empty() {
        return Err(failed.join("\n"));
    }
    out.push_str(
        "/// The templates.\n#[derive(Clone, Copy, Debug, PartialEq, Eq)]\npub enum Encoding {\n",
    );
    for o in &ops {
        let _ = writeln!(out, "    /// `{}`.\n    {},", o.name(), o.name());
    }
    out.push_str("}\n\n");
    out.push_str(&body);
    out.push_str("/// The template of `code` in group `group` (0 base, 1 CB, 2 ED, 3 DD, 4 FD, 5 DDCB, 6 FDCB, 7 interrupt entry, 8 lone prefix).\n");
    out.push_str(
        "pub(super) fn template(group: u8, code: u8) -> Option<super::GenFn> {\n    Some(match (group, code) {\n",
    );
    for o in &ops {
        let _ = writeln!(
            out,
            "        ({}, {:#04x}) => {},",
            o.group as u8,
            o.code,
            o.name()
        );
    }
    out.push_str("        _ => return None,\n    })\n}\n");
    out.push_str("/// The live template of `code` in group `group` (operands read when it runs), for the templates that read operands.\n");
    out.push_str(
        "pub(super) fn template_live(group: u8, code: u8) -> Option<super::GenFn> {\n    Some(match (group, code) {\n",
    );
    for o in &live {
        let _ = writeln!(
            out,
            "        ({}, {:#04x}) => {}L,",
            o.group as u8,
            o.code,
            o.name()
        );
    }
    out.push_str("        _ => return None,\n    })\n}\n");
    Ok(out)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    let usage = || {
        eprintln!("usage: relift-cpu-z80-gen gen SAIL_LIB MODEL | try SAIL_LIB MODEL OPCODE...");
        ExitCode::from(2)
    };
    if args.len() < 4 {
        return usage();
    }
    let m = match load(&args[2], &args[3]) {
        Ok(m) => m,
        Err(e) => {
            eprintln!("{e}");
            return ExitCode::FAILURE;
        }
    };
    match args[1].as_str() {
        "gen" => match generate(&m) {
            Ok(text) => {
                print!("{text}");
                ExitCode::SUCCESS
            }
            Err(e) => {
                eprintln!("{e}");
                ExitCode::FAILURE
            }
        },
        "try" => {
            for a in &args[4..] {
                let Some(o) = Op::parse(a) else {
                    println!("{a}: not an opcode");
                    continue;
                };
                match one(&m, &o) {
                    Ok(t) => println!("{}", t.code),
                    Err(e) => println!("{}: {e}", o.name()),
                }
            }
            ExitCode::SUCCESS
        }
        _ => usage(),
    }
}
