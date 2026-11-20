//! A ROM as lifted units and the program files that run it.
//!
//! The fixed window (`$0000-$3FFF`) is one unit; each switchable bank with
//! code is a unit of its own, so a direct call inside a unit always names
//! code of the unit's bank, and every transfer that leaves it goes through
//! dispatch, which picks the function of the bank mapped at run time. A
//! function keeps only the instructions of its own bank: the analysis may
//! follow a jump into another bank, and that code then runs through
//! dispatch too.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;

use relift_cpu_sm83::{HALF, decode};
use relift_emit::{Function, Insn, Options, Output};
use relift_gb_analyze::{Loc, Report};
use relift_guest::{Frontend, LiftFailure};
use relift_ir::{Block, Flow, Next};

use crate::frontend::{Gb, device_map};

/// The most instructions of a run of one filler byte (`nop`, `rst $38`)
/// that are lifted one by one. The rest of a `nop` run is a sled
/// ([`Lifted::sleds`]), which the runtime runs as one loop; the rest of a
/// `rst $38` run is left to dispatch. Test ROMs pad whole banks with such
/// runs, which discovery reaches by falling through.
pub const FILLER_RUN: usize = 8;

/// The bank of a unit of the fixed window in [`Unit::bank`].
pub const FIXED: u32 = u32::MAX;

/// One lifted unit: the fixed window or one switchable bank.
#[derive(Clone, Debug)]
pub struct Unit {
    /// The bank whose window functions the unit holds, or [`FIXED`].
    pub bank: u32,
    /// The C symbol of the unit table.
    pub symbol: String,
    /// The directory of its files, relative to the C directory.
    pub dir: String,
    /// The emitted files.
    pub output: Output,
    /// Functions in the unit.
    pub functions: usize,
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
#[derive(Clone, Debug, Default)]
pub struct Lifted {
    /// The units, the fixed window first.
    pub units: Vec<Unit>,
    /// Instructions that did not lift.
    pub unlifted: Vec<Unlifted>,
    /// Instructions an analysis function held in another bank's window;
    /// each is lifted once more in that bank's unit, in a function of
    /// its own there that dispatch enters at any instruction.
    pub foreign: usize,
    /// Instructions of filler runs past [`FILLER_RUN`] that are not
    /// sleds, left to dispatch.
    pub filler: usize,
    /// Runs of `nop` past [`FILLER_RUN`] as (unit bank, first address,
    /// end address), which the runtime runs as one loop; [`RAM_UNIT`] for a
    /// run in RAM (`RamOps::sleds`).
    pub sleds: Vec<(u32, u16, u16)>,
    /// Instructions lifted at RAM addresses as (address, opcode key, host
    /// name), in the units whose bank is [`RAM_UNIT`].
    pub ram: Vec<(u16, u16, String)>,
    /// ROM instructions lifted with their operands read as they run, as
    /// (unit bank, address, opcode, host name), in the units whose bank is
    /// [`LIVE_UNIT`]: see [`lift_live`].
    pub live: Vec<(u32, u16, u16, String)>,
    /// The instruction after each lifted `HALT` as the HALT bug runs it, as
    /// (unit bank, address, host name), in the units whose bank is
    /// [`HALT_BUG_UNIT`]: see [`halt_bug_bytes`].
    pub halt_bug: Vec<(u32, u16, String)>,
}

/// The [`Unit::bank`] of a unit of HALT-bug forms ([`Lifted::halt_bug`]),
/// which the runtime reaches through its HALT-bug table rather than
/// dispatch.
pub const HALT_BUG_UNIT: u32 = u32::MAX - 2;

/// The [`Unit::bank`] of a unit of live ROM instructions
/// ([`Lifted::live`]), which the runtime reaches through its table of them.
pub const LIVE_UNIT: u32 = u32::MAX - 3;

/// Lifts the ROM instructions of `live`, each as (location, opcode), with
/// their operand bytes read as they run, as code in RAM is: while the OAM
/// DMA holds the bus the CPU fetches the DMA's bytes rather than the ROM's,
/// so the runtime runs the one lifted for the opcode it fetched. An opcode
/// plus 0x100 is one fetched without the program counter moving: it is
/// lifted one byte before its location, as a HALT-bug form is
/// ([`halt_bug_bytes`]), so its operands are read from the location on;
/// such a location may be `$8000`, after a `HALT` at the window's end.
/// The locations and opcodes come from the analysis (`RamOps::live`).
pub fn lift_live(live: &[(Loc, u16)], image_id: [u8; 32], lifted: &mut Lifted) {
    let fe = Gb::live();
    let mut by: BTreeMap<(u32, u16), BTreeSet<u16>> = BTreeMap::new();
    for &(l, k) in live {
        let edge = l.addr == 0x8000 && k >= 0x100;
        if (l.addr < 0x8000 || edge)
            && k < 0x200
            && k & 0xFF != 0xCB
            && (k < 0x100 || l.addr & 0x3FFF != 0 || edge)
        {
            let key = if l.addr < 0x4000 {
                FIXED
            } else {
                u32::from(l.bank)
            };
            by.entry((key, l.addr)).or_default().insert(k);
        }
    }
    let mut groups: Vec<Vec<Function<'_>>> = Vec::new();
    let mut lifts = Vec::new();
    for (&(key, pc), keys) in &by {
        for (i, &k) in keys.iter().enumerate() {
            let bytes = [k as u8, 0, 0];
            let bank = (key != FIXED).then_some(u64::from(key));
            let entry = live_entry(pc, k);
            let blocks: Vec<_> = lift_one(&fe, &bytes, u64::from(entry), bank)
                .into_iter()
                .filter(|(_, r, _)| r.is_ok())
                .collect();
            if blocks.is_empty() {
                continue;
            }
            lifts.push((i * 4 + usize::from(entry & 3), key, pc, k, blocks));
        }
    }
    for (g, key, pc, k, _) in &lifts {
        let name = live_name(*key, *pc, *k);
        lifted.live.push((*key, *pc, *k, name));
        if groups.len() <= *g {
            groups.resize_with(*g + 1, Vec::new);
        }
    }
    for (g, key, pc, k, blocks) in &lifts {
        groups[*g].push(Function {
            entry: u64::from(live_entry(*pc, *k)),
            name: live_name(*key, *pc, *k),
            guest_name: None,
            span: 1,
            insts: blocks
                .iter()
                .filter_map(|(at, r, raw)| {
                    r.as_ref()
                        .ok()
                        .map(|block| (*at, Insn::Lifted { block, raw: *raw }))
                })
                .collect(),
            tables: BTreeMap::new(),
            guesses: BTreeMap::new(),
            entries: BTreeMap::new(),
            pads: BTreeSet::new(),
        });
    }
    for fns in &mut groups {
        fns.sort_by_key(|f| f.entry);
    }
    for (i, fns) in groups.iter().enumerate().filter(|(_, f)| !f.is_empty()) {
        let symbol = format!("rl_unit_gb_live{i}");
        let opts = Options {
            unit_name: format!("live{i}"),
            unit_symbol: symbol.clone(),
            io_map: device_map(),
            regs_in_context: true,
            image_id,
            ..Options::default()
        };
        let output = relift_emit::emit(&fe, fns, &[], &opts);
        lifted.units.push(Unit {
            bank: LIVE_UNIT,
            symbol,
            dir: format!("live{i}"),
            output,
            functions: fns.len(),
        });
    }
}

fn live_entry(pc: u16, k: u16) -> u16 {
    if k >= 0x100 { pc - 1 } else { pc }
}

fn live_name(key: u32, pc: u16, k: u16) -> String {
    if key == FIXED {
        format!("gb_live_f_{pc:04x}_{k:02x}")
    } else {
        format!("gb_live_b{key:03x}_{pc:04x}_{k:02x}")
    }
}

/// The bytes the CPU runs as one instruction at `l`, the address after a
/// `HALT`, when the HALT bug leaves the program counter on it: the opcode
/// at `l` is fetched without the program counter moving, so the operand
/// reads start at `l` again. They are lifted as an instruction one byte
/// before `l`, whose fetch has moved the program counter to `l`; its
/// length, relative targets and return address then come out as the
/// hardware's.
#[must_use]
pub fn halt_bug_bytes(rom: &[u8], l: Loc) -> Option<[u8; 3]> {
    let b = bytes_at(rom, l)?;
    let op = *b.first()?;
    Some([op, op, b.get(1).copied().unwrap_or(0xFF)])
}

/// The [`Unit::bank`] of a unit of instructions lifted at RAM addresses,
/// which the runtime reaches through its RAM table rather than dispatch.
pub const RAM_UNIT: u32 = u32::MAX - 1;

/// What earlier runs recorded in the learned file.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Learned {
    /// ROM locations run without lifted code.
    pub rom: Vec<Loc>,
    /// RAM instructions run without lifted code, as (address, opcode key):
    /// the opcode, or 0x100 plus the byte after a CB prefix.
    pub ram: Vec<(u16, u16)>,
}

