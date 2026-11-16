//! Discovery on a synthetic cartridge whose ARM9 static binary calls into
//! an overlay.

use relift_nds_analyze::{Loc, Program, UnitId, UnitReport};
use relift_nds_formats::checksum::sha256;
use relift_nds_formats::synth::{self, OverlaySpec, Spec, StaticSpec};
use relift_nds_formats::{Cpu, Rom};

const RAM: u32 = 0x0200_0000;
const ENTRY: u32 = RAM + 0x400;
const OVERLAY: u32 = 0x0210_0000;

const A_PUSH_R4_LR: u32 = 0xE92D_4010;
const A_POP_R4_PC: u32 = 0xE8BD_8010;
const A_MOV_R0_1: u32 = 0xE3A0_0001;
const A_BX_LR: u32 = 0xE12F_FF1E;

fn a_bl(at: u32, to: u32) -> u32 {
    0xEB00_0000 | (to.wrapping_sub(at + 8) >> 2 & 0x00FF_FFFF)
}

fn words(w: &[u32]) -> Vec<u8> {
    w.iter().flat_map(|w| w.to_le_bytes()).collect()
}

fn overlay() -> Vec<u8> {
    let mut d = vec![0; 0x300];
    d[..12].copy_from_slice(&words(&[A_PUSH_R4_LR, A_MOV_R0_1, A_POP_R4_PC]));
    d[0x100..0x104].copy_from_slice(&words(&[A_BX_LR]));
    d[0x200..0x204].copy_from_slice(&words(&[OVERLAY + 0x100]));
    d
}

fn cartridge() -> Vec<u8> {
    let arm9 = StaticSpec {
        entry: ENTRY,
        code_len: 0x1000,
        ..StaticSpec::default()
    };
    let mut rom = synth::rom(&Spec {
        arm9,
        overlays9: vec![OverlaySpec {
            id: 4,
            ram: OVERLAY,
            data: overlay(),
            bss: 0,
            compress: true,
            authenticated: false,
            static_init: (OVERLAY + 0x200, OVERLAY + 0x204),
        }],
        ..Spec::default()
    });
    let at = Rom::parse(&rom).expect("parses").header.arm9.rom_offset as usize + 0x400;
    let code = words(&[
        A_PUSH_R4_LR,
        a_bl(ENTRY + 4, OVERLAY),
        A_MOV_R0_1,
        A_POP_R4_PC,
    ]);
    rom[at..at + code.len()].copy_from_slice(&code);
    synth::fix_crcs(&mut rom);
    rom
}

fn unit(rs: &[UnitReport], id: UnitId) -> &UnitReport {
    rs.iter().find(|r| r.id == id).expect("unit analyzed")
}

fn found(r: &UnitReport, addr: u32) -> bool {
    r.report.instructions().contains_key(&Loc::rom(addr, false))
}

#[test]
fn a_call_into_an_overlay_seeds_its_run() {
    let bytes = cartridge();
    let rom = Rom::parse(&bytes).expect("parses");
    let program = Program::read(&rom).expect("units read");
    let ids: Vec<UnitId> = program.units.iter().map(|u| u.id).collect();
    assert_eq!(
        ids,
        [
            UnitId::Static(Cpu::Arm9),
            UnitId::Static(Cpu::Arm7),
            UnitId::Overlay(Cpu::Arm9, 4)
        ]
    );
    let rs = program.analyze(2, &|_| {});
    let st = unit(&rs, UnitId::Static(Cpu::Arm9));
    assert!(st.report.outside.contains(&Loc::rom(OVERLAY, false)));
    assert!(found(st, ENTRY + 8), "a call into an overlay returns");
    let ov = unit(&rs, UnitId::Overlay(Cpu::Arm9, 4));
    assert_eq!(ov.sha256, Some(sha256(&overlay())));
    assert!(found(ov, OVERLAY + 0x100), "the static initializer");
    assert!(found(ov, OVERLAY), "the call from the static binary");
    assert_eq!(ov.runs, 2);
    assert!(ov.seeds.contains(&Loc::rom(OVERLAY, false)));
}

#[test]
fn the_irq_slot_is_at_the_end_of_the_data_tcm() {
    let bytes = cartridge();
    let rom = Rom::parse(&bytes).expect("parses");
    let program = Program::read(&rom).expect("units read");
    let irq: Vec<(Cpu, u32)> = program
        .processors
        .iter()
        .map(|p| (p.cpu, p.irq_vector))
        .collect();
    assert_eq!(irq, [(Cpu::Arm9, 0x027E_3FFC), (Cpu::Arm7, 0x0380_FFFC)]);
    let arm9 = &program.processors[0];
    assert!(
        arm9.segments.iter().any(|(b, _)| *b == 0x01FF_8000),
        "the ITCM block is code"
    );
    assert!(
        !arm9.segments.iter().any(|(b, _)| *b == 0x027E_0000),
        "the data TCM block is not"
    );
}
