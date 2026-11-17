//! Function discovery for Nintendo DS programs across ARM and Thumb code.
//!
//! The analysis is `relift-cpu-arm32-analyze` on the DS machine, one run
//! per unit ([`Unit`]): the static binary of each processor with its code
//! autoload blocks, and each overlay over the static binary of its
//! processor. A unit's bytes are the image of its run (the decompressed
//! static binary at its load address, its ITCM and main-RAM autoload
//! blocks where crt0 copies them, and in an overlay run the overlay at its
//! RAM address); the rest of the address space is the processor's memory
//! map ([`Nds`]). The ARM9 is lifted for ARMv5TE, the ARM7 for ARMv4T.
//!
//! Every run uses the rules [`relift_cpu_arm32_analyze::Options`] lists.
//! The other overlays are [`Platform::external`] code: a call there
//! returns. An overlay run takes as entries the overlay's static
//! initializers and finds only its own code through the code addresses it
//! takes; the static binary's code it reaches is found by calls. Calls
//! from one run into an overlay of the same processor (to RAM no copy
//! holds there, [`Report::outside`]) seed that overlay's run, and the
//! runs repeat until no seed is new.
//!
//! Each report also lists the places a suspended thread resumes at inside
//! its functions ([`resume`]), which become entries of their own.

use std::collections::{BTreeMap, BTreeSet};
use std::sync::Mutex;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::{Duration, Instant};

use relift_cpu_arm32_analyze::{BiosCall, Config, Dma, Kind, Options, Platform, Profile, discover};
use relift_nds_formats::checksum::sha256;
use relift_nds_formats::{Cpu, Rom, StaticUnit};

pub mod resume;

pub use relift_cpu_arm32_analyze::{
    CopyKind, Edge, EdgeKind, Function, Loc, Origin, RamCopy, RegionCoverage, Report, Swi, Table,
    Unresolved, UnresolvedKind,
};

/// The data TCM base the SDK sets on the ARM9 when no autoload block
/// names it.
const DTCM: u32 = 0x027E_0000;
/// The IRQ handler slot at the end of the ARM9's data TCM.
const DTCM_IRQ: u32 = 0x3FFC;
/// The ARM7's IRQ handler slot, at the end of its work RAM mirror.
const ARM7_IRQ: u32 = 0x0380_FFFC;
/// Seeding rounds before the runs are taken as they are.
const MAX_ROUNDS: usize = 8;

/// The rules every run uses.
pub const OPTIONS: Options = Options {
    arm_pointers: true,
    scan_words: true,
    switches: true,
    image_calls: true,
    reachable_locals: true,
};

/// Which unit of a program a run analyzes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UnitId {
    /// The static binary of a processor with its code autoload blocks.
    Static(Cpu),
    /// An overlay of a processor, by its id.
    Overlay(Cpu, u32),
}

impl UnitId {
    /// The processor.
    #[must_use]
    pub fn cpu(self) -> Cpu {
        match self {
            UnitId::Static(c) | UnitId::Overlay(c, _) => c,
        }
    }

    /// A short name: `static` (ARM9), `arm7`, `ov<id>` (ARM9) or
    /// `arm7-ov<id>`.
    #[must_use]
    pub fn name(self) -> String {
        match self {
            UnitId::Static(Cpu::Arm9) => "static".to_owned(),
            UnitId::Static(Cpu::Arm7) => "arm7".to_owned(),
            UnitId::Overlay(Cpu::Arm9, id) => format!("ov{id}"),
            UnitId::Overlay(Cpu::Arm7, id) => format!("arm7-ov{id}"),
        }
    }
}

/// One unit: its bytes at their run addresses and its entries.
#[derive(Clone, Debug)]
pub struct Unit {
    /// Which unit.
    pub id: UnitId,
    /// The unit's own bytes: base and decompressed bytes.
    pub base: u32,
    /// The bytes.
    pub bytes: Vec<u8>,
    /// For an overlay, the SHA-256 of its decompressed bytes: an overlay
    /// is known by its id and this hash.
    pub sha256: Option<[u8; 32]>,
    /// The entries: the reset entry of a static binary, the static
    /// initializers of an overlay.
    pub roots: Vec<Loc>,
}

impl Unit {
    /// The address range of the unit's own bytes.
    #[must_use]
    pub fn range(&self) -> (u32, u32) {
        (self.base, self.base.wrapping_add(self.bytes.len() as u32))
    }

