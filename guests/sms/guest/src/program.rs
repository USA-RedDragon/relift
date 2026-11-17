//! A ROM's code as lifted units and the program files that run it.
//!
//! Each ROM bank, as one slot shows it, is a unit of its own, so lifted code
//! inside a unit may go on directly only while the slot shows that bank,
//! which the runtime checks at each such exit; every other transfer goes
//! through dispatch, which picks the function of the bank the slot shows at
//! run time. Code in work RAM is lifted from the bytes found there, in
//! groups that hold one form of the code at each address, with its operand
//! bytes read when it runs; dispatch and the checks compare the opcode bytes
//! in RAM with those of the group. The four
//! interrupt entry blocks are a unit of their own. Every instruction of a
//! unit is a dispatch entry.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;

use relift_cpu_z80::{ENTRY, Entry, HALF};
use relift_emit::{Function, Insn, Options, Output};
use relift_guest::{Frontend, LiftFailure};
use relift_ir::{Block, Next};

use crate::code::{Code, CodeSrc, Loc};
use crate::frontend::{RAM, Sms, UnitTag, device_map};

/// The C symbol of the interrupt entries' unit.
pub const ENTRIES_SYMBOL: &str = "rl_unit_sms_entries";

/// One lifted unit.
#[derive(Clone, Debug)]
pub struct Unit {
    /// The code it holds.
    pub tag: UnitTag,
    /// The C symbol of the unit table.
    pub symbol: String,
    /// The directory of its files, relative to the C directory.
    pub dir: String,
    /// The emitted files.
    pub output: Output,
    /// Instructions lifted.
    pub instructions: usize,
}

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
    /// The interrupt entry blocks.
    pub entries: Unit,
    /// The ROM and RAM units.
    pub units: Vec<Unit>,
    /// The instructions of each RAM group as (address, bytes), sorted.
    pub ram_groups: Vec<Vec<(u16, Vec<u8>)>>,
    /// Instructions that did not lift.
    pub unlifted: Vec<Unlifted>,
}

/// The console the program runs as.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Console {
    /// A Master System II.
    Ms,
    /// A Game Gear running a Game Gear cartridge.
    Gg,
    /// A Game Gear running a Master System cartridge.
    GgMs,
}

/// The video standard and region.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Region {
    /// NTSC, export.
    NtscU,
    /// NTSC, Japan.
    NtscJ,
    /// PAL.
    Pal,
}

/// The cartridge's bank hardware, as the runtime models it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mapper {
    /// The Sega mapper; it also runs a cartridge of 48 KiB or less as it is.
    Sega,
    /// The Codemasters mapper.
    Codemasters,
    /// The Korean mapper.
    Korea,
}

impl From<relift_sms_formats::MapperKind> for Mapper {
    fn from(k: relift_sms_formats::MapperKind) -> Mapper {
        match k {
            relift_sms_formats::MapperKind::None | relift_sms_formats::MapperKind::Sega => {
                Mapper::Sega
            }
            relift_sms_formats::MapperKind::Codemasters => Mapper::Codemasters,
            relift_sms_formats::MapperKind::Korea => Mapper::Korea,
        }
    }
}

/// What the program file needs besides the units.
#[derive(Clone, Debug)]
pub struct Program<'a> {
    /// The title, for the window and reports.
    pub title: &'a str,
    /// The console.
    pub console: Console,
    /// The region.
    pub region: Region,
    /// The mapper.
    pub mapper: Mapper,
}

/// The host name of the function that starts at `addr` in `tag`.
#[must_use]
pub fn host_name(tag: UnitTag, addr: u64) -> String {
    match tag {
        UnitTag::Entries => format!("sms_entry_{}", addr & 0xF),
        UnitTag::Rom { bank, slot } => format!("sms_b{bank:03x}_{slot}_{addr:04x}"),
        UnitTag::Ram { group } => format!("sms_r{group}_{addr:04x}"),
    }
}

/// The C symbol of the unit of `tag`.
#[must_use]
pub fn unit_symbol(tag: UnitTag) -> String {
    match tag {
        UnitTag::Entries => ENTRIES_SYMBOL.into(),
        UnitTag::Rom { bank, slot } => format!("rl_unit_sms_b{bank:03x}_{slot}"),
        UnitTag::Ram { group } => format!("rl_unit_sms_r{group}"),
    }
}