fn unit_key(l: Loc) -> u32 {
    if l.is_window() {
        u32::from(l.bank)
    } else {
        FIXED
    }
}

fn bytes_at(rom: &[u8], l: Loc) -> Option<&[u8]> {
    let start = relift_gb_formats::banks::rom_offset(usize::from(l.bank), l.addr)?;
    let window_end: u16 = if l.addr < 0x4000 { 0x4000 } else { 0x8000 };
    let end = start
        .checked_add(usize::from(window_end.checked_sub(l.addr)?))?
        .min(rom.len());
    rom.get(start..end).filter(|s| !s.is_empty())
}

/// The host name of the function at `l`.
#[must_use]
pub fn host_name(l: Loc) -> String {
    if l.is_window() {
        format!("gb_b{:03x}_{:04x}", l.bank, l.addr)
    } else {
        format!("gb_f_{:04x}", l.addr)
    }
}

/// Lifted blocks as (address, block or failure, raw bytes).
type Blocks = Vec<(u64, Result<Block, LiftFailure>, u64)>;

struct Lifts {
    blocks: Blocks,
    span: u64,
}

/// A RAM unit: the entry addresses it holds, and its functions as (entry, key, lifts).
type RamUnit = (BTreeSet<u16>, Vec<(u16, u16, Lifts)>);

fn lift_function(
    fe: &Gb,
    rom: &[u8],
    f: &relift_gb_analyze::Function,
    lifted: &mut Lifted,
    foreign: &mut BTreeMap<Loc, u8>,
    sleds: &mut BTreeSet<(u32, u16)>,
) -> Lifts {
    let own = unit_key(f.entry);
    let bank = (own != FIXED).then_some(u64::from(own));
    let mut out = Vec::new();
    let mut end = u64::from(f.entry.addr);
    let mut todo: Vec<(Loc, u64)> = Vec::new();
    let mut run: (Option<(Loc, u8)>, usize) = (None, 0);
    for (&l, &len) in &f.body {
        if unit_key(l) != own && l.is_window() {
            lifted.foreign += 1;
            foreign.insert(l, len);
            continue;
        }
        let byte = bytes_at(rom, l).and_then(|b| b.first().copied());
        let filler = len == 1 && matches!(byte, Some(0x00 | 0xFF));
        run = match (run, byte) {
            ((Some((prev, b)), n), Some(now))
                if filler && now == b && prev.bank == l.bank && prev.addr + 1 == l.addr =>
            {
                (Some((l, b)), n + 1)
            }
            (_, Some(now)) if filler => (Some((l, now)), 1),
            _ => (None, 0),
        };
        if run.1 > FILLER_RUN {
            if byte == Some(0x00) {
                sleds.insert((unit_key(l), l.addr));
            } else {
                lifted.filler += 1;
            }
            continue;
        }
        if l.addr >= f.entry.addr {
            end = end.max(u64::from(l.addr) + u64::from(len));
        }
        todo.push((l, u64::from(l.addr)));
    }
    let mut seen = BTreeSet::new();
    while let Some((l, pc)) = todo.pop() {
        if !seen.insert(pc) {
            continue;
        }
        let Some(bytes) = bytes_at(rom, l) else {
            continue;
        };
        let raw = bytes
            .iter()
            .take(3)
            .fold(0u64, |a, b| (a << 8) | u64::from(*b));
        let r = fe.lift(bytes, pc).map(|mut b| {
            b.bank = bank;
            b
        });
        if let Ok(b) = &r {
            let targets: Vec<u64> = match b.exit.next {
                Next::Const(t) => vec![t],
                Next::Select {
                    taken, not_taken, ..
                } => vec![taken, not_taken],
                Next::Dynamic(_) => Vec::new(),
            };
            for t in targets {
                if t >= HALF && t & 0xFFFF == pc & 0xFFFF {
                    todo.push((l, t));
                }
            }
        }
        if let Err(e) = &r
            && pc < HALF
        {
            lifted.unlifted.push(Unlifted {
                at: l,
                failure: e.clone(),
            });
        }
        out.push((pc, r, raw));
    }
    out.sort_by_key(|(pc, ..)| *pc);
    Lifts {
        blocks: out,
        span: end - u64::from(f.entry.addr),
    }
}

/// The blocks of one instruction lifted from `bytes` at `pc` with `bank`,
/// with the continuations its exits name.
fn lift_one(
    fe: &Gb,
    bytes: &[u8],
    pc: u64,
    bank: Option<u64>,
) -> Vec<(u64, Result<Block, LiftFailure>, u64)> {
    let raw = bytes
        .iter()
        .take(3)
        .fold(0u64, |a, b| (a << 8) | u64::from(*b));
    let mut out = Vec::new();
    let mut todo = vec![pc];
    let mut seen = BTreeSet::new();
    while let Some(at) = todo.pop() {
        if !seen.insert(at) {
            continue;
        }
        let r = fe.lift(bytes, at).map(|mut b| {
            b.bank = bank;
            b
        });
        if let Ok(b) = &r {
            let targets: Vec<u64> = match b.exit.next {
                Next::Const(t) => vec![t],
                Next::Select {
                    taken, not_taken, ..
                } => vec![taken, not_taken],
                Next::Dynamic(_) => Vec::new(),
            };
            todo.extend(
                targets
                    .into_iter()
                    .filter(|&t| t >= HALF && t & 0xFFFF == pc & 0xFFFF),
            );
        }
        out.push((at, r, raw));
    }
    out.sort_by_key(|(a, ..)| *a);
    out
}