    fn holds(&self, addr: u32) -> bool {
        addr.wrapping_sub(self.base) < self.bytes.len() as u32
    }
}

/// The code a processor's runs share: its static binary and code autoload
/// blocks, and where its IRQ handler is installed.
#[derive(Clone, Debug)]
pub struct Processor {
    /// Which processor.
    pub cpu: Cpu,
    /// The static binary and the code autoload blocks: base and bytes.
    pub segments: Vec<(u32, Vec<u8>)>,
    /// The IRQ handler slot.
    pub irq_vector: u32,
}

/// The units of a program, read from a ROM.
#[derive(Clone, Debug)]
pub struct Program {
    /// The ARM9 and the ARM7.
    pub processors: Vec<Processor>,
    /// The static binaries, then the overlays.
    pub units: Vec<Unit>,
}

/// The analysis of one unit.
#[derive(Clone, Debug)]
pub struct UnitReport {
    /// Which unit.
    pub id: UnitId,
    /// For an overlay, the SHA-256 of its decompressed bytes.
    pub sha256: Option<[u8; 32]>,
    /// The unit's own range.
    pub range: (u32, u32),
    /// The seeds its last run took (code addresses other runs call).
    pub seeds: BTreeSet<Loc>,
    /// The runs it took, the first included.
    pub runs: usize,
    /// How long its last run took.
    pub elapsed: Duration,
    /// What the last run found.
    pub report: Report,
    /// The places a suspended thread resumes at that are no function
    /// entry, each with the entry of the function it is an instruction of.
    pub resumes: BTreeMap<Loc, Loc>,
}

/// The Nintendo DS as one run of one unit sees it. The ARM9's instruction
/// TCM mirrors start at address 0, but programs use it at its SDK address
/// in the `0x01` block; the lowest 16 MiB are where null pointers and
/// small integers taken as addresses lead, so they hold no RAM here.
pub struct Nds<'a> {
    cpu: Cpu,
    segments: Vec<(u32, &'a [u8])>,
    irq_vector: u32,
    external: Vec<(u32, u32)>,
    owns: Option<(u32, u32)>,
}

impl Platform for Nds<'_> {
    fn image_writable(&self) -> bool {
        true
    }

    fn profile(&self) -> Profile {
        match self.cpu {
            Cpu::Arm9 => Profile::V5te,
            Cpu::Arm7 => Profile::V4t,
        }
    }

    fn kind(&self, addr: u32) -> Kind {
        if self.image(addr, 1).is_some() {
            return Kind::Image;
        }
        match (self.cpu, addr >> 24) {
            (Cpu::Arm9, 0x01) | (_, 0x02 | 0x03) => Kind::Ram,
            (Cpu::Arm9, 0xFF) if addr >= 0xFFFF_0000 => Kind::Bios,
            (Cpu::Arm7, 0x00) if addr < 0x4000 => Kind::Bios,
            _ => Kind::Other,
        }
    }

    fn image(&self, addr: u32, n: u32) -> Option<&[u8]> {
        self.segments.iter().find_map(|(b, d)| {
            let off = addr.checked_sub(*b)? as usize;
            d.get(off..off.checked_add(n as usize)?)
        })
    }

    fn code_addr(&self, addr: u32) -> bool {
        self.image(addr, 1).is_some()
    }

    fn irq_vector(&self, addr: u32) -> bool {
        addr == self.irq_vector
    }

    fn bios_call(&self, number: u8) -> BiosCall {
        match number {
            0x00 => BiosCall::NoReturn,
            0x0B => BiosCall::CpuSet,
            0x0C => BiosCall::CpuFastSet,
            _ => BiosCall::Returns,
        }
    }

    fn dma(&self, d: &Dma) -> Option<(u32, u32, u32)> {
        let c = u32::from(d.control);
        let (immediate, count, max) = match self.cpu {
            Cpu::Arm9 => ((c >> 11) & 7 == 0, d.count? | (c & 0x1F) << 16, 0x20_0000),
            Cpu::Arm7 => (
                (c >> 12) & 3 == 0,
                d.count?,
                if d.channel == 3 { 0x1_0000 } else { 0x4000 },
            ),
        };
        let src_ctl = (c >> 7) & 3;
        let dst_ctl = (c >> 5) & 3;
        if !immediate || src_ctl != 0 || !(dst_ctl == 0 || dst_ctl == 3) {
            return None;
        }
        let count = if count == 0 { max } else { count };
        let unit = if c & 0x400 != 0 { 4 } else { 2 };
        Some((d.src?, d.dst?, count * unit))
    }

    fn segments(&self) -> Vec<(u32, &[u8])> {
        self.segments.clone()
    }

    fn external(&self, addr: u32) -> bool {
        self.external
            .iter()
            .any(|&(lo, hi)| (lo..hi).contains(&addr))
    }

    fn owns(&self, addr: u32) -> bool {
        self.owns.is_none_or(|(lo, hi)| (lo..hi).contains(&addr))
    }
}

