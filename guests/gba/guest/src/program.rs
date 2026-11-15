//! A ROM as one lifted unit and the program files that run it.
//!
//! Every function `relift-gba-analyze` found becomes a C function at its
//! entry address; a function holds the instructions it reaches in their
//! state. Code in work RAM is lifted from the bytes of the copy the analysis
//! proved put it there; code the game writes there otherwise is missed
//! code. The runtime finds a function by address alone, so an address that
//! is an entry twice (both states, or two copies) keeps the first one.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;

use relift_emit::{Function, Insn, Options, Output};
use relift_gba_analyze::{EdgeKind, Loc, Report};
use relift_guest::LiftFailure;
use relift_ir::{Block, Flow, Next};

use crate::frontend::{Gba, RAW_THUMB, device_map};

/// Where the ROM is mapped.
pub const ROM_BASE: u32 = 0x0800_0000;

/// The unit's C symbol.
pub const UNIT_SYMBOL: &str = "rl_unit_gba_rom";

/// An instruction that did not lift.
#[derive(Clone, Debug)]
pub struct Unlifted {
    /// Where.
    pub at: Loc,
    /// Why.
    pub failure: LiftFailure,
}

/// Everything a ROM lifts to.
#[derive(Clone, Debug)]
pub struct Lifted {
    /// The emitted files.
    pub output: Output,
    /// Functions in the unit.
    pub functions: usize,
    /// Instructions lifted.
    pub instructions: usize,
    /// Instructions that did not lift.
    pub unlifted: Vec<Unlifted>,
    /// Entries left out because another function has the address.
    pub shadowed: Vec<Loc>,
}

/// The host name of the function at `l`.
#[must_use]
pub fn host_name(l: Loc) -> String {
    let copy = l.copy.map(|c| format!("_c{c}")).unwrap_or_default();
    format!(
        "gba_{}_{:08x}{copy}",
        if l.thumb { 't' } else { 'a' },
        l.addr
    )
}

/// The bytes code at `l` is lifted from and the address they start at: the
/// ROM, or the copy that put the code in work RAM.
fn image<'a>(rom: &'a [u8], report: &'a Report, l: Loc) -> (&'a [u8], u32) {
    match l.copy.and_then(|c| report.copies.get(usize::from(c))) {
        Some(c) => (&c.bytes, c.dst),
        None => (rom, ROM_BASE),
    }
}

fn raw(img: &[u8], base: u32, l: Loc) -> u64 {
    let off = l.addr.wrapping_sub(base) as usize;
    let v = img
        .get(off..off + l.size() as usize)
        .unwrap_or_default()
        .iter()
        .rev()
        .fold(0u64, |a, b| (a << 8) | u64::from(*b));
    if l.thumb { v | RAW_THUMB } else { v }
}

/// The instructions of a function that no other one of it reaches by
/// falling through or by a constant branch: where its computed jumps can
/// land inside it (the analysis follows them; the emitter turns each into a
/// case of the jump's switch, with dispatch for any other target).
fn landings(blocks: &BTreeMap<u64, (Result<Block, LiftFailure>, u64)>, entry: u32) -> Vec<u64> {
    let mut reached = BTreeSet::new();
    for (&pc, (b, _)) in blocks {
        let Ok(b) = b else { continue };
        let fall = pc + u64::from(b.len);
        match b.exit.next {
            Next::Const(t) => {
                reached.insert(t);
            }
            Next::Select {
                taken, not_taken, ..
            } => {
                reached.insert(taken);
                reached.insert(not_taken);
            }
            Next::Dynamic(_) => {}
        }
        if !matches!(b.exit.flow, Flow::Jump)
            || b.insts
                .iter()
                .any(|i| matches!(i.op, relift_ir::Op::Guard(..)))
        {
            reached.insert(fall);
        }
    }
    blocks
        .keys()
        .copied()
        .filter(|pc| *pc != u64::from(entry) && !reached.contains(pc))
        .collect()
}

type Lifts = Vec<(u64, Result<Block, LiftFailure>, u64)>;

/// A function to emit: entry, instructions, span and jump tables.
type Lifting = (Loc, Lifts, u64, BTreeMap<u64, Vec<u64>>);

