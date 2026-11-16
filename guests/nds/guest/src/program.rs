//! A cartridge's units lifted into C units and the program files that run
//! them.
//!
//! Every unit `relift-nds-analyze` reads (each processor's static binary
//! with its code autoload blocks, each overlay) becomes a C unit of its own,
//! named by its id and the hash of its bytes. A function found in a unit's
//! run belongs to the unit that holds its entry; the static binary's code
//! an overlay run finds joins the static unit. The first instruction of
//! every function checks, as the CPU enters it, that memory holds the bytes
//! it was lifted from; when it does not (another overlay is loaded there,
//! the game rewrote the code), the runtime goes on in the function of
//! another unit lifted from the bytes there, or stops with a report of the
//! CPU, the address and the bytes. Code no unit holds is never
//! interpreted: it stops the program the same way.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;

use relift_emit::{Function, Insn, Options, Output};
use relift_guest::LiftFailure;
use relift_ir::{Block, Flow, Next};
use relift_nds_analyze::{EdgeKind, Loc, Program as Units, Report, UnitId, UnitReport};
use relift_nds_formats::Cpu;
use relift_nds_formats::checksum::{hex, sha256};

use crate::frontend::{Entry, Nds, RAW_THUMB, device_map};

/// An instruction that did not lift.
#[derive(Clone, Debug)]
pub struct Unlifted {
    /// The unit.
    pub unit: UnitId,
    /// Where.
    pub at: Loc,
    /// Why.
    pub failure: LiftFailure,
}

/// A lifted function as the runtime checks it at entry.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Checked {
    /// The entry, with bit 0 set in Thumb state.
    pub at: u32,
    /// The bytes it was lifted from: where they start in the unit's blob
    /// and how many (from the entry, aligned).
    pub offset: u32,
    /// The length.
    pub len: u32,
    /// The host function.
    pub name: String,
}

/// One lifted unit.
#[derive(Clone, Debug)]
pub struct LiftedUnit {
    /// Which unit.
    pub id: UnitId,
    /// The directory of its C files and the stem of its symbols: the unit's
    /// name and the first bytes of its hash.
    pub dir: String,
    /// The C symbol of its `rl_unit`.
    pub symbol: String,
    /// The emitted files.
    pub output: Output,
    /// The functions, in the order of the entry checks' indices.
    pub functions: Vec<Checked>,
    /// The bytes the entry checks compare.
    pub blob: Vec<u8>,
}

/// Everything a cartridge lifts to.
#[derive(Clone, Debug)]
pub struct Lifted {
    /// The units, the static binaries first.
    pub units: Vec<LiftedUnit>,
    /// Instructions lifted.
    pub instructions: usize,
    /// Instructions that did not lift.
    pub unlifted: Vec<Unlifted>,
    /// Entries left out because another function of the unit has the
    /// address.
    pub shadowed: Vec<(UnitId, Loc)>,
}

impl Lifted {
    /// Functions in all units.
    #[must_use]
    pub fn functions(&self) -> usize {
        self.units.iter().map(|u| u.functions.len()).sum()
    }
}

/// The short name of a unit in symbols and directories.
#[must_use]
pub fn tag(id: UnitId) -> String {
    id.name().replace('-', "_")
}

/// The letter of a CPU in host names.
fn cpu_digit(cpu: Cpu) -> char {
    match cpu {
        Cpu::Arm9 => '9',
        Cpu::Arm7 => '7',
    }
}

/// The host name of the function at `l` in unit `id`.
#[must_use]
pub fn host_name(id: UnitId, l: Loc) -> String {
    let copy = l.copy.map(|c| format!("_c{c}")).unwrap_or_default();
    format!(
        "nds{}_{}_{}_{:08x}{copy}",
        cpu_digit(id.cpu()),
        tag(id),
        if l.thumb { 't' } else { 'a' },
        l.addr
    )
}

/// The images a run lifts from: base and bytes.
type Images<'a> = Vec<(u32, &'a [u8])>;

