//! A ROM as one lifted unit and the program files that run it.
//!
//! Every function the discovery found becomes a C function at its entry
//! address; a function holds the instructions it reaches in their state.
//! The runtime finds a function by address alone, so an address that is
//! the entry in both states keeps the first one found (ARM before Thumb).

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;

use relift_emit::{Function, Insn, Options, Output};
use relift_guest::LiftFailure;
use relift_ir::Block;

use crate::discover::{Loc, Report};
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
}

/// The host name of the function at `l`.
#[must_use]
pub fn host_name(l: Loc) -> String {
    format!("gba_{}_{:08x}", if l.thumb { 't' } else { 'a' }, l.addr)
}

fn raw(rom: &[u8], l: Loc) -> u64 {
    let off = (l.addr - ROM_BASE) as usize;
    let n = l.size() as usize;
    let v = rom
        .get(off..off + n)
        .unwrap_or_default()
        .iter()
        .rev()
        .fold(0u64, |a, b| (a << 8) | u64::from(*b));
    if l.thumb { v | RAW_THUMB } else { v }
}

type Lifts = Vec<(u64, Result<Block, LiftFailure>, u64)>;

/// A function to emit: entry, instructions, span and jump tables.
type Lifting = (Loc, Lifts, u64, BTreeMap<u64, Vec<u64>>);

/// Lifts every function of `report` (a discovery of `rom`) into one unit
/// and emits it; `image_id` identifies the ROM to the runtime.
#[must_use]
pub fn lift(rom: &[u8], report: &Report, image_id: [u8; 32]) -> Lifted {
    let calls: BTreeSet<u32> = report
        .functions
        .values()
        .flat_map(|f| f.calls.iter().map(|l| l.addr))
        .collect();
    let fe = Gba::with_calls(calls);
    let mut unlifted = Vec::new();
    let mut seen_entry = BTreeSet::new();
    let mut lifted: Vec<Lifting> = Vec::new();
    let mut instructions = BTreeSet::new();
    for (entry, f) in &report.functions {
        if !seen_entry.insert(entry.addr) || (entry.addr >> 24) < 0x08 {
            continue;
        }
        let mut blocks: BTreeMap<u64, (Result<Block, LiftFailure>, u64)> = BTreeMap::new();
        let mut end = u64::from(entry.addr);
        for &l in &f.instructions {
            let pc = u64::from(l.addr);
            if blocks.contains_key(&pc) {
                continue;
            }
            let r = fe.lift_at(rom, ROM_BASE, l.addr, l.thumb);
            if let Err(e) = &r
                && instructions.insert(l)
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
            blocks.insert(pc, (r, raw(rom, l)));
        }
        let span = (end - u64::from(entry.addr)).max(2);
        let tables = f
            .jumps
            .iter()
            .map(|(&at, ts)| (u64::from(at), ts.iter().map(|&t| u64::from(t)).collect()))
            .collect();
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
    }
}

/// What the program file needs besides the unit.
#[derive(Clone, Debug)]
pub struct Program<'a> {
    /// The title, for reports.
    pub title: &'a str,
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
        "#include <stddef.h>\n#include <stdint.h>\n#include <string.h>\n#include \"gbamain.h\"\nextern const rl_unit {UNIT_SYMBOL};\nextern const uint8_t rl_gba_rom[];\nextern const size_t rl_gba_rom_len;\nstatic const rl_unit *const units[] = {{ &{UNIT_SYMBOL} }};\nRL_TEXT_BOUNDS_DEFINE()\nextern int relift_info_main(void);\nint main(int argc, char **argv)\n{{\n    if (argc > 1 && strcmp(argv[1], \"--relift-info\") == 0)\n        return relift_info_main();\n    rl_gba_program prog = {{ rl_gba_rom, rl_gba_rom_len, units, 1u, {} }};\n    return rl_gba_main(&prog, argc, argv);\n}}\n",
        c_string(p.title)
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rom() -> Vec<u8> {
        let mut r = vec![0u8; 0x200];
        let arm: [u32; 4] = [0xE28F_0001, 0xE12F_FF10, 0, 0];
        for (i, w) in arm.iter().enumerate() {
            r[4 * i..4 * i + 4].copy_from_slice(&w.to_le_bytes());
        }
        let thumb: [u16; 4] = [0xF000, 0xF803, 0xDF05, 0xE7FD];
        for (i, h) in thumb.iter().enumerate() {
            r[8 + 2 * i..10 + 2 * i].copy_from_slice(&h.to_le_bytes());
        }
        r[0x12..0x14].copy_from_slice(&0x4770u16.to_le_bytes());
        r
    }

    #[test]
    fn the_rom_lifts_into_one_unit_with_both_states() {
        let r = rom();
        let report = crate::discover::discover(&r);
        let l = lift(&r, &report, [0; 32]);
        assert!(l.unlifted.is_empty(), "{:?}", l.unlifted);
        assert_eq!(l.functions, 2);
        let c: String = l.output.files.iter().map(|f| f.text.as_str()).collect();
        assert!(c.contains("gba_a_08000000"), "{c}");
        assert!(c.contains("gba_t_08000012"), "{c}");
        assert!(c.contains("rl_gba_swi_trap(c, mem, 0x105u)"), "{c}");
        assert!(c.contains("rl_arm32_call(c,"), "{c}");
        assert!(c.contains("case UINT64_C(0x8000008): goto"), "{c}");
    }

    #[test]
    fn the_program_lists_the_unit() {
        let c = program_c(&Program { title: "T\"" });
        assert!(c.contains("&rl_unit_gba_rom"), "{c}");
        assert!(c.contains("\"T\\\"\""), "{c}");
    }
}
