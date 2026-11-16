//! A ROM as one lifted unit and the program files that run it.
//!
//! Every function `relift-gba-analyze` found becomes a C function at its
//! entry address; a function holds the instructions it reaches in their
//! state. Code in work RAM is lifted from the bytes of the copy the analysis
//! proved put it there, and each of its instructions checks those bytes as
//! the CPU fetches them. The runtime's dispatch finds a function by address
//! alone, so it holds the first function at each entry (the unit
//! [`UNIT_SYMBOL`]); the other functions in RAM go to units of their own
//! (`_v1`, `_v2`, ..., each with one function per address), and the program
//! lists every function in RAM by its entry and first unit, where code that
//! found other bytes goes on (`rl_gba_cpu_variant`). Code the game writes
//! there otherwise is missed code.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;

use relift_emit::{Function, Insn, Options, Output};
use relift_gba_analyze::{CopyKind, EdgeKind, Loc, Report};
use relift_guest::{Entry, LiftFailure};
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

/// A lifted function in work RAM, as the program lists it.
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct RamEntry {
    /// The entry, with bit 0 set in Thumb state.
    pub at: u32,
    /// The instruction unit at the entry it was lifted from.
    pub unit: u32,
    /// The host function.
    pub name: String,
}

/// Everything a ROM lifts to.
#[derive(Clone, Debug)]
pub struct Lifted {
    /// The emitted files of the unit the dispatch holds.
    pub output: Output,
    /// The emitted files of the units of the other functions in RAM, the
    /// `k`th one named `rom_v{k + 1}`.
    pub variants: Vec<Output>,
    /// Every function in RAM, sorted.
    pub ram: Vec<RamEntry>,
    /// Functions in the unit.
    pub functions: usize,
    /// Instructions lifted.
    pub instructions: usize,
    /// Instructions that did not lift.
    pub unlifted: Vec<Unlifted>,
    /// Entries left out because another function has the address (in RAM:
    /// with the same first unit).
    pub shadowed: Vec<Loc>,
}