fn unit_dir(tag: UnitTag) -> String {
    match tag {
        UnitTag::Entries => "entries".into(),
        UnitTag::Rom { bank, slot } => format!("b{bank:03x}_{slot}"),
        UnitTag::Ram { group } => format!("r{group}"),
    }
}

/// Four bytes from `rom` at `off`, 0xFF past its end.
fn rom_bytes(rom: &[u8], off: usize) -> [u8; 4] {
    let mut b = [0xFF; 4];
    for (k, x) in b.iter_mut().enumerate() {
        if let Some(v) = rom.get(off + k) {
            *x = *v;
        }
    }
    b
}

type Blocks = BTreeMap<u64, (Result<Block, LiftFailure>, u64)>;

fn raw(bytes: &[u8]) -> u64 {
    bytes
        .iter()
        .take(4)
        .fold(0u64, |acc, b| (acc << 8) | u64::from(*b))
}

/// Lifts the instruction at `pc` from `bytes`, and its taken half when it
/// has one, into `blocks`.
fn lift_one(fe: &Sms, bytes: &[u8], pc: u64, blocks: &mut Blocks) {
    let r = fe.lift(bytes, pc);
    let half = pc | HALF;
    let has_half = r.as_ref().is_ok_and(|b| match b.exit.next {
        Next::Const(t) => t == half,
        Next::Select {
            taken, not_taken, ..
        } => taken == half || not_taken == half,
        Next::Dynamic(_) => false,
    });
    blocks.insert(pc, (r, raw(bytes)));
    if has_half {
        blocks.insert(half, (fe.lift(bytes, half), raw(bytes)));
    }
}

/// The functions of a unit: one per run of adjacent instructions.
fn functions<'a>(tag: UnitTag, blocks: &'a Blocks, lens: &BTreeMap<u16, u8>) -> Vec<Function<'a>> {
    let mut out: Vec<Function<'a>> = Vec::new();
    let mut end = u32::MAX;
    for (&a, &n) in lens {
        let a32 = u32::from(a);
        if a32 != end || out.is_empty() {
            out.push(Function {
                entry: u64::from(a),
                name: host_name(tag, u64::from(a)),
                guest_name: None,
                span: 0,
                insts: BTreeMap::new(),
                tables: BTreeMap::new(),
                pads: BTreeSet::new(),
                guesses: BTreeMap::new(),
            });
        }
        end = a32 + u32::from(n);
        let f = out.last_mut().expect("a function");
        f.span = u64::from(end) - f.entry;
        for key in [u64::from(a), u64::from(a) | HALF] {
            if let Some((r, raw)) = blocks.get(&key) {
                let i = match r {
                    Ok(block) => Insn::Lifted { block, raw: *raw },
                    Err(e) => Insn::Failed(e),
                };
                f.insts.insert(key, i);
            }
        }
    }
    out
}

