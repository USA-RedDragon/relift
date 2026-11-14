use super::cli::{dispatch, parse_rom, parse_step};
use super::{Hardware, Regs, RomSpec, Segment, StepSpec, Stop, run_rom, run_step};

fn spec(hw: Hardware, regs: Regs, code: &[u8]) -> StepSpec {
    StepSpec {
        hardware: hw,
        regs,
        image: vec![Segment {
            addr: regs.pc,
            bytes: code.to_vec(),
        }],
        fill: 0,
    }
}

fn regs() -> Regs {
    Regs {
        pc: 0x0100,
        sp: 0xFFFE,
        ..Regs::default()
    }
}

#[test]
fn inc_a_sets_zero_and_half_carry() {
    let r = Regs { a: 0xFF, ..regs() };
    let out = run_step(&spec(Hardware::DMG, r, &[0x3C])).unwrap();
    assert_eq!(out.regs.a, 0);
    assert_eq!(out.regs.flags(), [true, false, true, false]);
    assert_eq!(out.regs.pc, 0x0101);
    assert_eq!(out.cycles, 4);
    assert!(out.writes.is_empty());
}

#[test]
fn flag_low_nibble_is_masked() {
    let r = Regs { f: 0xFF, ..regs() };
    let out = run_step(&spec(Hardware::CGB, r, &[0x00])).unwrap();
    assert_eq!(out.regs.f, 0xF0);
}

#[test]
fn add_immediate_sets_carry() {
    let r = Regs { a: 0xF0, ..regs() };
    let out = run_step(&spec(Hardware::CGBE, r, &[0xC6, 0x20])).unwrap();
    assert_eq!(out.regs.a, 0x10);
    assert_eq!(out.regs.flags(), [false, false, false, true]);
    assert_eq!(out.cycles, 8);
    assert_eq!(out.regs.pc, 0x0102);
}

#[test]
fn store_through_hl_is_a_write() {
    let r = Regs {
        a: 0x5A,
        h: 0xC1,
        l: 0x23,
        ..regs()
    };
    let out = run_step(&spec(Hardware::DMG, r, &[0x77])).unwrap();
    assert_eq!(out.cycles, 8);
    assert_eq!(out.writes.len(), 1);
    assert_eq!((out.writes[0].addr, out.writes[0].value), (0xC123, 0x5A));
}

#[test]
fn a_write_of_the_fill_value_is_still_seen() {
    let r = Regs {
        h: 0xC1,
        l: 0x23,
        ..regs()
    };
    let out = run_step(&spec(Hardware::DMG, r, &[0x77])).unwrap();
    assert_eq!(out.writes.len(), 1);
    assert_eq!((out.writes[0].addr, out.writes[0].value), (0xC123, 0));
}

#[test]
fn push_writes_both_bytes_and_moves_sp() {
    let r = Regs {
        b: 0x12,
        c: 0x34,
        sp: 0xD000,
        ..regs()
    };
    let out = run_step(&spec(Hardware::CGB, r, &[0xC5])).unwrap();
    assert_eq!(out.regs.sp, 0xCFFE);
    assert_eq!(out.cycles, 16);
    let mut w: Vec<_> = out.writes.iter().map(|w| (w.addr, w.value)).collect();
    w.sort();
    assert_eq!(w, [(0xCFFE, 0x34), (0xCFFF, 0x12)]);
}

#[test]
fn jr_costs_more_when_taken() {
    let taken = run_step(&spec(Hardware::DMG, regs(), &[0x20, 0x05])).unwrap();
    assert_eq!((taken.regs.pc, taken.cycles), (0x0107, 12));
    let r = Regs { f: 0x80, ..regs() };
    let not = run_step(&spec(Hardware::DMG, r, &[0x20, 0x05])).unwrap();
    assert_eq!((not.regs.pc, not.cycles), (0x0102, 8));
}

#[test]
fn memory_image_is_read_by_the_instruction() {
    let r = Regs {
        h: 0xC0,
        l: 0x10,
        ..regs()
    };
    let mut s = spec(Hardware::CGB, r, &[0x7E]);
    s.image.push(Segment {
        addr: 0xC010,
        bytes: vec![0x99],
    });
    assert_eq!(run_step(&s).unwrap().regs.a, 0x99);
    let mut s = spec(Hardware::DMG, r, &[0x7E]);
    s.image.push(Segment {
        addr: 0xFF80,
        bytes: vec![0x01],
    });
    s.image.push(Segment {
        addr: 0xC010,
        bytes: vec![0x77],
    });
    assert_eq!(run_step(&s).unwrap().regs.a, 0x77);
}

#[test]
fn ime_is_set_and_cleared() {
    let on = Regs {
        ime: true,
        ..regs()
    };
    assert!(
        run_step(&spec(Hardware::DMG, on, &[0x00]))
            .unwrap()
            .regs
            .ime
    );
    assert!(
        !run_step(&spec(Hardware::DMG, on, &[0xF3]))
            .unwrap()
            .regs
            .ime
    );
    assert!(
        !run_step(&spec(Hardware::DMG, regs(), &[0x00]))
            .unwrap()
            .regs
            .ime
    );
}