/// Lifts every function of `report` (the analysis of `rom`) into one unit
/// and emits it; `image_id` identifies the ROM to the runtime.
#[must_use]
pub fn lift(rom: &[u8], report: &Report, image_id: [u8; 32]) -> Lifted {
    let calls: BTreeSet<u32> = report
        .functions
        .values()
        .flat_map(|f| f.linked.iter().map(|l| l.addr))
        .collect();
    let fe = Gba::with_calls(calls);
    let mut unlifted = Vec::new();
    let mut shadowed = Vec::new();
    let mut seen_entry = BTreeSet::new();
    let mut lifted: Vec<Lifting> = Vec::new();
    let mut instructions = BTreeSet::new();
    for (entry, f) in &report.functions {
        if f.instructions.is_empty() || !matches!(entry.addr >> 24, 0x02 | 0x03 | 0x08..=0x0D) {
            continue;
        }
        if !seen_entry.insert(entry.addr) {
            shadowed.push(*entry);
            continue;
        }
        let mut blocks: BTreeMap<u64, (Result<Block, LiftFailure>, u64)> = BTreeMap::new();
        let mut end = u64::from(entry.addr);
        for &l in f.instructions.keys() {
            let pc = u64::from(l.addr);
            if blocks.contains_key(&pc) {
                continue;
            }
            let (img, base) = image(rom, report, l);
            let r = fe.lift_at(img, base, l.addr, l.thumb);
            if let Err(e) = &r
                && !instructions.contains(&l)
            {
                unlifted.push(Unlifted {
                    at: l,
                    failure: e.clone(),
                });
            }
            instructions.insert(l);
            if l.addr >= entry.addr {
                end = end.max(pc + u64::from(l.size()));
            }
            blocks.insert(pc, (r, raw(img, base, l)));
        }
        let span = (end - u64::from(entry.addr)).max(2);
        let mut tables: BTreeMap<u64, Vec<u64>> = BTreeMap::new();
        for t in &f.tables {
            let v = tables.entry(u64::from(t.site.addr)).or_default();
            v.extend(t.targets.iter().map(|x| u64::from(x.addr)));
        }
        for e in &f.edges {
            if matches!(e.kind, EdgeKind::Tail | EdgeKind::Pointer) {
                tables
                    .entry(u64::from(e.site.addr))
                    .or_default()
                    .push(u64::from(e.target.addr));
            }
        }
        let landing = landings(&blocks, entry.addr);
        for (&pc, (b, _)) in &blocks {
            if let Ok(b) = b
                && matches!(b.exit.next, Next::Dynamic(_))
                && !landing.is_empty()
            {
                tables.entry(pc).or_default().extend(&landing);
            }
        }
        for v in tables.values_mut() {
            v.sort_unstable();
            v.dedup();
        }
        lifted.push((
            *entry,
            blocks.into_iter().map(|(pc, (r, w))| (pc, r, w)).collect(),
            span,
            tables,
        ));
    }
    let fns: Vec<Function<'_>> = lifted
        .iter()
        .map(|(entry, l, span, tables)| Function {
            entry: u64::from(entry.addr),
            name: host_name(*entry),
            guest_name: None,
            span: *span,
            insts: l
                .iter()
                .map(|(pc, r, raw)| {
                    let i = match r {
                        Ok(block) => Insn::Lifted { block, raw: *raw },
                        Err(e) => Insn::Failed(e),
                    };
                    (*pc, i)
                })
                .collect(),
            tables: tables.clone(),
            pads: BTreeSet::new(),
        })
        .collect();
    let opts = Options {
        unit_name: "rom".into(),
        unit_symbol: UNIT_SYMBOL.into(),
        io_map: device_map(),
        image_id,
        ..Options::default()
    };
    let output = relift_emit::emit(&fe, &fns, &[], &opts);
    Lifted {
        output,
        functions: fns.len(),
        instructions: instructions.len(),
        unlifted,
        shadowed,
    }
}

/// What the program file needs besides the unit.
#[derive(Clone, Debug)]
pub struct Program<'a> {
    /// The title, for reports.
    pub title: &'a str,
    /// The path of the imported ROM, which names the save; empty for none.
    pub rom_path: &'a str,
}

