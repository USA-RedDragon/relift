use relift_gb_devices::{Rlgbd, Verdict};
use std::io::Write;
use std::path::PathBuf;

pub const JR_SELF: [u8; 2] = [0x18, 0xFE];

/// A cartridge image: `code` at 0x150 behind a jump at the entry point.
pub fn rom(cart_type: u8, size_code: u8, ram_code: u8, cgb: u8, code: &[u8]) -> Vec<u8> {
    let banks = 2usize << size_code;
    let mut r = vec![0u8; banks * 0x4000];
    r[0x143] = cgb;
    r[0x147] = cart_type;
    r[0x148] = size_code;
    r[0x149] = ram_code;
    r[0x100..0x104].copy_from_slice(&[0x00, 0xC3, 0x50, 0x01]);
    r[0x150..0x150 + code.len()].copy_from_slice(code);
    r
}

pub fn plain(cgb: u8, code: &[u8]) -> Vec<u8> {
    rom(0, 0, 0, cgb, code)
}

pub fn then_spin(code: &[u8]) -> Vec<u8> {
    let mut v = code.to_vec();
    v.extend_from_slice(&JR_SELF);
    v
}

/// `ld a,n ; ldh (reg),a` for each byte.
pub fn ldh_all(reg: u8, bytes: &[u8]) -> Vec<u8> {
    bytes.iter().flat_map(|&b| [0x3E, b, 0xE0, reg]).collect()
}

/// Writes each byte to SB and pulses SC, so every byte is one start edge.
pub fn serial_sc_edge(text: &str) -> Vec<u8> {
    text.bytes()
        .flat_map(|b| {
            [
                0x3E, b, 0xE0, 0x01, 0x3E, 0x81, 0xE0, 0x02, 0xAF, 0xE0, 0x02,
            ]
        })
        .collect()
}

/// Reaches the next vblank, blackens tile 0 and clears the boot logo's map rows, lets three frames draw (the panel
/// shows nothing before two have), then `ld b,b`.
pub fn black_screen_then_marker() -> Vec<u8> {
    let mut v = vec![
        0xF0, 0x44, 0xFE, 0x90, 0x38, 0xFA, 0x21, 0x00, 0x80, 0x0E, 0x10, 0x3E, 0xFF, 0x22, 0x0D,
        0x20, 0xFA, 0x21, 0x00, 0x99, 0x0E, 0x40, 0xAF, 0x22, 0x0D, 0x20, 0xFB,
    ];
    for _ in 0..3 {
        v.extend_from_slice(&[
            0xF0, 0x44, 0xFE, 0x90, 0x30, 0xFA, 0xF0, 0x44, 0xFE, 0x96, 0x38, 0xFA,
        ]);
    }
    v.push(0x40);
    v.extend_from_slice(&JR_SELF);
    v
}

/// Runs the machine on its own CPU, polling the case at each boundary.
pub fn drive(m: &mut Rlgbd, max_steps: u64) -> Verdict {
    for _ in 0..max_steps {
        let pc = m.regs().pc;
        let mut v = m.case_poll(pc);
        if v == Verdict::NeedRegs {
            let regs = m.regs();
            m.case_regs(&regs);
            v = m.case_poll(pc);
        }
        if v != Verdict::Continue {
            return v;
        }
        if let Some(op) = m.begin() {
            m.exec_native(op);
        }
    }
    Verdict::Continue
}

pub fn detail(m: &Rlgbd) -> String {
    let d = m.case_detail();
    String::from_utf8_lossy(&d[..d.len() - 1]).into_owned()
}

pub fn scratch(name: &str) -> PathBuf {
    std::env::temp_dir().join(format!("relift-gb-devices-{}-{name}", std::process::id()))
}

pub fn write_new(path: &PathBuf, bytes: &[u8]) {
    match std::fs::remove_file(path) {
        Err(e) if e.kind() != std::io::ErrorKind::NotFound => panic!("{e}"),
        _ => {}
    }
    let mut f = std::fs::File::create_new(path).unwrap();
    f.write_all(bytes).unwrap();
}