/// Why a program cannot be read.
pub type Error = relift_nds_formats::Error;

impl Program {
    /// The units of `rom`: both static binaries with their autoload
    /// blocks and every overlay of either processor that holds bytes.
    ///
    /// # Errors
    ///
    /// A static binary or an overlay lies outside the image or does not
    /// decompress.
    pub fn read(rom: &Rom<'_>) -> Result<Program, Error> {
        let mut processors = Vec::new();
        let mut units = Vec::new();
        for cpu in [Cpu::Arm9, Cpu::Arm7] {
            let s = match cpu {
                Cpu::Arm9 => rom.arm9()?,
                Cpu::Arm7 => rom.arm7()?,
            };
            processors.push(processor(&s));
            units.push(Unit {
                id: UnitId::Static(cpu),
                base: s.base,
                bytes: s.image.clone(),
                sha256: None,
                roots: vec![Loc::rom(s.entry, false)],
            });
        }
        for cpu in [Cpu::Arm9, Cpu::Arm7] {
            for o in rom.overlays(cpu) {
                let bytes = rom.overlay_data(o)?;
                if bytes.is_empty() {
                    continue;
                }
                let roots = o
                    .static_init_pointers(&bytes)
                    .into_iter()
                    .filter(|p| *p != 0)
                    .map(|p| Loc::rom(p & !1, p & 1 == 1))
                    .collect();
                units.push(Unit {
                    id: UnitId::Overlay(cpu, o.id),
                    base: o.ram,
                    sha256: Some(sha256(&bytes)),
                    bytes,
                    roots,
                });
            }
        }
        Ok(Program { processors, units })
    }

    fn processor(&self, cpu: Cpu) -> Option<&Processor> {
        self.processors.iter().find(|p| p.cpu == cpu)
    }