/// The HALT-bug forms ([`halt_bug_bytes`]) of the instructions after the
/// lifted `HALT` instructions of `groups`, and of both bytes after each
/// `STOP` (a speed switch may end it at either), emitted into units of their own.
/// Each goes to a unit whose other functions start at least 4 bytes away,
/// so no function of a unit starts where another one's instruction ends: a
/// transfer there must go through dispatch.
fn lift_halt_bug(
    rom: &[u8],
    groups: &BTreeMap<u32, Vec<(Loc, Lifts)>>,
    fe: &Gb,
    image_id: [u8; 32],
    lifted: &mut Lifted,
) {
    let mut sites = BTreeSet::new();
    for (&key, list) in groups {
        for (_, l) in list {
            for (pc, r, _) in &l.blocks {
                let Ok(b) = r else { continue };
                if *pc >= HALF
                    || !matches!(b.exit.flow, Flow::Trap(code) if code >> 16 == 0x76 || code >> 16 == 0x10)
                {
                    continue;
                }
                let (key, end) = if *pc < 0x4000 {
                    (FIXED, 0x4000)
                } else {
                    (key, 0x8000)
                };
                let stop = matches!(b.exit.flow, Flow::Trap(code) if code >> 16 == 0x10);
                for k in 1..=if stop { 2 } else { 1 } {
                    let at = (*pc as u16).wrapping_add(k);
                    if u32::from(at) < end && (key == FIXED || at >= 0x4000) {
                        sites.insert((key, at));
                    }
                }
            }
        }
    }
    let mut units: Vec<Vec<(u32, u16, Blocks)>> = Vec::new();
    for (key, at) in sites {
        let l = Loc {
            bank: if key == FIXED { 0 } else { key as u16 },
            addr: at,
        };
        let Some(bytes) = halt_bug_bytes(rom, l) else {
            continue;
        };
        let bank = (key != FIXED).then_some(u64::from(key));
        let blocks: Vec<_> = lift_one(fe, &bytes, u64::from(at) - 1, bank)
            .into_iter()
            .filter(|(_, r, _)| r.is_ok())
            .collect();
        if blocks.is_empty() {
            continue;
        }
        let entry = at - 1;
        let slot = units.iter().position(|u| {
            u.iter()
                .all(|(_, a, _)| (i32::from(*a) - 1 - i32::from(entry)).abs() >= 4)
        });
        match slot {
            Some(i) => units[i].push((key, at, blocks)),
            None => units.push(vec![(key, at, blocks)]),
        }
    }
    for u in &mut units {
        u.sort_by_key(|(_, at, _)| *at);
    }
    for (i, list) in units.iter().enumerate() {
        let fns: Vec<Function<'_>> = list
            .iter()
            .map(|(key, at, blocks)| Function {
                entry: u64::from(*at) - 1,
                name: halt_bug_name(*key, *at),
                guest_name: None,
                span: 1,
                insts: blocks
                    .iter()
                    .filter_map(|(pc, r, raw)| {
                        r.as_ref()
                            .ok()
                            .map(|block| (*pc, Insn::Lifted { block, raw: *raw }))
                    })
                    .collect(),
                tables: BTreeMap::new(),
                guesses: BTreeMap::new(),
                entries: BTreeMap::new(),
                pads: BTreeSet::new(),
            })
            .collect();
        let symbol = format!("rl_unit_gb_hb{i}");
        let opts = Options {
            unit_name: format!("hb{i}"),
            unit_symbol: symbol.clone(),
            io_map: device_map(),
            regs_in_context: true,
            image_id,
            ..Options::default()
        };
        let output = relift_emit::emit(fe, &fns, &[], &opts);
        for (key, at, _) in list {
            lifted.halt_bug.push((*key, *at, halt_bug_name(*key, *at)));
        }
        lifted.units.push(Unit {
            bank: HALT_BUG_UNIT,
            symbol,
            dir: format!("hb{i}"),
            output,
            functions: fns.len(),
        });
    }
}

fn halt_bug_name(key: u32, at: u16) -> String {
    if key == FIXED {
        format!("gb_hb_f_{at:04x}")
    } else {
        format!("gb_hb_b{key:03x}_{at:04x}")
    }
}

/// Functions in their own bank's unit for the instructions that analysis
/// functions of other units held there and no function of the bank lifts:
/// one per bank, entered at its lowest instruction.
fn homes(
    groups: &BTreeMap<u32, Vec<(Loc, Lifts)>>,
    foreign: &BTreeMap<Loc, u8>,
) -> Vec<(Loc, relift_gb_analyze::Function)> {
    let held: BTreeSet<(u32, u64)> = groups
        .iter()
        .flat_map(|(&k, list)| {
            list.iter()
                .flat_map(move |(_, f)| f.blocks.iter().map(move |(pc, ..)| (k, *pc)))
        })
        .collect();
    let mut by_bank: BTreeMap<u32, BTreeMap<Loc, u8>> = BTreeMap::new();
    for (&l, &len) in foreign {
        let key = unit_key(l);
        if !held.contains(&(key, u64::from(l.addr))) {
            by_bank.entry(key).or_default().insert(l, len);
        }
    }
    by_bank
        .into_values()
        .filter_map(|body| {
            let entry = *body.keys().next()?;
            let bytes = body.values().map(|&n| usize::from(n)).sum();
            Some((
                entry,
                relift_gb_analyze::Function {
                    entry,
                    origin: relift_gb_analyze::Origin::Root,
                    body,
                    bytes,
                    switches_bank: false,
                    trampoline: None,
                },
            ))
        })
        .collect()
}

/// Lifts every function of `report` (an analysis of `rom`) into units and
/// emits them, with the instructions at RAM addresses in `ram` (address,
/// opcode key; see [`Learned::ram`]); `image_id` identifies the ROM to the
/// runtime.
#[must_use]
pub fn lift(rom: &[u8], report: &Report, ram: &[(u16, u16)], image_id: [u8; 32]) -> Lifted {
    lift_superset(rom, report, ram, image_id, &[])
}

/// Functions over every address of the units `keys` (a bank, or [`FIXED`])
/// that starts an instruction the bytes decode to and no function of the
/// unit lifts: a superset of the code there, entered at any instruction.
/// An unused opcode is lifted as the lock the library makes of it.
fn superset(
    rom: &[u8],
    groups: &BTreeMap<u32, Vec<(Loc, Lifts)>>,
    keys: &[u32],
) -> Vec<(Loc, relift_gb_analyze::Function)> {
    let mut out = Vec::new();
    for &key in keys {
        let (bank, lo, hi) = if key == FIXED {
            (0u16, 0u16, 0x4000u16)
        } else {
            let Ok(b) = u16::try_from(key) else { continue };
            (b, 0x4000, 0x8000)
        };
        let held: BTreeSet<u64> = groups
            .get(&key)
            .into_iter()
            .flatten()
            .flat_map(|(_, f)| f.blocks.iter().map(|(pc, ..)| *pc))
            .collect();
        let mut body = BTreeMap::new();
        for addr in lo..hi {
            let l = Loc { bank, addr };
            if held.contains(&u64::from(addr)) {
                continue;
            }
            let Some(bytes) = bytes_at(rom, l) else {
                continue;
            };
            let Ok(ins) = decode(bytes) else { continue };
            let len = ins.length();
            if usize::from(addr) + usize::from(len) > usize::from(hi) {
                continue;
            }
            body.insert(l, len);
        }
        let Some(&entry) = body.keys().next() else {
            continue;
        };
        let bytes = body.values().map(|&n| usize::from(n)).sum();
        out.push((
            entry,
            relift_gb_analyze::Function {
                entry,
                origin: relift_gb_analyze::Origin::Root,
                body,
                bytes,
                switches_bank: false,
                trampoline: None,
            },
        ));
    }
    out
}