/// The C symbol of the unit of variant layer `k` (1 and up).
#[must_use]
pub fn variant_symbol(k: usize) -> String {
    format!("{UNIT_SYMBOL}_v{k}")
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

/// The instructions of a function that neither its entry nor its jump
/// tables reach by falling through or by constant branches: where its
/// computed jumps can land inside it (the analysis follows them; the
/// emitter turns each into a case of the jump's switch, with dispatch for
/// any other target). A block that only branches to itself is a landing.
fn landings(
    blocks: &BTreeMap<u64, (Result<Block, LiftFailure>, u64)>,
    entry: u32,
    tables: &BTreeMap<u64, Vec<u64>>,
) -> Vec<u64> {
    let succ = |pc: u64| -> Vec<u64> {
        let Some((Ok(b), _)) = blocks.get(&pc) else {
            return Vec::new();
        };
        let mut out = match b.exit.next {
            Next::Const(t) => vec![t],
            Next::Select {
                taken, not_taken, ..
            } => vec![taken, not_taken],
            Next::Dynamic(_) => Vec::new(),
        };
        if !matches!(b.exit.flow, Flow::Jump)
            || b.insts
                .iter()
                .any(|i| matches!(i.op, relift_ir::Op::Guard(..)))
        {
            out.push(pc + u64::from(b.len));
        }
        out
    };
    let mut reached: BTreeSet<u64> = tables.values().flatten().copied().collect();
    reached.insert(u64::from(entry));
    let mut todo: Vec<u64> = reached.iter().copied().collect();
    while let Some(pc) = todo.pop() {
        for t in succ(pc) {
            if blocks.contains_key(&t) && reached.insert(t) {
                todo.push(t);
            }
        }
    }
    blocks
        .keys()
        .copied()
        .filter(|pc| !reached.contains(pc))
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
    let mut layers: BTreeMap<u32, usize> = BTreeMap::new();
    let mut keys = BTreeSet::new();
    let mut lifted: Vec<(usize, Lifting)> = Vec::new();
    let mut ram = Vec::new();
    let mut instructions = BTreeSet::new();
    let rewrite = |l: Loc| {
        l.copy
            .and_then(|c| report.copies.get(usize::from(c)))
            .is_some_and(|c| matches!(c.kind, CopyKind::Rewrite(_)))
    };
    let mut order: Vec<(&Loc, &relift_gba_analyze::Function)> = report.functions.iter().collect();
    order.sort_by_key(|(l, _)| rewrite(**l));
    for (entry, f) in order {
        if f.instructions.is_empty() || !matches!(entry.addr >> 24, 0x02 | 0x03 | 0x08..=0x0D) {
            continue;
        }
        let in_ram = crate::frontend::in_ram(entry.addr);
        let first = if in_ram {
            let (img, base) = image(rom, report, *entry);
            let unit = raw(img, base, *entry) as u32;
            if !keys.insert((entry.addr, entry.thumb, unit)) {
                shadowed.push(*entry);
                continue;
            }
            Some(unit)
        } else {
            None
        };
        let layer = layers.entry(entry.addr).or_insert(0);
        if *layer > 0 && !in_ram {
            shadowed.push(*entry);
            continue;
        }
        let k = *layer;
        *layer += 1;
        if let Some(unit) = first {
            ram.push(RamEntry {
                at: entry.addr | u32::from(entry.thumb),
                unit,
                name: host_name(*entry),
            });
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
        let landing = landings(&blocks, entry.addr, &tables);
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
            k,
            (
                *entry,
                blocks.into_iter().map(|(pc, (r, w))| (pc, r, w)).collect(),
                span,
                tables,
            ),
        ));
    }
    ram.sort();
    let layer_count = lifted.iter().map(|(k, _)| k + 1).max().unwrap_or(1);
    let mut outputs = (0..layer_count).map(|k| {
        let fns = functions(lifted.iter().filter(|(l, _)| *l == k).map(|(_, f)| f));
        let opts = Options {
            unit_name: if k == 0 {
                "rom".into()
            } else {
                format!("rom_v{k}")
            },
            unit_symbol: if k == 0 {
                UNIT_SYMBOL.into()
            } else {
                variant_symbol(k)
            },
            io_map: device_map(),
            image_id,
            ..Options::default()
        };
        (fns.len(), relift_emit::emit(&fe, &fns, &[], &opts))
    });
    let (functions, output) = outputs.next().unwrap_or_default();
    let variants: Vec<(usize, Output)> = outputs.collect();
    Lifted {
        output,
        functions: functions + variants.iter().map(|v| v.0).sum::<usize>(),
        variants: variants.into_iter().map(|v| v.1).collect(),
        ram,
        instructions: instructions.len(),
        unlifted,
        shadowed,
    }
}

/// The functions to emit for `lifted`.
fn functions<'a>(lifted: impl Iterator<Item = &'a Lifting>) -> Vec<Function<'a>> {
    lifted
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
            guesses: Default::default(),
            pads: BTreeSet::new(),
        })
        .collect()
}

/// What the program file needs besides the unit.
#[derive(Clone, Debug)]
pub struct Program<'a> {
    /// The title, for reports.
    pub title: &'a str,
    /// The path of the imported ROM, which names the save; empty for none.
    pub rom_path: &'a str,
    /// The save file the program reads and writes unless told otherwise,
    /// before the one `rom_path` names: a path for [`Entry::Main`], a file
    /// name in the app's own folder otherwise; empty for none.
    pub save: &'a str,
    /// How the program enters.
    pub entry: Entry,
    /// The units of the other functions in RAM ([`Lifted::variants`]).
    pub variant_units: usize,
    /// Every function in RAM ([`Lifted::ram`]).
    pub ram: &'a [RamEntry],
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
pub fn rom_c(rom: &[u8]) -> String {
    let mut t = relift_guest::blob::blob_c("rl_gba_rom", "c/rom.bin", rom);
    t.push_str("#include <stddef.h>\nextern const size_t rl_gba_rom_len;\n");
    let _ = writeln!(t, "const size_t rl_gba_rom_len = {}u;", rom.len());
    t
}