fn emit_unit(tag: UnitTag, fns: &[Function<'_>], image_id: [u8; 32], anywhere: bool) -> Unit {
    let symbol = unit_symbol(tag);
    let opts = Options {
        unit_name: unit_dir(tag),
        unit_symbol: symbol.clone(),
        io_map: device_map(),
        regs_in_context: true,
        image_id,
        enter_anywhere: anywhere.then_some(0xFFFF),
        ..Options::default()
    };
    let output = relift_emit::emit(&Sms::new(tag), fns, &[], &opts);
    Unit {
        tag,
        symbol,
        dir: unit_dir(tag),
        output,
        instructions: fns.iter().map(|f| f.insts.len()).sum(),
    }
}

fn entries(image_id: [u8; 32]) -> Unit {
    let fe = Sms::new(UnitTag::Entries);
    let mut blocks = Blocks::new();
    for e in [Entry::Nmi, Entry::Im0, Entry::Im1, Entry::Im2] {
        blocks.insert(e.addr(), (fe.lift(&[0; 4], e.addr()), 0));
    }
    let fns: Vec<Function<'_>> = blocks
        .iter()
        .map(|(&pc, (r, raw))| Function {
            entry: pc,
            name: host_name(UnitTag::Entries, pc - ENTRY),
            guest_name: None,
            span: 1,
            insts: std::iter::once((
                pc,
                match r {
                    Ok(block) => Insn::Lifted { block, raw: *raw },
                    Err(e) => Insn::Failed(e),
                },
            ))
            .collect(),
            tables: BTreeMap::new(),
            pads: BTreeSet::new(),
            guesses: BTreeMap::new(),
        })
        .collect();
    emit_unit(UnitTag::Entries, &fns, image_id, false)
}

/// The bytes of `b` that select its template (`opcode_mask`), others zero.
#[must_use]
pub fn opcode_bytes(b: &[u8]) -> Vec<u8> {
    let mask = relift_cpu_z80::opcode_mask(b).unwrap_or(0xFF);
    b.iter()
        .enumerate()
        .map(|(k, &x)| if mask & (1 << k) != 0 { x } else { 0 })
        .collect()
}

/// The mask of the bytes of `b` the runtime checks at entry.
#[must_use]
pub fn check_mask(b: &[u8]) -> u8 {
    relift_cpu_z80::opcode_mask(b).unwrap_or(0xFF) & ((1u16 << b.len().min(8)) - 1) as u8
}

fn same_opcodes(a: &[u8], b: &[u8]) -> bool {
    a.len() == b.len() && opcode_bytes(a) == opcode_bytes(b)
}

/// Lifts the instructions of `code` (in `rom`, without a copier header)
/// into units; `image_id` identifies the ROM to the runtime.
#[must_use]
pub fn lift(rom: &[u8], code: &Code, image_id: [u8; 32]) -> Lifted {
    let mut rom_units: BTreeMap<UnitTag, BTreeMap<u16, u8>> = BTreeMap::new();
    let mut ram_at: BTreeMap<u16, Vec<Vec<u8>>> = BTreeMap::new();
    for (l, &n) in &code.instructions {
        match &l.src {
            CodeSrc::Rom { bank } if l.addr < RAM => {
                rom_units
                    .entry(UnitTag::Rom {
                        bank: *bank,
                        slot: l.slot(),
                    })
                    .or_default()
                    .insert(l.addr, n);
            }
            CodeSrc::Ram { bytes } if l.addr >= RAM => {
                let forms = ram_at.entry(l.addr).or_default();
                if !forms.iter().any(|f| same_opcodes(f, bytes)) {
                    forms.push(bytes.clone());
                }
            }
            _ => {}
        }
    }
    let mut groups: Vec<BTreeMap<u16, Vec<u8>>> = Vec::new();
    for (addr, forms) in ram_at {
        for (k, b) in forms.into_iter().enumerate() {
            if groups.len() <= k {
                groups.resize_with(k + 1, BTreeMap::new);
            }
            groups[k].insert(addr, b);
        }
    }
    let mut unlifted = Vec::new();
    let mut units = Vec::new();
    for (tag, lens) in &rom_units {
        let UnitTag::Rom { bank, .. } = *tag else {
            continue;
        };
        let fe = Sms::new(*tag);
        let mut blocks = Blocks::new();
        for &a in lens.keys() {
            let off = bank as usize * 0x4000 + usize::from(a & 0x3FFF);
            lift_one(&fe, &rom_bytes(rom, off), u64::from(a), &mut blocks);
        }
        collect_failures(&blocks, |pc| Loc::rom(pc, bank), &mut unlifted);
        units.push(emit_unit(
            *tag,
            &functions(*tag, &blocks, lens),
            image_id,
            true,
        ));
    }
    let mut ram_groups = Vec::new();
    for (g, insns) in groups.iter().enumerate() {
        let tag = UnitTag::Ram { group: g as u32 };
        let fe = Sms::new(tag);
        let mut blocks = Blocks::new();
        let mut lens = BTreeMap::new();
        for (&a, b) in insns {
            let mut bytes = b.clone();
            bytes.resize(4, 0);
            lift_one(&fe, &bytes, u64::from(a), &mut blocks);
            lens.insert(a, b.len() as u8);
        }
        collect_failures(
            &blocks,
            |pc| Loc {
                addr: pc,
                src: CodeSrc::Ram {
                    bytes: insns.get(&pc).cloned().unwrap_or_default(),
                },
            },
            &mut unlifted,
        );
        units.push(emit_unit(
            tag,
            &functions(tag, &blocks, &lens),
            image_id,
            true,
        ));
        ram_groups.push(insns.iter().map(|(a, b)| (*a, b.clone())).collect());
    }
    Lifted {
        entries: entries(image_id),
        units,
        ram_groups,
        unlifted,
    }
}

fn collect_failures(blocks: &Blocks, loc: impl Fn(u16) -> Loc, out: &mut Vec<Unlifted>) {
    for (&pc, (r, _)) in blocks {
        if let Err(e) = r {
            out.push(Unlifted {
                at: loc((pc & 0xFFFF) as u16),
                failure: e.clone(),
            });
        }
    }
}

/// `rom.c`: the ROM as the blob `rl_sms_rom` (its bytes in `c/rom.bin`).
#[must_use]
pub fn rom_c(rom: &[u8]) -> String {
    let mut t = relift_guest::blob::blob_c("rl_sms_rom", "c/rom.bin", rom);
    t.push_str("#include <stddef.h>\nextern const size_t rl_sms_rom_len;\n");
    let _ = writeln!(t, "const size_t rl_sms_rom_len = {}u;", rom.len());
    t
}

fn c_string(s: &str) -> String {
    let mut o = String::from("\"");
    for ch in s.chars() {
        match ch {
            '"' => o.push_str("\\\""),
            '\\' => o.push_str("\\\\"),
            c if c.is_ascii_graphic() || c == ' ' => o.push(c),
            _ => o.push('?'),
        }
    }
    o.push('"');
    o
}

/// `program.c`: the unit tables and `main`.
#[must_use]
pub fn program_c(p: &Program<'_>, l: &Lifted) -> String {
    let mut s = String::from(
        "#include <stddef.h>\n#include <stdint.h>\n#include <string.h>\n#include \"smsmain.h\"\n",
    );
    for u in std::iter::once(&l.entries).chain(&l.units) {
        let _ = writeln!(s, "extern const rl_unit {};", u.symbol);
    }
    s.push_str("extern const uint8_t rl_sms_rom[];\nextern const size_t rl_sms_rom_len;\n");
    s.push_str("static const rl_sms_rom_unit roms[] = {\n");
    let mut roms = 0;
    for u in &l.units {
        if let UnitTag::Rom { bank, slot } = u.tag {
            let _ = writeln!(s, "    {{ {bank}u, {slot}u, &{} }},", u.symbol);
            roms += 1;
        }
    }
    if roms == 0 {
        s.push_str("    { 0u, 0u, NULL },\n");
    }
    s.push_str("};\n");
    for (g, insns) in l.ram_groups.iter().enumerate() {
        let _ = writeln!(s, "static const rl_sms_ram_insn ram{g}[] = {{");
        for (a, b) in insns {
            let bytes: Vec<String> = (0..4)
                .map(|k| format!("{:#04x}", b.get(k).copied().unwrap_or(0)))
                .collect();
            let _ = writeln!(
                s,
                "    {{ {a:#06x}u, {}u, {:#04x}u, {{ {} }} }},",
                b.len(),
                check_mask(b),
                bytes.join(", ")
            );
        }
        s.push_str("};\n");
    }
    s.push_str("static const rl_sms_ram_group rams[] = {\n");
    for (g, insns) in l.ram_groups.iter().enumerate() {
        let _ = writeln!(
            s,
            "    {{ ram{g}, {}u, &{} }},",
            insns.len(),
            unit_symbol(UnitTag::Ram { group: g as u32 })
        );
    }
    if l.ram_groups.is_empty() {
        s.push_str("    { NULL, 0u, NULL },\n");
    }
    let console = match p.console {
        Console::Ms => "SMS_SYSTEM_MS",
        Console::Gg => "SMS_SYSTEM_GG",
        Console::GgMs => "SMS_SYSTEM_GG_MS",
    };
    let region = match p.region {
        Region::NtscU => "SMS_REGION_NTSC_U",
        Region::NtscJ => "SMS_REGION_NTSC_J",
        Region::Pal => "SMS_REGION_PAL",
    };
    let mapper = match p.mapper {
        Mapper::Sega => "SMS_MAPPER_SEGA",
        Mapper::Codemasters => "SMS_MAPPER_CODEMASTERS",
        Mapper::Korea => "SMS_MAPPER_KOREA",
    };
    let _ = write!(
        s,
        "}};\nint main(int argc, char **argv)\n{{\n    rl_sms_program prog = {{ rl_sms_rom, rl_sms_rom_len, {}, {console}, {region}, {mapper}, &{ENTRIES_SYMBOL}, roms, {roms}u, rams, {}u }};\n    return rl_sms_main(&prog, argc, argv);\n}}\n",
        c_string(p.title),
        l.ram_groups.len(),
    );
    s
}
