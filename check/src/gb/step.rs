//! Single-step mode: one instruction from a given state.
//!
//! rustyboi-core has no public hook that logs bus writes, so the writes are
//! found by diffing every writable region around the step. A run is repeated
//! with a complemented fill so a write of the fill value is still seen. The
//! M-cycle of a write and the order of writes are not available; `mcycle` is
//! always `None` and the writes are sorted by address.

use rustyboi_core_lib::gb::GB;

use super::machine::{self, Hardware, flat_rom};
use super::{Error, Regs, err};

/// Bytes at an address of the 64 KiB address space.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Segment {
    pub addr: u16,
    pub bytes: Vec<u8>,
}

/// What to run.
#[derive(Clone, Debug)]
pub struct StepSpec {
    pub hardware: Hardware,
    pub regs: Regs,
    /// The memory image. `0000-7FFF` becomes the cartridge, `8000-9FFF` the
    /// first VRAM bank, `A000-BFFF` cartridge RAM, `C000-DFFF` work RAM,
    /// `FE00-FE9F` OAM, `FF00-FF7F` and `FFFF` hardware registers (written
    /// through the bus) and `FF80-FFFE` high RAM. The cartridge header bytes
    /// `0143` and `0147-0149` belong to the oracle.
    pub image: Vec<Segment>,
    /// The value of every RAM byte the image does not set.
    pub fill: u8,
}

/// A byte an instruction changed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MemWrite {
    pub addr: u16,
    pub value: u8,
    /// The M-cycle of the write; `None` until rustyboi-core can log it.
    pub mcycle: Option<u8>,
}

/// What one instruction did.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StepResult {
    pub regs: Regs,
    /// T-cycles as rustyboi counts them (normal speed: four per M-cycle).
    pub cycles: u32,
    pub writes: Vec<MemWrite>,
    /// `FF00-FF7F` and `FFFF` read back after the step.
    pub io: Vec<(u16, u8)>,
}

const VRAM: std::ops::Range<u32> = 0x8000..0xA000;
const SRAM: std::ops::Range<u32> = 0xA000..0xC000;
const WRAM: std::ops::Range<u32> = 0xC000..0xE000;
const HRAM: std::ops::Range<u32> = 0xFF80..0xFFFF;

const RAM_RANGES: [std::ops::Range<u32>; 4] = [VRAM, SRAM, WRAM, HRAM];

fn is_io(addr: u32) -> bool {
    (0xFF00..0xFF80).contains(&addr) || addr == 0xFFFF
}

fn fill_ram(gb: &mut GB, fill: u8) {
    gb.vram_mut().fill(fill);
    gb.wram_bank0_mut().fill(fill);
    gb.wram_bank1_mut().fill(fill);
    gb.hram_mut().fill(fill);
    for a in SRAM {
        gb.write_memory(a as u16, fill);
    }
}

fn put(gb: &mut GB, addr: u32, v: u8) -> Result<(), Error> {
    match addr {
        a if VRAM.contains(&a) => gb.vram_mut()[(a - VRAM.start) as usize] = v,
        a if WRAM.contains(&a) && a < 0xD000 => gb.wram_bank0_mut()[(a - 0xC000) as usize] = v,
        a if WRAM.contains(&a) => gb.wram_bank1_mut()[(a - 0xD000) as usize] = v,
        a if HRAM.contains(&a) => gb.hram_mut()[(a - HRAM.start) as usize] = v,
        a if SRAM.contains(&a) || (0xFE00..0xFEA0).contains(&a) || is_io(a) => {
            gb.write_memory(a as u16, v)
        }
        a => return err(format!("address {a:04x} is not settable")),
    }
    Ok(())
}

