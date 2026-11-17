//! A ROM as lifted units and the program files that run it.
//!
//! Every segment `relift-n64-analyze` placed at an address in RDRAM becomes
//! one unit: each code word of the segment is lifted at its link address,
//! and the functions hold the instructions their entries reach (the
//! analysis's entries first, then one at the first word no function
//! reached, which stops at the words of earlier functions, until every
//! code word is in one). Dispatch may enter any
//! instruction, since an exception handler returns wherever its exception
//! was raised.
//!
//! No unit is in the dispatch table at the start. The program lists each
//! code run of the analysis (a [`relift_n64_analyze::Unit`]) with its
//! source, offset, length, link address and hash; the runtime binds a run
//! when the machine copies its source bytes to its link address (a PI DMA,
//! or the boot), checks the hash against RDRAM on the first entry into it
//! and enters the unit's functions only when it matches. A jump to an
//! address with no bound run, or a run whose bytes in RDRAM differ, is
//! missed code; nothing is interpreted. A call or jump from one run to
//! another goes through dispatch: the other run may hold other code by
//! then (an overlay loaded over the boot image's bytes), which only its
//! check sees.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;

use relift_emit::{Function, Insn, Options, Output};
use relift_guest::LiftFailure;
use relift_ir::{Block, Flow, Next, Op};
use relift_n64_analyze::{Boot, Report, Source};
use relift_n64_formats::Cic;

use crate::frontend::{
    ICACHE_LINE, N64, RDRAM_END, dispatch_exit, fetch_check, in_sp_memory, io_map,
};

/// The physical address bits of a kseg0 or kseg1 address.
const PHYS: u32 = 0x1FFF_FFFF;

/// An instruction that did not lift.
#[derive(Clone, Debug)]
pub struct Unlifted {
    /// The segment.
    pub segment: u32,
    /// Where.
    pub pc: u64,
    /// Why.
    pub failure: LiftFailure,
}

/// One lifted segment.
#[derive(Clone, Debug)]
pub struct SegmentUnit {
    /// The segment, an index into the report's segments.
    pub segment: u32,
    /// The C symbol of its unit.
    pub symbol: String,
    /// The directory name of its files.
    pub dir: String,
    /// The emitted files.
    pub output: Output,
    /// Its functions.
    pub functions: usize,
}

/// A code run the runtime binds, as the program lists it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Code {
    /// The lifted unit, an index into [`Lifted::units`].
    pub unit: usize,
    /// 0 for the ROM, `1 + n` for decoded stream `n`.
    pub source: u32,
    /// The offset in the source.
    pub offset: u32,
    /// The length in bytes.
    pub len: u32,
    /// The link address, sign-extended.
    pub link: u64,
    /// FNV-1a (64-bit) of the words, big-endian.
    pub hash: u64,
}

