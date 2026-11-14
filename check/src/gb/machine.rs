//! Building a rustyboi machine in a chosen state.
//!
//! rustyboi-core keeps its CPU private, so registers are set through the
//! savestate: the CPU register file is the first thing bincode writes, and
//! the layout is checked against the live registers before it is patched.

use rustyboi_core_lib::cartridge::Cartridge;
use rustyboi_core_lib::gb::GB;
pub use rustyboi_core_lib::gb::Hardware;

use super::{Error, Regs, err};

pub const ROM_SIZE: usize = 0x8000;
const HEADER_CGB: usize = 0x143;
const HEADER_TYPE: usize = 0x147;
const HEADER_ROM: usize = 0x148;
const HEADER_RAM: usize = 0x149;

pub fn parse_hardware(name: &str) -> Option<Hardware> {
    Some(match name.to_ascii_lowercase().as_str() {
        "dmg" => Hardware::DMG,
        "dmg0" => Hardware::DMG0,
        "mgb" => Hardware::MGB,
        "sgb" => Hardware::SGB,
        "sgb2" => Hardware::SGB2,
        "cgb0" => Hardware::CGB0,
        "cgbb" => Hardware::CGBB,
        "cgb" | "cgbc" => Hardware::CGB,
        "cgbe" => Hardware::CGBE,
        "agb" => Hardware::AGB,
        _ => return None,
    })
}

pub fn hardware_name(hw: Hardware) -> &'static str {
    match hw {
        Hardware::DMG => "dmg",
        Hardware::DMG0 => "dmg0",
        Hardware::MGB => "mgb",
        Hardware::SGB => "sgb",
        Hardware::SGB2 => "sgb2",
        Hardware::CGB0 => "cgb0",
        Hardware::CGBB => "cgbb",
        Hardware::CGB => "cgb",
        Hardware::CGBE => "cgbe",
        Hardware::AGB => "agb",
    }
}

pub fn is_cgb(hw: Hardware) -> bool {
    matches!(
        hw,
        Hardware::CGB0 | Hardware::CGBB | Hardware::CGB | Hardware::CGBE | Hardware::AGB
    )
}

/// A 32 KiB ROM-only cartridge with 8 KiB of RAM around `code`.
///
/// The oracle owns the header bytes for the CGB flag, the cartridge type and
/// the ROM and RAM sizes; `code` supplies everything else.
pub fn flat_rom(hw: Hardware, code: &[u8]) -> Vec<u8> {
    let mut rom = vec![0u8; ROM_SIZE];
    let n = code.len().min(ROM_SIZE);
    rom[..n].copy_from_slice(&code[..n]);
    rom[HEADER_CGB] = if is_cgb(hw) { 0x80 } else { 0x00 };
    rom[HEADER_TYPE] = 0x08;
    rom[HEADER_ROM] = 0x00;
    rom[HEADER_RAM] = 0x02;
    rom
}

/// A machine with `rom` inserted and the boot ROM skipped.
pub fn boot(hw: Hardware, rom: &[u8]) -> Result<GB, Error> {
    let cart = Cartridge::from_bytes(rom).map_err(|e| Error(format!("cartridge: {e}")))?;
    let mut gb = GB::new(hw);
    gb.insert(cart);
    gb.skip_bios();
    Ok(gb)
}

const REG_BYTES: usize = 13;

fn encode(r: &rustyboi_core_lib::cpu::registers::Registers) -> [u8; REG_BYTES] {
    let mut b = [0u8; REG_BYTES];
    b[..8].copy_from_slice(&[r.a, r.f, r.b, r.c, r.d, r.e, r.h, r.l]);
    b[8..10].copy_from_slice(&r.pc.to_le_bytes());
    b[10..12].copy_from_slice(&r.sp.to_le_bytes());
    b[12] = r.ime as u8;
    b
}

/// Replace the CPU registers and interrupt master enable.
pub fn set_regs(gb: &mut GB, regs: &Regs) -> Result<(), Error> {
    let rom = gb
        .detach_rom_bytes()
        .ok_or_else(|| Error("no cartridge in the machine".into()))?;
    let mut bytes = gb
        .to_state_bytes()
        .map_err(|e| Error(format!("savestate: {e}")))?;
    let live = encode(gb.get_cpu_registers());
    if bytes.len() < REG_BYTES || bytes[..REG_BYTES] != live {
        return err("rustyboi savestate layout changed: the register file is not first");
    }
    let new = [
        regs.a,
        regs.f & 0xF0,
        regs.b,
        regs.c,
        regs.d,
        regs.e,
        regs.h,
        regs.l,
    ];
    bytes[..8].copy_from_slice(&new);
    bytes[8..10].copy_from_slice(&regs.pc.to_le_bytes());
    bytes[10..12].copy_from_slice(&regs.sp.to_le_bytes());
    bytes[12] = regs.ime as u8;
    let mut fresh =
        GB::from_state_bytes(&bytes).map_err(|e| Error(format!("savestate load: {e}")))?;
    if !fresh.reattach_rom(&rom) {
        return err("savestate carried no cartridge");
    }
    *gb = fresh;
    Ok(())
}

pub fn regs_of(gb: &GB) -> Regs {
    let r = gb.get_cpu_registers();
    Regs {
        a: r.a,
        f: r.f,
        b: r.b,
        c: r.c,
        d: r.d,
        e: r.e,
        h: r.h,
        l: r.l,
        sp: r.sp,
        pc: r.pc,
        ime: r.ime,
    }
}
