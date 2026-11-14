//! ROM mode: a whole cartridge on a chosen hardware revision.

use rustyboi_core_lib::cartridge::Cartridge;
use rustyboi_core_lib::gb::GB;

use super::machine::{self, Hardware};
use super::{Error, Regs};

/// T-cycles in one frame at normal speed.
pub const FRAME_CYCLES: u64 = 70_224;

/// A condition that ends the run.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Stop {
    /// After this many T-cycles.
    Cycles(u64),
    /// After this many frames of [`FRAME_CYCLES`] T-cycles.
    Frames(u64),
    /// Before the instruction at this address executes.
    Pc(u16),
    /// After the byte at `addr` reads `value`.
    Mem { addr: u16, value: u8 },
}

/// What to run. The run also ends after [`DEFAULT_FRAMES`] frames when no
/// stop gives a cycle or frame budget.
#[derive(Clone, Debug)]
pub struct RomSpec {
    pub hardware: Hardware,
    pub rom: Vec<u8>,
    /// A boot ROM to run; the boot ROM is skipped when this is `None`.
    pub boot_rom: Option<Vec<u8>>,
    /// The run ends at the first of these.
    pub stops: Vec<Stop>,
    /// Memory ranges to read back as `(address, length)`.
    pub dump: Vec<(u16, u16)>,
}

/// Why the run ended.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StopReason(pub Stop);

/// The machine after the run.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RomResult {
    pub regs: Regs,
    pub reason: StopReason,
    /// T-cycles stepped, as rustyboi counts them.
    pub cycles: u64,
    /// Instructions stepped.
    pub steps: u64,
    /// rustyboi's master cycle counter.
    pub master_cc: u64,
    /// FNV-1a 64 of the presented RGB frame.
    pub frame_hash: u64,
    pub dump: Vec<(u16, Vec<u8>)>,
}

pub fn fnv1a(bytes: &[u8]) -> u64 {
    let mut h = 0xcbf2_9ce4_8422_2325u64;
    for b in bytes {
        h ^= u64::from(*b);
        h = h.wrapping_mul(0x0000_0100_0000_01b3);
    }
    h
}

pub const DEFAULT_FRAMES: u64 = 600;

fn limit(stops: &[Stop]) -> u64 {
    stops
        .iter()
        .filter_map(|s| match s {
            Stop::Cycles(n) => Some(*n),
            Stop::Frames(n) => Some(n * FRAME_CYCLES),
            _ => None,
        })
        .min()
        .unwrap_or(DEFAULT_FRAMES * FRAME_CYCLES)
}

fn hit(gb: &GB, stops: &[Stop], before_step: bool) -> Option<Stop> {
    let pc = gb.get_cpu_registers().pc;
    stops.iter().copied().find(|s| match *s {
        Stop::Pc(p) => before_step && p == pc,
        Stop::Mem { addr, value } => !before_step && gb.read_memory(addr) == value,
        _ => false,
    })
}

/// Run a ROM.
pub fn run_rom(spec: &RomSpec) -> Result<RomResult, Error> {
    let cart = Cartridge::from_bytes(&spec.rom).map_err(|e| Error(format!("cartridge: {e}")))?;
    let mut gb = GB::new(spec.hardware);
    gb.insert(cart);
    match &spec.boot_rom {
        Some(bytes) => {
            gb.load_bios_bytes(bytes)
                .map_err(|e| Error(format!("boot rom: {e}")))?;
            gb.run_boot_rom();
        }
        None => gb.skip_bios(),
    }
    let cap = limit(&spec.stops);
    if spec.stops.is_empty() {
        return Err(Error("no stop condition".into()));
    }
    let (mut cycles, mut steps) = (0u64, 0u64);
    let reason = loop {
        if let Some(s) = hit(&gb, &spec.stops, true) {
            break s;
        }
        if cycles >= cap {
            break spec
                .stops
                .iter()
                .copied()
                .find(|s| {
                    matches!(s, Stop::Cycles(n) if *n == cap)
                        || matches!(s, Stop::Frames(n) if n * FRAME_CYCLES == cap)
                })
                .unwrap_or(Stop::Cycles(cap));
        }
        let (breakpoint, n) = gb.step_instruction(false);
        if breakpoint {
            return Err(Error("a breakpoint stopped the run".into()));
        }
        cycles += u64::from(n);
        steps += 1;
        if let Some(s) = hit(&gb, &spec.stops, false) {
            break s;
        }
    };
    let regs = machine::regs_of(&gb);
    let master_cc = gb.master_cc();
    let frame = gb.get_current_frame();
    let frame_hash = fnv1a(frame.rgb());
    let dump = spec
        .dump
        .iter()
        .map(|&(addr, len)| {
            let bytes = (0..u32::from(len))
                .map(|i| gb.read_memory(addr.wrapping_add(i as u16)))
                .collect();
            (addr, bytes)
        })
        .collect();
    Ok(RomResult {
        regs,
        reason: StopReason(reason),
        cycles,
        steps,
        master_cc,
        frame_hash,
        dump,
    })
}