    /// The platform of unit `i`'s run.
    fn platform(&self, i: usize) -> Option<Nds<'_>> {
        let u = self.units.get(i)?;
        let p = self.processor(u.id.cpu())?;
        let mut segments: Vec<(u32, &[u8])> =
            p.segments.iter().map(|(b, d)| (*b, d.as_slice())).collect();
        let others = self
            .units
            .iter()
            .enumerate()
            .filter(|(j, o)| *j != i && matches!(o.id, UnitId::Overlay(c, _) if c == p.cpu));
        let external = others.map(|(_, o)| o.range()).collect();
        let owns = match u.id {
            UnitId::Static(_) => None,
            UnitId::Overlay(..) => {
                segments.push((u.base, u.bytes.as_slice()));
                Some(u.range())
            }
        };
        Some(Nds {
            cpu: p.cpu,
            segments,
            irq_vector: p.irq_vector,
            external,
            owns,
        })
    }

    /// Runs discovery on every unit, `jobs` runs at a time, and then the
    /// seeding rounds; `progress` hears of each finished run.
    #[must_use]
    pub fn analyze(&self, jobs: usize, progress: &(dyn Fn(&UnitReport) + Sync)) -> Vec<UnitReport> {
        let mut seeds: Vec<BTreeSet<Loc>> = vec![BTreeSet::new(); self.units.len()];
        let mut done: Vec<Option<UnitReport>> = vec![None; self.units.len()];
        let mut todo: Vec<usize> = (0..self.units.len()).collect();
        for _ in 0..MAX_ROUNDS {
            if todo.is_empty() {
                break;
            }
            let runs: Vec<usize> = done
                .iter()
                .map(|d| d.as_ref().map_or(1, |d| d.runs + 1))
                .collect();
            for (i, r) in self.run_all(&todo, &seeds, &runs, jobs, progress) {
                done[i] = Some(r);
            }
            let mut grown = Vec::new();
            for (i, u) in self.units.iter().enumerate() {
                let UnitId::Overlay(cpu, _) = u.id else {
                    continue;
                };
                let calls = done
                    .iter()
                    .enumerate()
                    .filter(|(j, r)| *j != i && r.as_ref().is_some_and(|r| r.id.cpu() == cpu))
                    .flat_map(|(_, r)| r.iter().flat_map(|r| r.report.outside.iter()))
                    .filter(|l| u.holds(l.addr));
                let before = seeds[i].len();
                seeds[i].extend(calls);
                if seeds[i].len() > before {
                    grown.push(i);
                }
            }
            todo = grown;
        }
        let mut done: Vec<UnitReport> = done.into_iter().flatten().collect();
        for cpu in [Cpu::Arm9, Cpu::Arm7] {
            let idx: Vec<usize> = (0..done.len())
                .filter(|&i| done[i].id.cpu() == cpu)
                .collect();
            let reports: Vec<&Report> = idx.iter().map(|&i| &done[i].report).collect();
            let ids: Vec<UnitId> = idx.iter().map(|&i| done[i].id).collect();
            let found = resume::resumes(&reports, &|k, a| self.word(ids[k], a));
            for (i, r) in idx.into_iter().zip(found) {
                done[i].resumes = r;
            }
        }
        done
    }

    /// The ARM word at `addr` in the code of the run of the unit `id`: its
    /// own bytes, then its processor's static binary and autoload blocks.
    fn word(&self, id: UnitId, addr: u32) -> Option<u32> {
        let own = self
            .units
            .iter()
            .filter(|u| u.id == id && matches!(id, UnitId::Overlay(..)))
            .map(|u| (u.base, u.bytes.as_slice()));
        let segments = self
            .processor(id.cpu())?
            .segments
            .iter()
            .map(|(b, d)| (*b, d.as_slice()));
        own.chain(segments).find_map(|(b, d)| {
            let off = addr.checked_sub(b)? as usize;
            let w = d.get(off..off.checked_add(4)?)?;
            Some(u32::from_le_bytes([w[0], w[1], w[2], w[3]]))
        })
    }

    fn run_all(
        &self,
        todo: &[usize],
        seeds: &[BTreeSet<Loc>],
        runs: &[usize],
        jobs: usize,
        progress: &(dyn Fn(&UnitReport) + Sync),
    ) -> Vec<(usize, UnitReport)> {
        let next = AtomicUsize::new(0);
        let out = Mutex::new(Vec::new());
        std::thread::scope(|s| {
            for _ in 0..jobs.clamp(1, todo.len().max(1)) {
                s.spawn(|| {
                    while let Some(&i) = todo.get(next.fetch_add(1, Ordering::Relaxed)) {
                        let r = self.run(i, &seeds[i], runs[i]);
                        progress(&r);
                        if let Ok(mut o) = out.lock() {
                            o.push((i, r));
                        }
                    }
                });
            }
        });
        let mut out = out.into_inner().unwrap_or_default();
        out.sort_by_key(|(i, _)| *i);
        out
    }

    fn run(&self, i: usize, seeds: &BTreeSet<Loc>, runs: usize) -> UnitReport {
        let u = &self.units[i];
        let start = Instant::now();
        let report = self.platform(i).map_or_else(Report::default, |p| {
            let config = Config {
                options: OPTIONS,
                roots: u.roots.clone(),
                seeds: seeds.iter().copied().collect(),
                ..Config::default()
            };
            discover(&p, &config)
        });
        UnitReport {
            id: u.id,
            sha256: u.sha256,
            range: u.range(),
            seeds: seeds.clone(),
            runs,
            elapsed: start.elapsed(),
            report,
            resumes: BTreeMap::new(),
        }
    }
}

/// The code a processor's runs share, from its static binary: the binary
/// itself, its autoload blocks that hold code (all but the ARM9's data
/// TCM block, the SDK's second one when it is in main RAM) and the IRQ
/// slot.
fn processor(s: &StaticUnit<'_>) -> Processor {
    let mut segments = vec![(s.base, s.image.clone())];
    let mut dtcm = DTCM;
    for (i, a) in s.autoloads.iter().enumerate() {
        if a.size == 0 {
            continue;
        }
        if s.cpu == Cpu::Arm9 && i == 1 && a.addr >> 24 == 0x02 {
            dtcm = a.addr;
            continue;
        }
        segments.push((a.addr, s.autoload_data(a).to_vec()));
    }
    let irq_vector = match s.cpu {
        Cpu::Arm9 => dtcm + DTCM_IRQ,
        Cpu::Arm7 => ARM7_IRQ,
    };
    Processor {
        cpu: s.cpu,
        segments,
        irq_vector,
    }
}