/// `program.c`: the unit list and `main`.
#[must_use]
pub fn program_c(p: &Program<'_>) -> String {
    let symbols: Vec<String> = std::iter::once(UNIT_SYMBOL.to_owned())
        .chain((1..=p.variant_units).map(variant_symbol))
        .collect();
    let mut s = String::from(
        "#include <stddef.h>\n#include <stdint.h>\n#include <string.h>\n#include \"gbamain.h\"\n",
    );
    for u in &symbols {
        let _ = writeln!(s, "extern const rl_unit {u};");
    }
    for e in p.ram {
        let _ = writeln!(s, "extern RL_LIFTED({});", e.name);
    }
    let units: Vec<String> = symbols.iter().map(|u| format!("&{u}")).collect();
    let _ = write!(
        s,
        "extern const uint8_t rl_gba_rom[];\nextern const size_t rl_gba_rom_len;\nstatic const rl_unit *const units[] = {{ {} }};\n",
        units.join(", ")
    );
    s.push_str("static const rl_gba_variant ram[] = {\n");
    for e in p.ram {
        let _ = writeln!(s, "    {{ {:#x}u, {:#x}u, {} }},", e.at, e.unit, e.name);
    }
    if p.ram.is_empty() {
        s.push_str("    { 0u, 0u, NULL },\n");
    }
    s.push_str("};\nRL_TEXT_BOUNDS_DEFINE()\nextern int relift_info_main(void);\n");
    let mobile = |s: &mut String, header: &str, sig: &str, init: &str, dir: &str| {
        let _ = write!(
            s,
            "#include <stdio.h>\n#include <relift/{header}>\n#include <relift/settings.h>\n{sig}\n{{\n    {init}();\n    rl_settings_set_app(\"relift\");\n    if (argc > 1 && strcmp(argv[1], \"--relift-info\") == 0)\n        return relift_info_main();\n    const char *dir = {dir}();\n    char save[4096];\n    snprintf(save, sizeof save, \"%s/%s\", dir ? dir : \".\", {});\n",
            c_string(p.save)
        );
    };
    match p.entry {
        Entry::Android => mobile(
            &mut s,
            "android.h",
            "__attribute__((visibility(\"default\"))) int SDL_main(int argc, char **argv)",
            "rl_android_init",
            "rl_android_files_dir",
        ),
        Entry::Ios => mobile(
            &mut s,
            "ios.h",
            "int rl_ios_main(int argc, char **argv)",
            "rl_ios_init",
            "rl_ios_documents_dir",
        ),
        Entry::Main => s.push_str(
            "int main(int argc, char **argv)\n{\n    if (argc > 1 && strcmp(argv[1], \"--relift-info\") == 0)\n        return relift_info_main();\n",
        ),
    }
    let _ = write!(
        s,
        "    rl_gba_program prog = {{ rl_gba_rom, rl_gba_rom_len, units, {}u, {}, {}, {}, ram, {}u }};\n    int status = rl_gba_main(&prog, argc, argv);\n{}    return status;\n}}\n",
        symbols.len(),
        c_string(p.title),
        c_string(p.rom_path),
        match (p.entry, p.save.is_empty()) {
            (_, true) => "\"\"".into(),
            (Entry::Main, false) => c_string(p.save),
            _ => "save".into(),
        },
        p.ram.len(),
        match p.entry {
            Entry::Android => "    rl_android_flush();\n",
            Entry::Ios => "    rl_ios_flush();\n",
            Entry::Main => "",
        }
    );
    s
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
    fn a_loop_on_itself_after_a_computed_jump_is_a_landing() {
        let base = 0x0800_0000u32;
        let mut img = vec![0u8; 0x20];
        img[0..4].copy_from_slice(&0xE12F_FF10u32.to_le_bytes());
        img[4..8].copy_from_slice(&0xEAFF_FFFEu32.to_le_bytes());
        let fe = Gba::with_calls(BTreeSet::new());
        let mut blocks = BTreeMap::new();
        for pc in [base, base + 4] {
            blocks.insert(u64::from(pc), (fe.lift_at(&img, base, pc, false), 0));
        }
        assert_eq!(
            landings(&blocks, base, &BTreeMap::new()),
            [u64::from(base) + 4]
        );
        let tables = BTreeMap::from([(u64::from(base), vec![u64::from(base) + 4])]);
        assert!(landings(&blocks, base, &tables).is_empty());
    }

    #[test]
    fn the_program_lists_the_unit() {
        let c = program_c(&Program {
            title: "T\"",
            rom_path: "/r/a.gba",
            save: "",
            entry: Entry::Main,
            variant_units: 0,
            ram: &[],
        });
        assert!(c.contains("units[] = { &rl_unit_gba_rom }"), "{c}");
        assert!(
            c.contains("units, 1u, \"T\\\"\", \"/r/a.gba\", \"\", ram, 0u"),
            "{c}"
        );
        assert!(c.contains("int main(int argc, char **argv)"), "{c}");
    }

    #[test]
    fn an_app_program_enters_from_the_glue_with_its_save_in_the_app_folder() {
        let android = program_c(&Program {
            title: "T",
            rom_path: "",
            save: "battery.sav",
            entry: Entry::Android,
            variant_units: 0,
            ram: &[],
        });
        assert!(
            android.contains("int SDL_main(int argc, char **argv)"),
            "{android}"
        );
        assert!(android.contains("rl_android_init();"), "{android}");
        assert!(
            android.contains("\"%s/%s\", dir ? dir : \".\", \"battery.sav\")"),
            "{android}"
        );
        assert!(android.contains("\"T\", \"\", save, ram, 0u"), "{android}");
        assert!(android.contains("rl_android_flush();"), "{android}");
        assert!(!android.contains("int main("), "{android}");
        let ios = program_c(&Program {
            title: "T",
            rom_path: "",
            save: "battery.sav",
            entry: Entry::Ios,
            variant_units: 0,
            ram: &[],
        });
        assert!(
            ios.contains("int rl_ios_main(int argc, char **argv)"),
            "{ios}"
        );
        assert!(ios.contains("rl_ios_documents_dir()"), "{ios}");
        assert!(ios.contains("rl_ios_flush();"), "{ios}");
    }

    #[test]
    fn the_program_lists_the_variant_units_and_every_function_in_ram() {
        let ram = [
            RamEntry {
                at: 0x0300_0000,
                unit: 0xE3A0_0001,
                name: "gba_a_03000000_c0".into(),
            },
            RamEntry {
                at: 0x0300_0000,
                unit: 0xE12F_FF1E,
                name: "gba_a_03000000_c1".into(),
            },
        ];
        let c = program_c(&Program {
            title: "",
            rom_path: "",
            save: "",
            entry: Entry::Main,
            variant_units: 1,
            ram: &ram,
        });
        assert!(
            c.contains("units[] = { &rl_unit_gba_rom, &rl_unit_gba_rom_v1 }"),
            "{c}"
        );
        assert!(c.contains("extern RL_LIFTED(gba_a_03000000_c1);"), "{c}");
        assert!(
            c.contains("    { 0x3000000u, 0xe12fff1eu, gba_a_03000000_c1 },"),
            "{c}"
        );
        assert!(c.contains("units, 2u, \"\", \"\", \"\", ram, 2u"), "{c}");
    }
}