/// Everything a ROM lifts to.
#[derive(Clone, Debug, Default)]
pub struct Lifted {
    /// One unit per lifted segment.
    pub units: Vec<SegmentUnit>,
    /// The code runs, by unit and link address.
    pub code: Vec<Code>,
    /// Instructions lifted.
    pub instructions: usize,
    /// Instructions that did not lift.
    pub unlifted: Vec<Unlifted>,
    /// Segments left out, with the reason.
    pub skipped: Vec<(u32, &'static str)>,
}

impl Lifted {
    /// The functions of every unit.
    #[must_use]
    pub fn functions(&self) -> usize {
        self.units.iter().map(|u| u.functions).sum()
    }
}

/// A kseg0 address as the VR4300 holds it.
#[must_use]
pub fn sext(a: u32) -> u64 {
    a as i32 as i64 as u64
}

fn be32(b: &[u8], at: usize) -> Option<u32> {
    let w = b.get(at..at.checked_add(4)?)?;
    Some(u32::from_be_bytes([w[0], w[1], w[2], w[3]]))
}

/// The host name of the function at `pc` of segment `seg`.
#[must_use]
pub fn host_name(seg: u32, pc: u64) -> String {
    format!("n64_s{seg}_{:08x}", pc as u32)
}

/// The C symbol of the unit of segment `seg`.
#[must_use]
pub fn unit_symbol(seg: u32) -> String {
    format!("rl_unit_n64_s{seg}")
}

type Blocks = BTreeMap<u64, (Result<Block, LiftFailure>, u64)>;

/// The code runs of a segment, as `[first, end)` address ranges.
#[derive(Clone, Debug, Default)]
struct Runs(Vec<(u64, u64)>);

impl Runs {
    /// Whether one run holds both `a` and `b`: a transfer between them
    /// stays in code the runtime bound in one piece.
    fn together(&self, a: u64, b: u64) -> bool {
        self.0
            .iter()
            .any(|&(lo, hi)| (lo..hi).contains(&a) && (lo..hi).contains(&b))
    }
}

/// Makes every instruction a run may start a fetch at go through the
/// instruction cache ([`fetch_check`]): the start of a line, the entries
/// of `entries`, a branch or jump target and the return of a call. An
/// instruction reached only from the one before it in the same line is
/// in the cache already.
fn fetch_entries(blocks: &mut Blocks, entries: &[u64]) {
    let mut at: BTreeSet<u64> = entries.iter().copied().collect();
    for (&pc, (r, _)) in blocks.iter() {
        if pc.is_multiple_of(ICACHE_LINE) {
            at.insert(pc);
        }
        let Ok(b) = r else {
            continue;
        };
        let after = pc.wrapping_add(u64::from(b.len));
        if let Flow::Call { ret } = b.exit.flow {
            at.insert(ret);
        }
        match b.exit.next {
            Next::Const(t) if t != after => {
                at.insert(t);
            }
            Next::Select {
                taken, not_taken, ..
            } => {
                at.insert(taken);
                if not_taken != after {
                    at.insert(not_taken);
                }
            }
            _ => {}
        }
    }
    for pc in at {
        if let Some((Ok(b), _)) = blocks.get_mut(&pc) {
            fetch_check(b, pc);
        }
    }
}

/// The instructions a run from `entry` reaches without calls, stopping at
/// those of `taken` (other functions', which a jump there enters through
/// dispatch).
fn reach(blocks: &Blocks, entry: u64, taken: &BTreeSet<u64>) -> BTreeSet<u64> {
    let mut seen = BTreeSet::new();
    let mut todo = vec![entry];
    while let Some(pc) = todo.pop() {
        if !blocks.contains_key(&pc) || (pc != entry && taken.contains(&pc)) || !seen.insert(pc) {
            continue;
        }
        let Some((Ok(b), _)) = blocks.get(&pc) else {
            continue;
        };
        let after = pc.wrapping_add(u64::from(b.len));
        match (b.exit.flow, b.exit.next) {
            (Flow::Call { ret }, _) => todo.push(ret),
            (Flow::Trap(_), _) => todo.push(after),
            (_, Next::Const(t)) => todo.push(t),
            (
                _,
                Next::Select {
                    taken, not_taken, ..
                },
            ) => {
                todo.push(taken);
                todo.push(not_taken);
            }
            (_, Next::Dynamic(_)) => {}
        }
        if b.insts.iter().any(|i| matches!(i.op, Op::Guard(..))) {
            todo.push(after);
        }
    }
    seen
}

/// The bytes of `source`: the ROM or a decoded stream.
fn source_bytes<'a>(rom: &'a [u8], streams: &'a [Vec<u8>], s: Source) -> &'a [u8] {
    match s {
        Source::Rom => rom,
        Source::Stream(i) => streams.get(i as usize).map_or(&[][..], Vec::as_slice),
    }
}

/// Lifts every segment of `report` (the analysis of the big-endian image
/// `rom`) that is linked in RDRAM or SP memory (the boot code) and emits
/// one unit for each; `image_id`
/// identifies the ROM to the runtime.
#[must_use]
pub fn lift(rom: &[u8], report: &Report, image_id: [u8; 32]) -> Lifted {
    let streams: Vec<Vec<u8>> = report
        .streams
        .iter()
        .map(|s| s.kind.decode(&rom[s.offset..]).unwrap_or_default())
        .collect();
    let fe = N64;
    let mut out = Lifted::default();
    for (si, seg) in report.segments.iter().enumerate() {
        let si = si as u32;
        let units: Vec<_> = report.units.iter().filter(|u| u.segment == si).collect();
        if units.is_empty() {
            continue;
        }
        let Some(link) = seg.link else {
            out.skipped.push((si, "no link address"));
            continue;
        };
        if u64::from(link & PHYS) >= RDRAM_END && !in_sp_memory(sext(link)) {
            out.skipped.push((si, "linked outside RDRAM and SP memory"));
            continue;
        }
        let bytes = source_bytes(rom, &streams, seg.source);
        let end = seg.offset.saturating_add(seg.len) as usize;
        let runs = Runs(
            units
                .iter()
                .map(|u| {
                    let first = sext(u.link.unwrap_or(link));
                    (first, first.wrapping_add(u64::from(u.len)))
                })
                .collect(),
        );
        let mut blocks: Blocks = BTreeMap::new();
        for u in &units {
            for k in (0..u.len).step_by(4) {
                let off = (u.offset + k) as usize;
                let pc = sext(u.link.unwrap_or(link).wrapping_add(k));
                let w = be32(bytes, off).unwrap_or(0);
                let slot = if off + 8 <= end {
                    be32(bytes, off + 4).unwrap_or(0)
                } else {
                    0
                };
                let mut r = fe.lift_at(pc, w, slot);
                if let Ok(b) = &mut r {
                    dispatch_exit(b, |t| runs.together(pc, t));
                }
                if let Err(e) = &r {
                    out.unlifted.push(Unlifted {
                        segment: si,
                        pc,
                        failure: e.clone(),
                    });
                }
                blocks.insert(pc, (r, u64::from(w) | (u64::from(slot) << 32)));
            }
        }
        out.instructions += blocks.len();
        let mut entries: Vec<u64> = report
            .functions
            .iter()
            .filter(|f| f.segment == si)
            .map(|f| sext(f.addr))
            .filter(|pc| blocks.contains_key(pc))
            .collect();
        entries.sort_unstable();
        entries.dedup();
        fetch_entries(&mut blocks, &entries);
        let mut covered: BTreeSet<u64> = BTreeSet::new();
        let mut fns: BTreeMap<u64, BTreeSet<u64>> = BTreeMap::new();
        let none = BTreeSet::new();
        for &e in &entries {
            let r = reach(&blocks, e, &none);
            covered.extend(&r);
            fns.insert(e, r);
        }
        let found = covered.clone();
        let pcs: Vec<u64> = blocks.keys().copied().collect();
        for pc in pcs {
            if covered.contains(&pc) {
                continue;
            }
            let r = reach(&blocks, pc, &covered);
            covered.extend(&r);
            fns.insert(pc, r);
        }
        let functions: Vec<Function<'_>> = fns
            .iter()
            .map(|(&entry, pcs)| {
                let last = pcs
                    .iter()
                    .copied()
                    .filter(|p| *p >= entry)
                    .max()
                    .unwrap_or(entry);
                Function {
                    entry,
                    name: host_name(si, entry),
                    guest_name: None,
                    span: last - entry + 4,
                    insts: pcs
                        .iter()
                        .map(|pc| {
                            let (r, raw) = &blocks[pc];
                            let i = match r {
                                Ok(block) => Insn::Lifted { block, raw: *raw },
                                Err(e) => Insn::Failed(e),
                            };
                            (*pc, i)
                        })
                        .collect(),
                    tables: BTreeMap::new(),
                    guesses: BTreeMap::new(),
                    pads: BTreeSet::new(),
                }
            })
            .collect();
        let symbol = unit_symbol(si);
        let opts = Options {
            unit_name: format!("seg{si}"),
            unit_symbol: symbol.clone(),
            module_base: sext(link),
            io_map: io_map(),
            image_id,
            enter_anywhere: Some(u64::MAX),
            entry_ranges: true,
            ..Options::default()
        };
        let output = relift_emit::emit(&fe, &functions, &[], &opts);
        let index = out.units.len();
        for u in &units {
            let source = match u.source {
                Source::Rom => 0,
                Source::Stream(n) => n + 1,
            };
            let first = sext(u.link.unwrap_or(link));
            out.code.push(Code {
                unit: index,
                source,
                offset: u.offset,
                len: u.len,
                link: first,
                hash: u.hash,
            });
            if let Some((k0, k1)) = core(&found, first, u.len)
                && (k0, k1) != (0, u.len)
                && u.reloc == 0
            {
                let lo = (u.offset + k0) as usize;
                let hi = (u.offset + k1) as usize;
                out.code.push(Code {
                    unit: index,
                    source,
                    offset: u.offset + k0,
                    len: k1 - k0,
                    link: first.wrapping_add(u64::from(k0)),
                    hash: fnv(bytes.get(lo..hi).unwrap_or_default()),
                });
            }
        }
        out.units.push(SegmentUnit {
            segment: si,
            symbol,
            dir: format!("seg{si}"),
            output,
            functions: functions.len(),
        });
    }
    out
}

/// The part of the run of `len` bytes at `first` that the functions the
/// analysis entered cover, from the first such word to the last, as byte
/// offsets: the run less the words only the sweep added at its ends, which
/// may be data a loader does not bring along.
fn core(found: &BTreeSet<u64>, first: u64, len: u32) -> Option<(u32, u32)> {
    let end = first.wrapping_add(u64::from(len));
    let mut inside = found.range(first..end);
    let a = *inside.next()?;
    let b = inside.next_back().copied().unwrap_or(a);
    Some(((a - first) as u32, (b + 4 - first) as u32))
}

/// FNV-1a (64-bit) of `b`.
fn fnv(b: &[u8]) -> u64 {
    b.iter().fold(0xCBF2_9CE4_8422_2325, |h, &x| {
        (h ^ u64::from(x)).wrapping_mul(0x0100_0000_01B3)
    })
}

/// The boot variant number the runtime's boot takes: 6101, 6102, 6103,
/// 6105, 6106, 7102 or 8303, or 0 when unknown (booted as 6102).
#[must_use]
pub fn cic_number(boot: Boot) -> u32 {
    match boot {
        Boot::Standard(c) => match c {
            Cic::C6101 => 6101,
            Cic::C6102 => 6102,
            Cic::C6103 => 6103,
            Cic::C6105 => 6105,
            Cic::C6106 => 6106,
            Cic::C7102 => 7102,
            Cic::C8303 => 8303,
            _ => 0,
        },
        Boot::Elf(_) | Boot::Unknown => 0,
    }
}

/// What the program file needs besides the units.
#[derive(Clone, Debug)]
pub struct Program<'a> {
    /// The title, for reports.
    pub title: &'a str,
    /// The path of the imported ROM, which names the save files; empty for none.
    pub rom_path: &'a str,
    /// The units' symbols, in [`Code::unit`] order.
    pub units: &'a [String],
    /// The code runs.
    pub code: &'a [Code],
    /// The boot variant ([`cic_number`]).
    pub cic: u32,
    /// The entry point the boot jumps to.
    pub entry: u32,
    /// The developer's picture defaults, `name=value` texts for
    /// `rl_n64_declare_picture` (empty: the console's picture).
    pub picture: &'a [String],
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

/// `rom.c`, which embeds `rom.bin` (the big-endian ROM).
#[must_use]
pub fn rom_c(rom: &[u8]) -> String {
    let mut t = relift_guest::blob::blob_c("rl_n64_rom", "c/rom.bin", rom);
    t.push_str("#include <stddef.h>\nextern const size_t rl_n64_rom_len;\n");
    let _ = writeln!(t, "const size_t rl_n64_rom_len = {}u;", rom.len());
    t
}

/// `program.c`: the units, the code runs and `main`, which takes the RSP
/// images from `rsp_images.c` ([`crate::rsp::images_c`]).
#[must_use]
pub fn program_c(p: &Program<'_>) -> String {
    let mut s = String::from(
        "#include <stddef.h>\n#include <stdint.h>\n#include <string.h>\n#include \"n64main.h\"\n",
    );
    for u in p.units {
        let _ = writeln!(s, "extern const rl_unit {u};");
    }
    let units: Vec<String> = p.units.iter().map(|u| format!("&{u}")).collect();
    if units.is_empty() {
        s.push_str("static const rl_unit *const units[] = { NULL };\n");
    } else {
        let _ = writeln!(
            s,
            "static const rl_unit *const units[] = {{ {} }};",
            units.join(", ")
        );
    }
    s.push_str("static const rl_n64_code code[] = {\n");
    for c in p.code {
        let _ = writeln!(
            s,
            "    {{ {}u, {}u, {:#x}u, {:#x}u, UINT64_C({:#x}), UINT64_C({:#x}) }},",
            c.unit, c.source, c.offset, c.len, c.link, c.hash
        );
    }
    if p.code.is_empty() {
        s.push_str("    { 0u, 0u, 0u, 0u, 0u, 0u },\n");
    }
    s.push_str("};\nstatic const char *const picture[] = { ");
    for d in p.picture {
        let _ = write!(s, "{}, ", c_string(d));
    }
    s.push_str("NULL };\n");
    let _ = write!(
        s,
        "extern const uint8_t rl_n64_rom[];\nextern const size_t rl_n64_rom_len;\nextern const rl_rsp_image *const rl_n64_rsp_images[];\nextern const uint32_t rl_n64_rsp_count;\nRL_TEXT_BOUNDS_DEFINE()\nextern int relift_info_main(void);\nextern const rl_n64_renderer rl_n64_prdp;\nint main(int argc, char **argv)\n{{\n    if (argc > 1 && strcmp(argv[1], \"--relift-info\") == 0)\n        return relift_info_main();\n    rl_n64_program prog = {{ rl_n64_rom, rl_n64_rom_len, units, {}u, code, {}u, rl_n64_rsp_images, rl_n64_rsp_count, {}, {}u, {:#x}u, {}, &rl_n64_prdp, picture }};\n    return rl_n64_main(&prog, argc, argv);\n}}\n",
        p.units.len(),
        p.code.len(),
        c_string(p.title),
        p.cic,
        p.entry,
        c_string(p.rom_path)
    );
    s
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_core_of_a_run_drops_only_its_sweep_only_ends() {
        let found: BTreeSet<u64> = [0x110, 0x114, 0x120].into_iter().collect();
        assert_eq!(core(&found, 0x100, 0x40), Some((0x10, 0x24)));
        assert_eq!(core(&found, 0x110, 0x14), Some((0, 0x14)));
        assert_eq!(core(&found, 0x200, 0x40), None);
    }
    use relift_n64_formats::synth::{self, Spec};

    /// A ROM whose boot image at 0x80001000 calls a leaf and loops.
    fn rom() -> Vec<u8> {
        let mut r = synth::rom(&Spec {
            entry: 0x8000_1000,
            crc_as: Some(Cic::C6102),
            ..Spec::default()
        });
        let code: [u32; 6] = [
            0x0C00_0404, // jal 0x80001010
            0x0000_0000,
            0x1000_FFFF, // b .
            0x0000_0000,
            0x03E0_0008, // jr ra
            0x2402_0001, // li v0, 1
        ];
        for (i, w) in code.iter().enumerate() {
            r[0x1000 + 4 * i..0x1004 + 4 * i].copy_from_slice(&w.to_be_bytes());
        }
        r
    }

    /// A ROM whose boot image calls a leaf in a code run of its own, past
    /// data.
    fn two_runs() -> Vec<u8> {
        let mut r = rom();
        for i in 0..0x40 {
            let at = 0x1020 + 4 * i;
            r[at..at + 4].copy_from_slice(&0xDEAD_BEEFu32.to_be_bytes());
        }
        let code: [(usize, u32); 4] = [
            (0x1000, 0x0C00_0440), // jal 0x80001100
            (0x1010, 0x1000_FFFF), // b . (the old leaf is gone)
            (0x1100, 0x03E0_0008), // jr ra
            (0x1104, 0x2402_0001), // li v0, 1
        ];
        for (at, w) in code {
            r[at..at + 4].copy_from_slice(&w.to_be_bytes());
        }
        r
    }

    #[test]
    fn a_call_into_another_code_run_goes_through_dispatch() {
        let r = two_runs();
        let report = relift_n64_analyze::analyze_with(
            &r,
            relift_n64_analyze::Options {
                cic: Some(Cic::C6102),
            },
        )
        .expect("analyzes");
        let runs: Vec<Option<u32>> = report.units.iter().map(|u| u.link).collect();
        assert_eq!(runs, [Some(0x8000_1000), Some(0x8000_1100)]);
        let l = lift(&r, &report, [0; 32]);
        let c: String = l.units[0]
            .output
            .files
            .iter()
            .map(|f| f.text.as_str())
            .collect();
        assert!(
            c.contains("rl_indirect(c, UINT64_C(0xffffffff80001100))"),
            "{c}"
        );
        assert!(!c.contains("rc = n64_s0_80001100(c, mem)"), "{c}");
    }

    #[test]
    fn the_boot_segment_lifts_into_one_unit_with_its_code_runs() {
        let r = rom();
        let report = relift_n64_analyze::analyze_with(
            &r,
            relift_n64_analyze::Options {
                cic: Some(Cic::C6102),
            },
        )
        .expect("analyzes");
        let l = lift(&r, &report, [0; 32]);
        assert!(l.unlifted.is_empty(), "{:?}", l.unlifted);
        assert_eq!(l.units.len(), 1);
        let c: String = l.units[0]
            .output
            .files
            .iter()
            .map(|f| f.text.as_str())
            .collect();
        assert!(c.contains("n64_s0_80001000"), "{c}");
        assert!(c.contains("rl_mips_call(c,"), "{c}");
        assert!(
            l.code
                .iter()
                .any(|k| k.link == 0xFFFF_FFFF_8000_1000 && k.source == 0)
        );
        let p = program_c(&Program {
            title: "T",
            rom_path: "",
            units: &[l.units[0].symbol.clone()],
            code: &l.code,
            cic: cic_number(report.boot),
            entry: report.entry,
            picture: &["upscale=2x".into()],
        });
        assert!(p.contains("units[] = { &rl_unit_n64_s0 }"), "{p}");
        assert!(p.contains("picture[] = { \"upscale=2x\", NULL };"), "{p}");
        assert!(p.contains("&rl_n64_prdp, picture }"), "{p}");
        assert!(p.contains("\"T\", 6102u, 0x80001000u"), "{p}");
    }
}