/// As [`lift`], with every decodable address of the units `keys` lifted too
/// ([`superset`]).
#[must_use]
pub fn lift_superset(
    rom: &[u8],
    report: &Report,
    ram: &[(u16, u16)],
    image_id: [u8; 32],
    keys: &[u32],
) -> Lifted {
    let fe = Gb::new();
    let mut lifted = Lifted::default();
    let mut groups: BTreeMap<u32, Vec<(Loc, Lifts)>> = BTreeMap::new();
    let mut foreign = BTreeMap::new();
    let mut sleds = BTreeSet::new();
    for f in report.functions.values() {
        if !f.entry.is_window() && f.entry.addr >= 0x8000 {
            continue;
        }
        let l = lift_function(&fe, rom, f, &mut lifted, &mut foreign, &mut sleds);
        if l.blocks.is_empty() {
            continue;
        }
        groups
            .entry(unit_key(f.entry))
            .or_default()
            .push((f.entry, l));
    }
    for (entry, f) in homes(&groups, &foreign) {
        let l = lift_function(&fe, rom, &f, &mut lifted, &mut BTreeMap::new(), &mut sleds);
        groups.entry(unit_key(entry)).or_default().push((entry, l));
    }
    for (entry, f) in superset(rom, &groups, keys) {
        let l = lift_function(&fe, rom, &f, &mut lifted, &mut BTreeMap::new(), &mut sleds);
        if !l.blocks.is_empty() {
            groups.entry(unit_key(entry)).or_default().push((entry, l));
        }
    }
    for (key, addr) in sleds {
        match lifted.sleds.last_mut() {
            Some((k, _, end)) if *k == key && *end == addr => *end = addr + 1,
            _ => lifted.sleds.push((key, addr, addr + 1)),
        }
    }
    groups.entry(FIXED).or_default();
    for list in groups.values_mut() {
        list.sort_by_key(|(entry, _)| *entry);
    }
    let window: BTreeSet<(u64, u64)> = groups
        .iter()
        .filter(|(k, _)| **k != FIXED)
        .flat_map(|(k, list)| {
            list.iter().flat_map(move |(e, l)| {
                std::iter::once((u64::from(*k), u64::from(e.addr))).chain(
                    l.blocks
                        .iter()
                        .filter(|(pc, ..)| (0x4000..0x8000).contains(pc))
                        .map(move |(pc, ..)| (u64::from(*k), *pc)),
                )
            })
        })
        .collect();
    let fe = Gb::with_window(window);
    let order: Vec<u32> = std::iter::once(FIXED)
        .chain(groups.keys().copied().filter(|&k| k != FIXED))
        .collect();
    for key in order {
        let list = &groups[&key];
        let fns: Vec<Function<'_>> = list
            .iter()
            .map(|(entry, l)| Function {
                entry: u64::from(entry.addr),
                name: host_name(*entry),
                guest_name: None,
                span: l.span.max(1),
                insts: l
                    .blocks
                    .iter()
                    .map(|(pc, r, raw)| {
                        let i = match r {
                            Ok(block) => Insn::Lifted { block, raw: *raw },
                            Err(e) => Insn::Failed(e),
                        };
                        (*pc, i)
                    })
                    .collect(),
                tables: BTreeMap::new(),
                pads: BTreeSet::new(),
                guesses: BTreeMap::new(),
                entries: BTreeMap::new(),
            })
            .collect();
        let (dir, symbol, name) = if key == FIXED {
            ("fixed".to_string(), "rl_unit_gb_fixed".to_string(), "fixed")
        } else {
            (
                format!("bank{key:03x}"),
                format!("rl_unit_gb_b{key:03x}"),
                "bank",
            )
        };
        let opts = Options {
            unit_name: if key == FIXED {
                name.into()
            } else {
                format!("{name}{key:03x}")
            },
            unit_symbol: symbol.clone(),
            io_map: device_map(),
            regs_in_context: true,
            image_id,
            enter_anywhere: Some(0xFFFF),
            ..Options::default()
        };
        let output = relift_emit::emit(&fe, &fns, &[], &opts);
        lifted.units.push(Unit {
            bank: key,
            symbol,
            dir,
            output,
            functions: fns.len(),
        });
    }
    lift_halt_bug(rom, &groups, &fe, image_id, &mut lifted);
    lift_ram(ram, image_id, &mut lifted);
    lifted
}

/// The units of a ROM the analysis refused (a mapper it does not model):
/// an empty fixed unit, so every instruction runs on the device library's
/// CPU.
#[must_use]
pub fn lift_nothing(image_id: [u8; 32]) -> Lifted {
    let opts = Options {
        unit_name: "fixed".into(),
        unit_symbol: "rl_unit_gb_fixed".into(),
        io_map: device_map(),
        regs_in_context: true,
        image_id,
        ..Options::default()
    };
    Lifted {
        units: vec![Unit {
            bank: FIXED,
            symbol: opts.unit_symbol.clone(),
            dir: "fixed".into(),
            output: relift_emit::emit(&Gb::new(), &[], &[], &opts),
            functions: 0,
        }],
        ..Lifted::default()
    }
}

/// What the program file needs besides the units.
#[derive(Clone, Debug)]
pub struct Program<'a> {
    /// The title, for the window and reports.
    pub title: &'a str,
    /// The hardware the program starts as unless told otherwise
    /// (`dmg`, `cgb`, ...; see `rl_gb_model_parse`).
    pub model: &'a str,
    /// The save file the program reads and writes unless told otherwise.
    pub save: &'a str,
    /// How the program enters.
    pub entry: Entry,
    /// The file the program adds the ROM locations it ran without lifted
    /// code to, which the next build reads ([`read_learned`]); empty for
    /// none.
    pub learn: &'a str,
}

pub use relift_guest::Entry;

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

/// A learned file as the program writes it: lines `BBB:AAAA` (a ROM
/// location, bank and address in hex) and `ram AAAA KKK` (a RAM address
/// and opcode key in hex); lines that do not parse are left out.
#[must_use]
pub fn read_learned(text: &str) -> Learned {
    let mut out = Learned::default();
    for l in text.lines() {
        let l = l.trim();
        if let Some(rest) = l.strip_prefix("ram ") {
            let mut it = rest.split_whitespace();
            let a = it.next().and_then(|a| u16::from_str_radix(a, 16).ok());
            let k = it.next().and_then(|k| u16::from_str_radix(k, 16).ok());
            if let (Some(a), Some(k)) = (a, k)
                && a >= 0x8000
                && k < 0x200
            {
                out.ram.push((a, k));
            }
        } else if let Some((b, a)) = l.split_once(':')
            && let (Ok(bank), Ok(addr)) = (u16::from_str_radix(b, 16), u16::from_str_radix(a, 16))
        {
            out.rom.push(Loc { bank, addr });
        }
    }
    out
}