/// The bytes at `l` and the address they start at: the run's RAM copy, or
/// the image that holds it.
fn image<'a>(images: &Images<'a>, report: &'a Report, l: Loc) -> Option<(&'a [u8], u32)> {
    if let Some(c) = l.copy.and_then(|c| report.copies.get(usize::from(c))) {
        return Some((&c.bytes, c.dst));
    }
    images.iter().find_map(|&(b, d)| {
        let off = l.addr.checked_sub(b)? as usize;
        (off < d.len()).then_some((d, b))
    })
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

type Blocks = BTreeMap<u64, (Result<Block, LiftFailure>, u64)>;

/// The instructions of a function that neither its entry nor its jump
/// tables reach by falling through or by constant branches: where its
/// computed jumps can land inside it. A block that only branches to itself
/// is a landing.
fn landings(blocks: &Blocks, entry: u32, tables: &BTreeMap<u64, Vec<u64>>) -> Vec<u64> {
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

/// A function to emit: entry, instructions, span and jump tables.
struct Lifting {
    entry: Loc,
    insts: Vec<(u64, Result<Block, LiftFailure>, u64)>,
    span: u64,
    tables: BTreeMap<u64, Vec<u64>>,
}

/// What one unit collects before it is emitted.
struct Pending<'a> {
    id: UnitId,
    hash: [u8; 32],
    /// The functions: entry, the run that found it and its images.
    fns: BTreeMap<(u32, bool), (Loc, &'a Report, Images<'a>)>,
}

/// Lifts every unit of `units`, as `reports` (its analysis) found them;
/// `image_id` identifies the cartridge to the runtime.
#[must_use]
pub fn lift(units: &Units, reports: &[UnitReport], image_id: [u8; 32]) -> Lifted {
    let mut pending: Vec<Pending<'_>> = units
        .units
        .iter()
        .map(|u| Pending {
            id: u.id,
            hash: u.sha256.unwrap_or_else(|| {
                let p = units.processors.iter().find(|p| p.cpu == u.id.cpu());
                let mut all = Vec::new();
                for (b, d) in p.map(|p| p.segments.as_slice()).unwrap_or_default() {
                    all.extend_from_slice(&b.to_le_bytes());
                    all.extend_from_slice(d);
                }
                sha256(&all)
            }),
            fns: BTreeMap::new(),
        })
        .collect();
    let mut shadowed = Vec::new();
    for r in reports {
        let Some(ui) = units.units.iter().position(|u| u.id == r.id) else {
            continue;
        };
        let cpu = r.id.cpu();
        let Some(p) = units.processors.iter().find(|p| p.cpu == cpu) else {
            continue;
        };
        let statics: Images<'_> = p.segments.iter().map(|(b, d)| (*b, d.as_slice())).collect();
        let mut images = statics.clone();
        if let UnitId::Overlay(..) = r.id {
            let u = &units.units[ui];
            images.push((u.base, u.bytes.as_slice()));
        }
        let static_unit = units.units.iter().position(|u| u.id == UnitId::Static(cpu));
        for (entry, f) in &r.report.functions {
            if f.instructions.is_empty() {
                continue;
            }
            let in_static = statics
                .iter()
                .any(|(b, d)| entry.addr.wrapping_sub(*b) < d.len() as u32);
            let (lo, hi) = r.range;
            let owner = match (r.id, entry.copy) {
                (UnitId::Static(_), _) | (UnitId::Overlay(..), Some(_)) => ui,
                (UnitId::Overlay(..), None) if (lo..hi).contains(&entry.addr) => ui,
                (UnitId::Overlay(..), None) if in_static => match static_unit {
                    Some(s) => s,
                    None => continue,
                },
                (UnitId::Overlay(..), None) => continue,
            };
            if image(&images, &r.report, *entry).is_none() {
                continue;
            }
            let key = (entry.addr, entry.thumb);
            let slot = &mut pending[owner].fns;
            if let Some((had, ..)) = slot.get(&key) {
                if had != entry {
                    shadowed.push((pending[owner].id, *entry));
                }
                continue;
            }
            slot.insert(key, (*entry, &r.report, images.clone()));
        }
    }
    let mut unlifted = Vec::new();
    let mut instructions = BTreeSet::new();
    let mut out = Vec::new();
    for (k, p) in pending.iter().enumerate() {
        let cpu = p.id.cpu();
        let calls: BTreeSet<u32> = p
            .fns
            .values()
            .flat_map(|(e, r, _)| {
                r.functions
                    .get(e)
                    .into_iter()
                    .flat_map(|f| f.linked.iter().map(|l| l.addr))
            })
            .collect();
        let fe = Nds::with_calls(cpu, calls);
        let mut liftings = Vec::new();
        let mut checks = Vec::new();
        let mut blob = Vec::new();
        for (index, (entry, report, images)) in p.fns.values().enumerate() {
            let f = &report.functions[entry];
            let mut blocks: Blocks = BTreeMap::new();
            let mut end = u64::from(entry.addr);
            for &l in f.instructions.keys() {
                let pc = u64::from(l.addr);
                if blocks.contains_key(&pc) {
                    continue;
                }
                let at_entry = l.addr == entry.addr && l.thumb == entry.thumb;
                let check = at_entry.then_some(Entry {
                    unit: k as u32,
                    function: index as u32,
                });
                let r = match image(images, report, l) {
                    Some((img, base)) => (
                        fe.lift_at(img, base, l.addr, l.thumb, check),
                        raw(img, base, l),
                    ),
                    None => (
                        Err(LiftFailure {
                            insn: 0,
                            message: format!("{:#010x} has no bytes", l.addr),
                        }),
                        0,
                    ),
                };
                let first = instructions.insert((p.id.name(), l));
                if let Err(e) = &r.0
                    && first
                {
                    unlifted.push(Unlifted {
                        unit: p.id,
                        at: l,
                        failure: e.clone(),
                    });
                }
                if l.addr >= entry.addr {
                    end = end.max(pc + u64::from(l.size()));
                }
                blocks.insert(pc, r);
            }
            let span = (end - u64::from(entry.addr)).max(2);
            if let Some((img, base)) = image(images, report, *entry) {
                let off = (entry.addr - base) as usize;
                let n = (span as usize).min(img.len() - off);
                checks.push(Checked {
                    at: entry.addr | u32::from(entry.thumb),
                    offset: blob.len() as u32,
                    len: n as u32,
                    name: host_name(p.id, *entry),
                });
                blob.extend_from_slice(&img[off..off + n]);
            }
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
            liftings.push(Lifting {
                entry: *entry,
                insts: blocks.into_iter().map(|(pc, (r, w))| (pc, r, w)).collect(),
                span,
                tables,
            });
        }
        let dir = format!("{}_{}", tag(p.id), &hex(&p.hash)[..12]);
        let symbol = format!("rl_unit_nds_{dir}");
        let fns = functions(p.id, &liftings);
        let opts = Options {
            unit_name: dir.clone(),
            unit_symbol: symbol.clone(),
            io_map: device_map(),
            image_id,
            ..Options::default()
        };
        let output = relift_emit::emit(&fe, &fns, &[], &opts);
        out.push(LiftedUnit {
            id: p.id,
            dir,
            symbol,
            output,
            functions: checks,
            blob,
        });
    }
    Lifted {
        units: out,
        instructions: instructions.len(),
        unlifted,
        shadowed,
    }
}

/// The functions to emit for `lifted`.
fn functions(id: UnitId, lifted: &[Lifting]) -> Vec<Function<'_>> {
    lifted
        .iter()
        .map(|l| Function {
            entry: u64::from(l.entry.addr),
            name: host_name(id, l.entry),
            guest_name: None,
            span: l.span,
            insts: l
                .insts
                .iter()
                .map(|(pc, r, raw)| {
                    let i = match r {
                        Ok(block) => Insn::Lifted { block, raw: *raw },
                        Err(e) => Insn::Failed(e),
                    };
                    (*pc, i)
                })
                .collect(),
            tables: l.tables.clone(),
            guesses: BTreeMap::new(),
            pads: BTreeSet::new(),
        })
        .collect()
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

/// The file name of unit `k`'s blob next to `data.c`.
#[must_use]
pub fn blob_file(k: usize) -> String {
    format!("unit{k}.bin")
}

/// `data.c`, which embeds `rom.bin` (the cartridge) and each unit's blob
/// (`unit<k>.bin`, [`blob_file`]).
#[must_use]
pub fn data_c(units: usize) -> String {
    let mut s = String::from(
        "#include <stddef.h>\n#include <stdint.h>\n#pragma clang diagnostic ignored \"-Wc23-extensions\"\nextern const uint8_t rl_nds_rom[];\nconst uint8_t rl_nds_rom[] = {\n#embed \"rom.bin\"\n};\nextern const size_t rl_nds_rom_len;\nconst size_t rl_nds_rom_len = sizeof rl_nds_rom;\n",
    );
    for k in 0..units {
        let _ = write!(
            s,
            "extern const uint8_t rl_nds_blob{k}[];\nconst uint8_t rl_nds_blob{k}[] = {{\n#embed \"{}\" if_empty(0)\n}};\n",
            blob_file(k)
        );
    }
    s
}

/// `program.c`: the units with their entry checks and `main`, which runs
/// with the options `args` before the command line's.
#[must_use]
pub fn program_c(title: &str, units: &[LiftedUnit], args: &[String]) -> String {
    let mut s = String::from(
        "#include <stddef.h>\n#include <stdint.h>\n#include <string.h>\n#include \"ndsmain.h\"\n",
    );
    for (k, u) in units.iter().enumerate() {
        let _ = writeln!(s, "extern const rl_unit {};", u.symbol);
        let _ = writeln!(s, "extern const uint8_t rl_nds_blob{k}[];");
        for f in &u.functions {
            let _ = writeln!(s, "extern RL_LIFTED({});", f.name);
        }
    }
    for (k, u) in units.iter().enumerate() {
        let _ = writeln!(s, "static const rl_nds_fn fns{k}[] = {{");
        for f in &u.functions {
            let _ = writeln!(
                s,
                "    {{ {:#x}u, {:#x}u, {:#x}u, {} }},",
                f.at, f.offset, f.len, f.name
            );
        }
        if u.functions.is_empty() {
            s.push_str("    { 0u, 0u, 0u, NULL },\n");
        }
        s.push_str("};\n");
    }
    s.push_str("static const rl_nds_unit units[] = {\n");
    for (k, u) in units.iter().enumerate() {
        let _ = writeln!(
            s,
            "    {{ {}, &{}, rl_nds_blob{k}, fns{k}, {}u, {} }},",
            match u.id.cpu() {
                Cpu::Arm9 => "RL_NDS_ARM9",
                Cpu::Arm7 => "RL_NDS_ARM7",
            },
            u.symbol,
            u.functions.len(),
            c_string(&u.dir)
        );
    }
    s.push_str("};\nstatic const char *const args[] = {\n");
    for a in args {
        let _ = writeln!(s, "    {},", c_string(a));
    }
    s.push_str("    NULL,\n};\n");
    let _ = write!(
        s,
        "extern const uint8_t rl_nds_rom[];\nextern const size_t rl_nds_rom_len;\nRL_TEXT_BOUNDS_DEFINE()\nextern int relift_info_main(void);\nint main(int argc, char **argv)\n{{\n    if (argc > 1 && strcmp(argv[1], \"--relift-info\") == 0)\n        return relift_info_main();\n    rl_nds_program prog = {{ rl_nds_rom, rl_nds_rom_len, units, {}u, {}, args, {}u }};\n    return rl_nds_main(&prog, argc, argv);\n}}\n",
        units.len(),
        c_string(title),
        args.len()
    );
    s
}

#[cfg(test)]
mod tests {
    use super::*;
    use relift_nds_formats::synth;

    fn arm(words: &[u32]) -> Vec<u8> {
        words.iter().flat_map(|w| w.to_le_bytes()).collect()
    }

    /// A cartridge whose ARM9 code is `code9` at `0x02000000` and ARM7
    /// code `code7` at `0x02380000`, both entered at their start.
    fn rom(code9: &[u8], code7: &[u8]) -> Vec<u8> {
        let mut r = vec![0u8; 0x200];
        let off9 = r.len();
        r.extend_from_slice(code9);
        r.resize(r.len().next_multiple_of(0x200), 0);
        let off7 = r.len();
        r.extend_from_slice(code7);
        r.resize(r.len().next_multiple_of(0x200), 0);
        for (at, off, ram, len) in [
            (0x20, off9, 0x0200_0000u32, code9.len()),
            (0x30, off7, 0x0238_0000, code7.len()),
        ] {
            r[at..at + 4].copy_from_slice(&(off as u32).to_le_bytes());
            r[at + 4..at + 8].copy_from_slice(&ram.to_le_bytes());
            r[at + 8..at + 12].copy_from_slice(&ram.to_le_bytes());
            r[at + 12..at + 16].copy_from_slice(&(len as u32).to_le_bytes());
        }
        let total = r.len() as u32;
        r[0x80..0x84].copy_from_slice(&total.to_le_bytes());
        r[0x84..0x88].copy_from_slice(&0x200u32.to_le_bytes());
        synth::fix_crcs(&mut r);
        r
    }

    fn lifted(r: &[u8]) -> Lifted {
        let rom = relift_nds_formats::Rom::parse(r).expect("parses");
        let units = Units::read(&rom).expect("reads");
        let reports = units.analyze(1, &|_| {});
        lift(&units, &reports, [0; 32])
    }

    #[test]
    fn each_static_binary_is_a_unit_whose_entries_check_their_bytes() {
        let code9 = arm(&[0xE3A0_0001, 0xEB00_0000, 0xEAFF_FFFE, 0xE12F_FF1E]);
        let code7 = arm(&[0xEF06_0000, 0xEAFF_FFFE]);
        let l = lifted(&rom(&code9, &code7));
        assert!(l.unlifted.is_empty(), "{:?}", l.unlifted);
        assert_eq!(l.units.len(), 2);
        let (u9, u7) = (&l.units[0], &l.units[1]);
        assert_eq!(u9.id, UnitId::Static(Cpu::Arm9));
        assert!(u9.dir.starts_with("static_"), "{}", u9.dir);
        assert!(u7.dir.starts_with("arm7_"), "{}", u7.dir);
        let names: Vec<&str> = u9.functions.iter().map(|f| f.name.as_str()).collect();
        assert_eq!(names, ["nds9_static_a_02000000", "nds9_static_a_0200000c"]);
        assert_eq!(u9.functions[1].offset, 12);
        assert_eq!(&u9.blob[12..16], &code9[12..16]);
        let c: String = u9.output.files.iter().map(|f| f.text.as_str()).collect();
        assert!(
            c.contains("rl_nds_cpu_enter(c, (uint32_t)(UINT32_C(0x0)), (uint32_t)(UINT32_C(0x1)))"),
            "{c}"
        );
        let c7: String = u7.output.files.iter().map(|f| f.text.as_str()).collect();
        assert!(c7.contains("rl_nds_swi_trap(c, mem, 0x6u)"), "{c7}");
        assert!(
            c7.contains(
                "rl_nds_cpu_enter(c, (uint32_t)(UINT32_C(0x1)), (uint32_t)(UINT32_C(0x0)))"
            ),
            "{c7}"
        );
    }

    #[test]
    fn the_program_lists_each_unit_with_its_checks() {
        let code9 = arm(&[0xEAFF_FFFE]);
        let l = lifted(&rom(&code9, &code9));
        let c = program_c("T\"", &l.units, &["--peek".into(), "0x10".into()]);
        assert!(c.contains("static const rl_nds_fn fns0[] = {\n    { 0x2000000u, 0x0u, 0x4u, nds9_static_a_02000000 },"), "{c}");
        assert!(c.contains("{ RL_NDS_ARM7, &rl_unit_nds_arm7_"), "{c}");
        assert!(c.contains("units, 2u, \"T\\\"\", args, 2u }"), "{c}");
        assert!(
            c.contains("    \"--peek\",\n    \"0x10\",\n    NULL,"),
            "{c}"
        );
        let d = data_c(2);
        assert!(d.contains("#embed \"unit1.bin\" if_empty(0)"), "{d}");
    }
}