#[test]
fn step_cli_parses_a_state() {
    let args: Vec<String> = "--hw cgbe --code 3C --a ff --f 10 --pc 0200 --mem c000=0102 --ime 1"
        .split(' ')
        .map(String::from)
        .collect();
    let s = parse_step(&args).unwrap();
    assert_eq!(s.hardware, Hardware::CGBE);
    assert_eq!(
        (s.regs.a, s.regs.f, s.regs.pc, s.regs.ime),
        (0xFF, 0x10, 0x200, true)
    );
    assert_eq!(
        s.image[0],
        Segment {
            addr: 0x200,
            bytes: vec![0x3C]
        }
    );
    assert_eq!(
        s.image[1],
        Segment {
            addr: 0xC000,
            bytes: vec![1, 2]
        }
    );
    let bad: Vec<String> = ["--hw", "nes", "--code", "00"].map(String::from).to_vec();
    assert!(parse_step(&bad).is_err());
}

fn counter_rom() -> Vec<u8> {
    let mut rom = vec![0u8; 0x8000];
    rom[0x100..0x103].copy_from_slice(&[0xC3, 0x50, 0x01]);
    rom[0x150..0x155].copy_from_slice(&[0x3E, 0x42, 0xE0, 0x80, 0x18]);
    rom[0x155] = 0xFE;
    rom
}

fn rom_spec(hw: Hardware, rom: Vec<u8>, stops: Vec<Stop>) -> RomSpec {
    RomSpec {
        hardware: hw,
        rom,
        boot_rom: None,
        stops,
        dump: vec![(0xFF80, 1)],
    }
}

#[test]
fn rom_runs_until_a_memory_value() {
    let s = rom_spec(
        Hardware::DMG,
        counter_rom(),
        vec![Stop::Mem {
            addr: 0xFF80,
            value: 0x42,
        }],
    );
    let r = run_rom(&s).unwrap();
    assert_eq!(r.regs.a, 0x42);
    assert_eq!(r.regs.pc, 0x0154);
    assert_eq!(r.dump, [(0xFF80, vec![0x42])]);
    assert_eq!(r.steps, 3);
}

#[test]
fn rom_stops_before_a_pc_and_at_a_cycle_budget() {
    let r = run_rom(&rom_spec(
        Hardware::CGB,
        counter_rom(),
        vec![Stop::Pc(0x0150)],
    ))
    .unwrap();
    assert_eq!((r.regs.pc, r.steps), (0x0150, 1));
    let r = run_rom(&rom_spec(
        Hardware::CGB,
        counter_rom(),
        vec![Stop::Cycles(1000)],
    ))
    .unwrap();
    assert!(r.cycles >= 1000 && r.cycles < 1020);
    assert_eq!(r.dump[0].1[0], 0x42);
}

#[test]
fn rom_runs_are_deterministic_and_frame_hash_tracks_the_screen() {
    let stops = vec![Stop::Frames(3)];
    let a = run_rom(&rom_spec(Hardware::CGB, counter_rom(), stops.clone())).unwrap();
    let b = run_rom(&rom_spec(Hardware::CGB, counter_rom(), stops.clone())).unwrap();
    assert_eq!(a, b);
    assert_eq!(a.reason.0, Stop::Frames(3));
    let mut lit = counter_rom();
    lit[0x150..0x15C].copy_from_slice(&[
        0x3E, 0x00, 0xEA, 0x00, 0x80, 0x3E, 0xFF, 0xEA, 0x01, 0x80, 0x18, 0xFE,
    ]);
    let c = run_rom(&rom_spec(Hardware::DMG, lit, stops)).unwrap();
    let d = run_rom(&rom_spec(
        Hardware::DMG,
        counter_rom(),
        vec![Stop::Frames(3)],
    ))
    .unwrap();
    assert_ne!(c.frame_hash, d.frame_hash);
}

#[test]
fn hardware_revisions_start_with_their_own_registers() {
    let go = |hw| {
        run_rom(&rom_spec(hw, counter_rom(), vec![Stop::Pc(0x0100)]))
            .unwrap()
            .regs
    };
    assert_eq!(go(Hardware::DMG).a, 0x01);
    assert_eq!(go(Hardware::CGB).a, 0x11);
    assert_eq!(go(Hardware::CGBE).a, 0x11);
}

#[test]
fn rom_needs_a_stop_condition() {
    assert!(run_rom(&rom_spec(Hardware::DMG, counter_rom(), vec![])).is_err());
}

#[test]
fn other_oracles_fall_through() {
    let a = |s: &str| s.split(' ').map(String::from).collect::<Vec<_>>();
    assert!(dispatch(&a("insn 8b020020")).is_none());
    assert!(dispatch(&a("--oracle unicorn insn 8b020020")).is_none());
}

#[test]
fn a_real_rom_runs_when_one_is_given() {
    let Ok(path) = std::env::var("RELIFT_GB_TEST_ROM") else {
        return;
    };
    let rom = std::fs::read(&path).unwrap();
    let go = |hw| run_rom(&rom_spec(hw, rom.clone(), vec![Stop::Frames(120)])).unwrap();
    let (a, b) = (go(Hardware::CGB), go(Hardware::CGB));
    assert_eq!(a, b);
    assert!(a.steps > 1000);
    let args: Vec<String> = [
        path.as_str(),
        "--hw",
        "cgb",
        "--frames",
        "2",
        "--pc",
        "0150",
        "--dump",
        "ff80:4",
    ]
    .map(String::from)
    .to_vec();
    let s = parse_rom(&args).unwrap();
    assert_eq!(s.stops, [Stop::Frames(2), Stop::Pc(0x150)]);
    assert_eq!(s.dump, [(0xFF80, 4)]);
}

#[test]
fn a_stop_that_never_hits_ends_at_the_default_budget() {
    let r = run_rom(&rom_spec(
        Hardware::DMG,
        counter_rom(),
        vec![Stop::Pc(0x0000)],
    ))
    .unwrap();
    assert_eq!(r.reason.0, Stop::Cycles(600 * 70_224));
}