fn c_string(s: &str) -> String {
    let mut o = String::from("\"");
    for b in s.bytes() {
        match b {
            b'"' => o.push_str("\\\""),
            b'\\' => o.push_str("\\\\"),
            0x20..=0x7E => o.push(char::from(b)),
            _ => {
                let _ = write!(o, "\\{b:03o}");
            }
        }
    }
    o.push('"');
    o
}

/// `rom.c`, which embeds `rom.bin` (the ROM as the program runs it).
#[must_use]
pub fn rom_c() -> String {
    "#include <stddef.h>\n#include <stdint.h>\n#pragma clang diagnostic ignored \"-Wc23-extensions\"\nextern const uint8_t rl_gba_rom[];\nconst uint8_t rl_gba_rom[] = {\n#embed \"rom.bin\"\n};\nextern const size_t rl_gba_rom_len;\nconst size_t rl_gba_rom_len = sizeof rl_gba_rom;\n".into()
}

/// `program.c`: the unit list and `main`.
#[must_use]
pub fn program_c(p: &Program<'_>) -> String {
    format!(
        "#include <stddef.h>\n#include <stdint.h>\n#include <string.h>\n#include \"gbamain.h\"\nextern const rl_unit {UNIT_SYMBOL};\nextern const uint8_t rl_gba_rom[];\nextern const size_t rl_gba_rom_len;\nstatic const rl_unit *const units[] = {{ &{UNIT_SYMBOL} }};\nRL_TEXT_BOUNDS_DEFINE()\nextern int relift_info_main(void);\nint main(int argc, char **argv)\n{{\n    if (argc > 1 && strcmp(argv[1], \"--relift-info\") == 0)\n        return relift_info_main();\n    rl_gba_program prog = {{ rl_gba_rom, rl_gba_rom_len, units, 1u, {}, {} }};\n    return rl_gba_main(&prog, argc, argv);\n}}\n",
        c_string(p.title),
        c_string(p.rom_path)
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A cartridge: the header's branch to 0xC0, ARM code there that enters
    /// Thumb, a Thumb `BL` to a leaf, a SWI and a loop.
    fn rom() -> Vec<u8> {
        let mut r = vec![0u8; 0x200];
        r[0..4].copy_from_slice(&0xEA00_002Eu32.to_le_bytes());
        r[0xB2] = 0x96;
        let arm: [u32; 2] = [0xE28F_0001, 0xE12F_FF10];
        for (i, w) in arm.iter().enumerate() {
            r[0xC0 + 4 * i..0xC4 + 4 * i].copy_from_slice(&w.to_le_bytes());
        }
        let thumb: [u16; 4] = [0xF000, 0xF803, 0xDF05, 0xE7FD];
        for (i, h) in thumb.iter().enumerate() {
            r[0xC8 + 2 * i..0xCA + 2 * i].copy_from_slice(&h.to_le_bytes());
        }
        r[0xD2..0xD4].copy_from_slice(&0x4770u16.to_le_bytes());
        let sum = r[0xA0..0xBD]
            .iter()
            .fold(0u8, |a, b| a.wrapping_sub(*b))
            .wrapping_sub(0x19);
        r[0xBD] = sum;
        r
    }

    #[test]
    fn the_rom_lifts_into_one_unit_with_both_states() {
        let r = rom();
        let report = relift_gba_analyze::analyze(&r).expect("analyzes");
        let l = lift(&r, &report, [0; 32]);
        assert!(l.unlifted.is_empty(), "{:?}", l.unlifted);
        let c: String = l.output.files.iter().map(|f| f.text.as_str()).collect();
        assert!(c.contains("gba_a_08000000"), "{c}");
        assert!(c.contains("gba_t_080000d2"), "{c}");
        assert!(c.contains("rl_gba_swi_trap(c, mem, 0x105u)"), "{c}");
        assert!(c.contains("rl_arm32_call(c,"), "{c}");
    }

    #[test]
    fn the_program_lists_the_unit() {
        let c = program_c(&Program {
            title: "T\"",
            rom_path: "/r/a.gba",
        });
        assert!(c.contains("&rl_unit_gba_rom"), "{c}");
        assert!(c.contains("\"T\\\"\", \"/r/a.gba\""), "{c}");
    }
}