fn ram_snapshot(gb: &mut GB) -> Vec<u8> {
    let mut m = vec![0u8; 0x10000];
    m[0x8000..0xA000].copy_from_slice(&gb.vram_mut()[..0x2000]);
    m[0xC000..0xD000].copy_from_slice(gb.wram_bank0_mut());
    m[0xD000..0xE000].copy_from_slice(gb.wram_bank1_mut());
    m[0xFF80..0xFFFF].copy_from_slice(gb.hram_mut());
    for a in SRAM {
        m[a as usize] = gb.read_memory(a as u16);
    }
    m
}

/// How many instructions to run, and an interrupt state to install after
/// them.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Sequence {
    /// Instruction steps to run.
    pub steps: u32,
    /// `(IE, IF)` written after the steps; when the interrupt master enable
    /// is then set and an enabled interrupt is requested, one more step runs
    /// (the interrupt entry).
    pub late: Option<(u8, u8)>,
}

impl Sequence {
    /// One instruction.
    pub const ONE: Sequence = Sequence {
        steps: 1,
        late: None,
    };
}

struct Run {
    regs: Regs,
    cycles: u32,
    before: Vec<u8>,
    after: Vec<u8>,
    io: Vec<(u16, u8)>,
}

fn run_once(spec: &StepSpec, fill: u8, seq: Sequence) -> Result<Run, Error> {
    let mut code = vec![0u8; machine::ROM_SIZE];
    for s in &spec.image {
        for (i, b) in s.bytes.iter().enumerate() {
            let a = s.addr as usize + i;
            if a < machine::ROM_SIZE {
                code[a] = *b;
            }
        }
    }
    let rom = flat_rom(spec.hardware, &code);
    let mut gb = machine::boot(spec.hardware, &rom)?;
    machine::set_regs(&mut gb, &spec.regs)?;
    fill_ram(&mut gb, fill);
    for s in &spec.image {
        for (i, b) in s.bytes.iter().enumerate() {
            let a = s.addr as u32 + i as u32;
            if a >= machine::ROM_SIZE as u32 {
                put(&mut gb, a, *b)?;
            }
        }
    }
    let before = ram_snapshot(&mut gb);
    let mut cycles = 0;
    let mut run_step = |gb: &mut GB| -> Result<(), Error> {
        let (stopped, c) = gb.step_instruction(false);
        if stopped {
            return err("a breakpoint stopped the step");
        }
        cycles += c;
        Ok(())
    };
    for _ in 0..seq.steps {
        run_step(&mut gb)?;
    }
    if let Some((ie, iff)) = seq.late {
        gb.write_memory(0xFFFF, ie);
        gb.write_memory(0xFF0F, iff);
        if machine::regs_of(&gb).ime && ie & iff & 0x1F != 0 {
            run_step(&mut gb)?;
        }
    }
    let after = ram_snapshot(&mut gb);
    let io = (0..=0xFFFFu32)
        .filter(|a| is_io(*a))
        .map(|a| (a as u16, gb.read_memory(a as u16)))
        .collect();
    Ok(Run {
        regs: machine::regs_of(&gb),
        cycles,
        before,
        after,
        io,
    })
}

/// Run one instruction.
pub fn run_step(spec: &StepSpec) -> Result<StepResult, Error> {
    run_sequence(spec, Sequence::ONE)
}

/// Run a [`Sequence`].
pub fn run_sequence(spec: &StepSpec, seq: Sequence) -> Result<StepResult, Error> {
    let a = run_once(spec, spec.fill, seq)?;
    let b = run_once(spec, !spec.fill, seq)?;
    let mut writes = Vec::new();
    for range in RAM_RANGES {
        for addr in range {
            let i = addr as usize;
            let changed = a.after[i] != a.before[i];
            let wrote_fill = b.after[i] != b.before[i] && b.after[i] == a.before[i];
            if changed || wrote_fill {
                writes.push(MemWrite {
                    addr: addr as u16,
                    value: a.after[i],
                    mcycle: None,
                });
            }
        }
    }
    Ok(StepResult {
        regs: a.regs,
        cycles: a.cycles,
        writes,
        io: a.io,
    })
}