/// The units of the instructions at RAM addresses in `ram` (address,
/// opcode key): each one lifted with its operands read as it runs, as a
/// function of its own. The k-th key of an address goes to a unit of keys
/// k at addresses of one residue mod 4, so no function of a unit starts
/// where another one's instruction ends (up to 3 bytes on): a transfer
/// there must go through the RAM table, which checks the opcode fetched,
/// not straight to the function.
/// The RAM instructions to lift: `ram` (address, opcode key), with every
/// key at each address where `ram` already holds more than `slot` keys: a
/// test driver's slot that it fills with each opcode in turn, which the
/// analysis saw run with many of them. The runtime picks the one lifted for
/// the opcode it fetches, so an opcode the analysis did not see run there
/// is still lifted code. An address whose stored values are many but which
/// no path ran (`RamOps::wide`) is not completed: that is data more often
/// than code, and 512 functions there cost a build far more than they give.
#[must_use]
pub fn complete_ram_slots(ram: &[(u16, u16)], slot: usize) -> Vec<(u16, u16)> {
    let mut by: BTreeMap<u16, BTreeSet<u16>> = BTreeMap::new();
    for &(a, k) in ram {
        by.entry(a).or_default().insert(k);
    }
    for ks in by.values_mut() {
        if ks.len() > slot {
            ks.extend(0..0x200);
        }
    }
    by.into_iter()
        .filter(|(a, _)| *a >= 0x8000)
        .flat_map(|(a, ks)| ks.into_iter().map(move |k| (a, k)))
        .collect()
}

/// The HALT-bug forms of the instructions after the `HALT` instructions of
/// the RAM code `by_pc` (address to opcode keys): the instruction after a
/// `HALT` at `h` whose opcode key is `k` runs as an instruction at `h` with
/// that opcode, whose operands are read as it runs from the address after
/// `h` on ([`halt_bug_bytes`]): the key `k`, or `0x1CB` for a CB-prefixed
/// one, whose prefix byte is read again as the second byte. That is an
/// instruction of RAM code like any other, so the runtime finds it in the
/// RAM table at `h`.
fn ram_halt_bug_keys(by_pc: &BTreeMap<u16, BTreeSet<u16>>) -> Vec<(u16, u16)> {
    let mut out = Vec::new();
    for (&h, keys) in by_pc {
        if !keys.contains(&0x76) {
            continue;
        }
        let Some(next) = h.checked_add(1).and_then(|n| by_pc.get(&n)) else {
            continue;
        };
        for &k in next {
            out.push((h, if k >= 0x100 { 0x1CB } else { k }));
        }
    }
    out
}

/// The most functions of RAM instructions in one unit. Each unit is one C
/// file with the lifted prelude, so one per few instructions meant about
/// 4,000 compiler runs for a ROM that executes from RAM. No two functions in
/// a unit start within 3 bytes of each other, since the instruction of one
/// would run into the next, and they are in address order.
const RAM_UNIT_FUNCTIONS: usize = 96;

fn lift_ram(ram: &[(u16, u16)], image_id: [u8; 32], lifted: &mut Lifted) {
    let fe = Gb::new();
    let mut by_pc: BTreeMap<u16, BTreeSet<u16>> = BTreeMap::new();
    for &(pc, k) in ram {
        if pc >= 0x8000 && k < 0x200 {
            by_pc.entry(pc).or_default().insert(k);
        }
    }
    for (pc, k) in ram_halt_bug_keys(&by_pc) {
        by_pc.entry(pc).or_default().insert(k);
    }
    let mut groups: Vec<Vec<(u16, u16, Lifts)>> = Vec::new();
    for (&pc, keys) in &by_pc {
        for (i, &k) in keys.iter().enumerate() {
            let bytes = if k >= 0x100 {
                vec![0xCB, (k & 0xFF) as u8, 0]
            } else {
                vec![k as u8, 0, 0]
            };
            let raw = bytes.iter().fold(0u64, |a, b| (a << 8) | u64::from(*b));
            let mut blocks: Vec<(u64, Result<Block, LiftFailure>, u64)> = Vec::new();
            let mut todo = vec![u64::from(pc)];
            while let Some(at) = todo.pop() {
                let r = fe.lift(&bytes, at);
                if let Ok(b) = &r
                    && let Next::Select { taken, .. } = b.exit.next
                    && taken >= HALF
                    && taken & 0xFFFF == u64::from(pc)
                    && !blocks.iter().any(|(a, ..)| *a == taken)
                {
                    todo.push(taken);
                }
                if r.is_err() {
                    continue;
                }
                blocks.push((at, r, raw));
            }
            if blocks.is_empty() {
                continue;
            }
            blocks.sort_by_key(|(a, ..)| *a);
            let g = i * 4 + usize::from(pc & 3);
            if groups.len() <= g {
                groups.resize_with(g + 1, Vec::new);
            }
            groups[g].push((pc, k, Lifts { blocks, span: 1 }));
        }
    }
    let mut units: Vec<RamUnit> = Vec::new();
    for item in groups.into_iter().flatten() {
        let at = units.iter().position(|(pcs, list)| {
            list.len() < RAM_UNIT_FUNCTIONS && !pcs.iter().any(|p| p.abs_diff(item.0) < 4)
        });
        let (pcs, list) = match at {
            Some(at) => &mut units[at],
            None => {
                units.push((BTreeSet::new(), Vec::new()));
                units.last_mut().expect("just pushed")
            }
        };
        pcs.insert(item.0);
        list.push(item);
    }
    for (i, (_, list)) in units.iter_mut().enumerate() {
        list.sort_by_key(|(pc, k, _)| (*pc, *k));
        let fns: Vec<Function<'_>> = list
            .iter()
            .map(|(pc, k, l)| Function {
                entry: u64::from(*pc),
                name: format!("gb_ram_{pc:04x}_{k:03x}"),
                guest_name: None,
                span: 1,
                insts: l
                    .blocks
                    .iter()
                    .filter_map(|(at, r, raw)| {
                        r.as_ref()
                            .ok()
                            .map(|block| (*at, Insn::Lifted { block, raw: *raw }))
                    })
                    .collect(),
                tables: BTreeMap::new(),
                pads: BTreeSet::new(),
                guesses: BTreeMap::new(),
                entries: BTreeMap::new(),
            })
            .collect();
        let symbol = format!("rl_unit_gb_ram{i}");
        let opts = Options {
            unit_name: format!("ram{i}"),
            unit_symbol: symbol.clone(),
            io_map: device_map(),
            regs_in_context: true,
            image_id,
            ..Options::default()
        };
        let output = relift_emit::emit(&fe, &fns, &[], &opts);
        for f in &fns {
            let k = list
                .iter()
                .find(|(pc, ..)| u64::from(*pc) == f.entry)
                .map(|(_, k, _)| *k);
            if let Some(k) = k {
                lifted.ram.push((f.entry as u16, k, f.name.clone()));
            }
        }
        lifted.units.push(Unit {
            bank: RAM_UNIT,
            symbol,
            dir: format!("ram{i}"),
            output,
            functions: fns.len(),
        });
    }
}

/// `rom.c`, which embeds `rom.bin` (the ROM as the program runs it).
#[must_use]
pub fn rom_c(rom: &[u8]) -> String {
    let mut t = relift_guest::blob::blob_c("rl_gb_rom", "c/rom.bin", rom);
    t.push_str("#include <stddef.h>\nextern const size_t rl_gb_rom_len;\n");
    let _ = writeln!(t, "const size_t rl_gb_rom_len = {}u;", rom.len());
    t
}

/// `program.c`: the unit list and `main`.
#[must_use]
pub fn program_c(
    units: &[Unit],
    sleds: &[(u32, u16, u16)],
    ram: &[(u16, u16, String)],
    halt_bug: &[(u32, u16, String)],
    live: &[(u32, u16, u16, String)],
    p: &Program<'_>,
) -> String {
    let mut t = String::from(
        "#include <stddef.h>\n#include <stdint.h>\n#include <string.h>\n#include \"gbmain.h\"\n",
    );
    for u in units {
        let _ = writeln!(t, "extern const rl_unit {};", u.symbol);
    }
    t.push_str("extern const uint8_t rl_gb_rom[];\nextern const size_t rl_gb_rom_len;\n");
    if !ram.is_empty() {
        for (_, _, name) in ram {
            let _ = writeln!(t, "RL_LIFTED({name});");
        }
        t.push_str("static const rl_gb_ram ram[] = {\n");
        for (pc, k, name) in ram {
            let _ = writeln!(t, "    {{ {pc:#x}u, {k:#x}u, {name} }},");
        }
        t.push_str("};\n");
    }
    if !halt_bug.is_empty() {
        for (_, _, name) in halt_bug {
            let _ = writeln!(t, "RL_LIFTED({name});");
        }
        t.push_str("static const rl_gb_halt_bug halt_bug[] = {\n");
        for (bank, pc, name) in halt_bug {
            let _ = writeln!(t, "    {{ UINT32_C({bank:#x}), {pc:#x}u, {name} }},");
        }
        t.push_str("};\n");
    }
    if !live.is_empty() {
        for (_, _, _, name) in live {
            let _ = writeln!(t, "RL_LIFTED({name});");
        }
        t.push_str("static const rl_gb_live live[] = {\n");
        for (bank, pc, k, name) in live {
            let _ = writeln!(
                t,
                "    {{ UINT32_C({bank:#x}), {pc:#x}u, {k:#x}u, {name} }},"
            );
        }
        t.push_str("};\n");
    }
    if !sleds.is_empty() {
        t.push_str("static const rl_gb_sled sleds[] = {\n");
        for (bank, start, end) in sleds {
            let _ = writeln!(t, "    {{ UINT32_C({bank:#x}), {start:#x}u, {end:#x}u }},");
        }
        t.push_str("};\n");
    }
    t.push_str("static const rl_gb_unit units[] = {\n");
    let listed = |u: &&Unit| u.bank != RAM_UNIT && u.bank != HALT_BUG_UNIT && u.bank != LIVE_UNIT;
    for u in units.iter().filter(listed) {
        let _ = writeln!(t, "    {{ &{}, UINT32_C({:#x}) }},", u.symbol, u.bank);
    }
    t.push_str("};\nRL_TEXT_BOUNDS_DEFINE()\nextern int relift_info_main(void);\n");
    let mobile = |t: &mut String, header: &str, sig: &str, init: &str, dir: &str| {
        let _ = write!(
            t,
            "#include <stdio.h>\n#include <relift/{header}>\n#include <relift/settings.h>\n{sig}\n{{\n    {init}();\n    rl_settings_set_app(\"relift\");\n    if (argc > 1 && strcmp(argv[1], \"--relift-info\") == 0)\n        return relift_info_main();\n    const char *dir = {dir}();\n    char save[4096];\n    snprintf(save, sizeof save, \"%s/%s\", dir ? dir : \".\", {});\n",
            c_string(p.save)
        );
    };
    match p.entry {
        Entry::Android => mobile(
            &mut t,
            "android.h",
            "__attribute__((visibility(\"default\"))) int SDL_main(int argc, char **argv)",
            "rl_android_init",
            "rl_android_files_dir",
        ),
        Entry::Ios => mobile(
            &mut t,
            "ios.h",
            "int rl_ios_main(int argc, char **argv)",
            "rl_ios_init",
            "rl_ios_documents_dir",
        ),
        Entry::Main => t.push_str(
            "int main(int argc, char **argv)\n{\n    if (argc > 1 && strcmp(argv[1], \"--relift-info\") == 0)\n        return relift_info_main();\n",
        ),
    }
    let _ = write!(
        t,
        "    rl_gb_program prog = {{ rl_gb_rom, rl_gb_rom_len, units, {}u, {}, {}, {}, {}, {}, {}u, {}, {}u, {}, {}u, {}, {}u }};\n    int status = rl_gb_main(&prog, argc, argv);\n{}    return status;\n}}\n",
        units.iter().filter(listed).count(),
        c_string(p.title),
        c_string(p.model),
        if p.entry == Entry::Main {
            c_string(p.save)
        } else {
            "save".into()
        },
        c_string(p.learn),
        if sleds.is_empty() { "NULL" } else { "sleds" },
        sleds.len(),
        if ram.is_empty() { "NULL" } else { "ram" },
        ram.len(),
        if halt_bug.is_empty() {
            "NULL"
        } else {
            "halt_bug"
        },
        halt_bug.len(),
        if live.is_empty() { "NULL" } else { "live" },
        live.len(),
        match p.entry {
            Entry::Android => "    rl_android_flush();\n",
            Entry::Ios => "    rl_ios_flush();\n",
            Entry::Main => "",
        }
    );
    t
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rom() -> Vec<u8> {
        let mut r = vec![0u8; 0x8000];
        r[0x147] = 0x01;
        r[0x148] = 0x00;
        r[0x100..0x104].copy_from_slice(&[0x00, 0xC3, 0x50, 0x01]);
        let prog = [0x3E, 0x01, 0xEA, 0x00, 0x20, 0xCD, 0x00, 0x40, 0x18, 0xF6];
        r[0x150..0x150 + prog.len()].copy_from_slice(&prog);
        r[0x4000..0x4004].copy_from_slice(&[0xC4, 0x10, 0x40, 0xC9]);
        r[0x4010] = 0xC9;
        r
    }

    #[test]
    fn the_fixed_window_and_each_bank_are_units_of_their_own() {
        let rom = rom();
        let report = relift_gb_analyze::analyze(&rom).expect("analyzes");
        let l = lift(&rom, &report, &[], [0; 32]);
        let banks: Vec<u32> = l.units.iter().map(|u| u.bank).collect();
        assert_eq!(banks, [FIXED, 1]);
        assert!(l.unlifted.is_empty(), "{:?}", l.unlifted);
        let bank1: String = l.units[1]
            .output
            .files
            .iter()
            .map(|f| f.text.as_str())
            .collect();
        assert!(bank1.contains("gb_b001_4000"), "{bank1}");
        assert!(bank1.contains("rl_gb_sync(c, mem,"), "{bank1}");
        assert!(bank1.contains("UINT32_C(0x40030029)"), "{bank1}");
        let fixed: String = l.units[0]
            .output
            .files
            .iter()
            .map(|f| f.text.as_str())
            .collect();
        assert!(fixed.contains("rl_gb_io_write(c, mem,"), "{fixed}");
        assert!(!fixed.contains("a_ <= UINT64_C("), "{fixed}");
        assert!(
            fixed.contains("rl_indirect(c, UINT64_C(0x4000))"),
            "{fixed}"
        );
    }

    #[test]
    fn a_flag_write_an_instruction_later_overwrites_survives_its_sync_point() {
        let mut r = rom();
        r[0x150..0x153].copy_from_slice(&[0x1C, 0x1C, 0xC9]);
        let report = relift_gb_analyze::analyze(&r).expect("analyzes");
        let l = lift(&r, &report, &[], [0; 32]);
        let c: String = l.units[0]
            .output
            .files
            .iter()
            .map(|f| f.text.as_str())
            .collect();
        let first = c.split("L_150: {").nth(1).expect("the first INC E");
        let first = first.split("L_151: {").next().expect("ends");
        assert!(first.contains("c->f = (c->f & ~(1u << 5))"), "{first}");
        assert!(first.contains("c->f = (c->f & ~(1u << 6))"), "{first}");
    }

    #[test]
    fn a_long_run_of_nop_is_a_sled() {
        let mut r = rom();
        r[0x150..0x158].copy_from_slice(&[0x3E, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]);
        for b in &mut r[0x158..0x200] {
            *b = 0;
        }
        r[0x200..0x202].copy_from_slice(&[0x18, 0xFE]);
        let report = relift_gb_analyze::analyze(&r).expect("analyzes");
        let l = lift(&r, &report, &[], [0; 32]);
        let c: String = l.units[0]
            .output
            .files
            .iter()
            .map(|f| f.text.as_str())
            .collect();
        assert!(c.contains("L_159: {"), "{c}");
        assert!(!c.contains("L_15a: {"), "{c}");
        assert_eq!(l.filler, 0);
        assert!(l.sleds.contains(&(FIXED, 0x15A, 0x200)), "{:?}", l.sleds);
        assert!(c.contains("RL_JUMP(UINT64_C(0x15a), "), "{c}");
        let prog = program_c(
            &l.units,
            &l.sleds,
            &l.ram,
            &l.halt_bug,
            &l.live,
            &Program {
                title: "T",
                model: "dmg",
                save: "",
                entry: Entry::Main,
                learn: "",
            },
        );
        assert!(
            prog.contains("{ UINT32_C(0xffffffff), 0x15au, 0x200u },"),
            "{prog}"
        );
    }

    #[test]
    fn every_lifted_instruction_is_an_entry_of_dispatch() {
        let rom = rom();
        let report = relift_gb_analyze::analyze(&rom).expect("analyzes");
        let l = lift(&rom, &report, &[], [0; 32]);
        let unit = l.units[0]
            .output
            .files
            .iter()
            .find(|f| f.name == "unit.c")
            .expect("unit.c");
        assert!(
            unit.text
                .contains("{ UINT64_C(0x152), 0u, RL_FN_MAY_UNWIND | RL_FN_FRAGMENT,"),
            "{}",
            unit.text
        );
    }

    #[test]
    fn a_branch_after_a_sync_takes_the_flag_from_before_the_sync() {
        let mut r = rom();
        let prog = [0xAF, 0xFB, 0xE0, 0x0F, 0x28, 0xFE, 0x18, 0xFE];
        r[0x150..0x150 + prog.len()].copy_from_slice(&prog);
        let report = relift_gb_analyze::analyze(&r).expect("analyzes");
        let l = lift(&r, &report, &[], [0; 32]);
        let c: String = l.units[0]
            .output
            .files
            .iter()
            .map(|f| f.text.as_str())
            .collect();
        let start = c.find("L_154: {").expect("the jr z block");
        let body = &c[start..start + c[start..].find("\n}\n").expect("its end")];
        let sync =
            body.find("rl_gb_sync(c, mem, (").expect("a sync") + "rl_gb_sync(c, mem, (".len();
        let cond = &body[sync..sync + body[sync..].find(' ').expect("a condition")];
        assert!(cond.starts_with('v'), "{body}");
        assert!(
            body.contains(&format!("if ({cond}) {{ goto L_154; }}")),
            "{body}"
        );
    }

    #[test]
    fn a_seen_root_at_an_illegal_opcode_is_no_function() {
        let mut r = rom();
        r[0x300] = 0xD3;
        let report = relift_gb_analyze::analyze_with(&r, &[Loc::fixed(0x300)]).expect("analyzes");
        let l = lift(&r, &report, &[], [0; 32]);
        let c: String = l.units[0]
            .output
            .files
            .iter()
            .map(|f| f.text.as_str())
            .collect();
        assert!(!c.contains("goto L_300;") || c.contains("L_300: {"), "{c}");
    }

    #[test]
    fn the_superset_lifts_bytes_discovery_did_not_reach() {
        let mut r = rom();
        r[0x300..0x303].copy_from_slice(&[0x3E, 0x07, 0xC9]);
        let report = relift_gb_analyze::analyze(&r).expect("analyzes");
        let fixed = |l: &Lifted| -> String {
            l.units[0]
                .output
                .files
                .iter()
                .map(|f| f.text.as_str())
                .collect()
        };
        let plain = lift(&r, &report, &[], [0; 32]);
        assert!(!fixed(&plain).contains("L_300: {"));
        let all = lift_superset(&r, &report, &[], [0; 32], &[FIXED]);
        let c = fixed(&all);
        assert!(c.contains("L_300: {"), "{c}");
        assert!(c.contains("L_301: {"), "{c}");
    }

    #[test]
    fn the_superset_lifts_an_unused_opcode_as_the_library_lock() {
        let mut r = rom();
        r[0x300..0x302].copy_from_slice(&[0xD3, 0xC9]);
        let report = relift_gb_analyze::analyze(&r).expect("analyzes");
        let all = lift_superset(&r, &report, &[], [0; 32], &[FIXED]);
        let c: String = all.units[0]
            .output
            .files
            .iter()
            .map(|f| f.text.as_str())
            .collect();
        assert!(c.contains("L_300: {"), "{c}");
        assert!(c.contains("rl_gb_trap(c, mem, 0xd30301u)"), "{c}");
        assert!(c.contains("L_301: {"), "{c}");
        assert!(!c.contains("rl_undefined"), "{c}");
    }

    #[test]
    fn the_instruction_after_halt_is_lifted_as_the_halt_bug_runs_it() {
        let mut r = rom();
        r[0x150..0x154].copy_from_slice(&[0x76, 0x3E, 0x07, 0xC9]);
        let report = relift_gb_analyze::analyze(&r).expect("analyzes");
        let l = lift(&r, &report, &[], [0; 32]);
        assert_eq!(l.halt_bug, [(FIXED, 0x151, "gb_hb_f_0151".to_string())]);
        let hb: String = l
            .units
            .iter()
            .filter(|u| u.bank == HALT_BUG_UNIT)
            .flat_map(|u| u.output.files.iter().map(|f| f.text.as_str()))
            .collect();
        assert!(hb.contains("gb_hb_f_0151"), "{hb}");
        assert!(hb.contains("0x3e"), "{hb}");
        assert!(hb.contains("UINT64_C(0x152)"), "{hb}");
        let prog = program_c(
            &l.units,
            &l.sleds,
            &l.ram,
            &l.halt_bug,
            &l.live,
            &Program {
                title: "T",
                model: "dmg",
                save: "",
                entry: Entry::Main,
                learn: "",
            },
        );
        assert!(
            prog.contains("{ UINT32_C(0xffffffff), 0x151u, gb_hb_f_0151 },"),
            "{prog}"
        );
        assert!(!prog.contains("&rl_unit_gb_hb0"), "{prog}");
    }

    #[test]
    fn an_opcode_slot_in_ram_gets_every_key() {
        let mut ram: Vec<(u16, u16)> = (0..40).map(|k| (0xD820, k)).collect();
        ram.push((0xC000, 0x3E));
        let all = complete_ram_slots(&ram, 32);
        assert_eq!(all.iter().filter(|(a, _)| *a == 0xD820).count(), 0x200);
        assert_eq!(all.iter().filter(|(a, _)| *a == 0xC000).count(), 1);
    }

    #[test]
    fn a_halt_in_ram_code_gets_the_forms_of_what_follows_it() {
        let mut by_pc: BTreeMap<u16, BTreeSet<u16>> = BTreeMap::new();
        by_pc.entry(0xC000).or_default().insert(0x76);
        by_pc.entry(0xC001).or_default().extend([0x3E, 0x1A1]);
        by_pc.entry(0xC010).or_default().insert(0x3E);
        assert_eq!(ram_halt_bug_keys(&by_pc), [(0xC000, 0x3E), (0xC000, 0x1CB)]);
    }

    #[test]
    fn live_rom_instructions_are_lifted_with_live_operands() {
        let live = [
            (Loc::fixed(0x0A2D), 0x00),
            (
                Loc {
                    bank: 3,
                    addr: 0x4100,
                },
                0x3E,
            ),
            (Loc::fixed(0x0A2D), 0xCB),
            (Loc::fixed(0x9000), 0x00),
            (Loc::fixed(0x1031), 0x13C),
        ];
        let mut lifted = Lifted::default();
        lift_live(&live, [0; 32], &mut lifted);
        assert_eq!(
            lifted.live,
            [
                (3, 0x4100, 0x3E, "gb_live_b003_4100_3e".to_string()),
                (FIXED, 0x0A2D, 0x00, "gb_live_f_0a2d_00".to_string()),
                (FIXED, 0x1031, 0x13C, "gb_live_f_1031_13c".to_string()),
            ]
        );
        let c: String = lifted
            .units
            .iter()
            .filter(|u| u.bank == LIVE_UNIT)
            .flat_map(|u| u.output.files.iter().map(|f| f.text.as_str()))
            .collect();
        assert!(c.contains("rl_gb_operand(c, ((uint8_t)0x0))"), "{c}");
    }

    #[test]
    fn the_learned_file_gives_rom_locations_and_ram_opcodes() {
        let l =
            read_learned("001:4abc\n000:0150\nram c000 0c3\nram ff80 13e\nram 1000 000\nnoise\n");
        assert_eq!(
            l.rom,
            [
                Loc {
                    bank: 1,
                    addr: 0x4ABC
                },
                Loc::fixed(0x150)
            ]
        );
        assert_eq!(l.ram, [(0xC000, 0xC3), (0xFF80, 0x13E)]);
    }

    #[test]
    fn ram_instructions_read_their_operands_as_they_run() {
        let rom = rom();
        let report = relift_gb_analyze::analyze(&rom).expect("analyzes");
        let l = lift(
            &rom,
            &report,
            &[(0xC000, 0xC3), (0xC000, 0x3E), (0xFF80, 0x13E)],
            [0; 32],
        );
        assert_eq!(
            l.ram.iter().map(|(pc, k, _)| (*pc, *k)).collect::<Vec<_>>(),
            [(0xC000, 0x3E), (0xFF80, 0x13E), (0xC000, 0xC3)]
        );
        let ram: String = l
            .units
            .iter()
            .filter(|u| u.bank == RAM_UNIT)
            .flat_map(|u| u.output.files.iter().map(|f| f.text.as_str()))
            .collect();
        assert!(ram.contains("rl_gb_operand(c, "), "{ram}");
        assert!(ram.contains("gb_ram_c000_0c3"), "{ram}");
        let prog = program_c(
            &l.units,
            &l.sleds,
            &l.ram,
            &l.halt_bug,
            &l.live,
            &Program {
                title: "T",
                model: "dmg",
                save: "",
                entry: Entry::Main,
                learn: "",
            },
        );
        assert!(
            prog.contains("{ 0xc000u, 0xc3u, gb_ram_c000_0c3 },"),
            "{prog}"
        );
        assert!(!prog.contains("&rl_unit_gb_ram0"), "{prog}");
    }

    #[test]
    fn the_program_lists_the_units_with_their_banks() {
        let rom = rom();
        let report = relift_gb_analyze::analyze(&rom).expect("analyzes");
        let l = lift(&rom, &report, &[], [0; 32]);
        let c = program_c(
            &l.units,
            &l.sleds,
            &l.ram,
            &l.halt_bug,
            &l.live,
            &Program {
                title: "T\"",
                model: "cgb",
                save: "a.sav",
                entry: Entry::Main,
                learn: "",
            },
        );
        assert!(
            c.contains("{ &rl_unit_gb_fixed, UINT32_C(0xffffffff) },"),
            "{c}"
        );
        assert!(c.contains("{ &rl_unit_gb_b001, UINT32_C(0x1) },"), "{c}");
        assert!(c.contains("\"T\\\"\", \"cgb\", \"a.sav\", \"\""), "{c}");
    }

    #[test]
    fn the_android_entry_is_sdl_main_with_the_save_in_the_files_dir() {
        let rom = rom();
        let report = relift_gb_analyze::analyze(&rom).expect("analyzes");
        let l = lift(&rom, &report, &[], [0; 32]);
        let c = program_c(
            &l.units,
            &l.sleds,
            &l.ram,
            &l.halt_bug,
            &l.live,
            &Program {
                title: "T",
                model: "cgb",
                save: "a.sav",
                entry: Entry::Android,
                learn: "",
            },
        );
        assert!(c.contains("int SDL_main(int argc, char **argv)"), "{c}");
        assert!(!c.contains("int main("), "{c}");
        assert!(c.contains("rl_android_files_dir()"), "{c}");
        assert!(c.contains("\"T\", \"cgb\", save, \"\""), "{c}");
    }

    #[test]
    fn the_ios_entry_is_run_by_the_glue_with_the_save_in_documents() {
        let rom = rom();
        let report = relift_gb_analyze::analyze(&rom).expect("analyzes");
        let l = lift(&rom, &report, &[], [0; 32]);
        let c = program_c(
            &l.units,
            &l.sleds,
            &l.ram,
            &l.halt_bug,
            &l.live,
            &Program {
                title: "T",
                model: "cgb",
                save: "a.sav",
                entry: Entry::Ios,
                learn: "",
            },
        );
        assert!(c.contains("int rl_ios_main(int argc, char **argv)"), "{c}");
        assert!(!c.contains("int main("), "{c}");
        assert!(c.contains("rl_ios_documents_dir()"), "{c}");
        assert!(c.contains("rl_ios_flush();"), "{c}");
    }
}
