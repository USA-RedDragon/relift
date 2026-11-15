use std::path::{Path, PathBuf};

use super::cli::{parse_rom, ppm, run_rom};
use super::{Event, Nba, Options, Regs, binary, parse_event, parse_regs};

const FNV_BASIS: u64 = 0xcbf2_9ce4_8422_2325;

fn fnv(hash: u64, bytes: &[u8]) -> u64 {
    bytes.iter().fold(hash, |h, &b| {
        (h ^ u64::from(b)).wrapping_mul(0x100_0000_01b3)
    })
}

fn reg(r: &Regs, name: &str) -> Option<u32> {
    r.named.get(name).copied()
}

const PROGRAM: [u32; 9] = [
    0xE3A0_0001,
    0xE280_1002,
    0xE3A0_2301,
    0xE3A0_3B01,
    0xE383_3003,
    0xE1C2_30B0,
    0xE3A0_4403,
    0xE584_1000,
    0xEAFF_FFFE,
];

fn rom_file(name: &str) -> PathBuf {
    let mut rom = vec![0u8; 0x200];
    for (i, w) in PROGRAM.iter().enumerate() {
        rom[4 * i..4 * i + 4].copy_from_slice(&w.to_le_bytes());
    }
    let p = std::env::temp_dir().join(format!("relift-gba-{}-{name}.gba", std::process::id()));
    relift_cache::write_if_changed(&p, &rom).unwrap();
    p
}

fn oracle(test: &str) -> Option<PathBuf> {
    let b = binary().filter(|b| b.exists());
    if b.is_none() {
        eprintln!("{test}: skipped, no nba-oracle (check/oracle/nba/build.sh)");
    }
    b
}

fn start(bin: &Path, name: &str) -> Nba {
    Nba::spawn(
        bin,
        &Options {
            rom: rom_file(name),
            skip_bios: true,
            ..Options::default()
        },
    )
    .unwrap()
}

#[test]
fn parses_a_trace_line() {
    let l = "T 12 08000004 e2801002 00000001 00000003 0 0 0 0 0 0 0 0 0 0 0 03007f00 0 0800000c 0000001f";
    match parse_event(l).unwrap() {
        Event::Insn {
            cycles,
            pc,
            opcode,
            regs,
            cpsr,
        } => {
            assert_eq!(
                (cycles, pc, opcode, cpsr),
                (12, 0x0800_0004, 0xE280_1002, 0x1F)
            );
            assert_eq!((regs[1], regs[13], regs[15]), (3, 0x0300_7F00, 0x0800_000C));
        }
        e => panic!("{e:?}"),
    }
    assert_eq!(
        parse_event("I 80 08000010").unwrap(),
        Event::Irq {
            cycles: 80,
            pc: 0x0800_0010
        }
    );
    assert_eq!(parse_event("H 99").unwrap(), Event::Halt { cycles: 99 });
    assert!(parse_event("T 1 2").is_err());
}

#[test]
fn parses_a_regs_line() {
    let r = parse_regs("REGS r0=00000001 cpsr=6000001f r13_svc=03007fe0 pc=08000008 halt=1 cyc=42")
        .unwrap();
    assert_eq!(reg(&r, "r0"), Some(1));
    assert_eq!(reg(&r, "cpsr"), Some(0x6000_001F));
    assert_eq!(reg(&r, "r13_svc"), Some(0x0300_7FE0));
    assert_eq!((r.pc, r.halted, r.cycles), (0x0800_0008, true, 42));
    assert!(parse_regs("MEM 00").is_err());
}

#[test]
fn parses_rom_options() {
    let a = |s: &str| s.split(' ').map(String::from).collect::<Vec<_>>();
    let r = parse_rom(&a(
        "x.gba --skip-bios --frames 2 --dump 3000000:16 --keys 9 --io",
    ))
    .unwrap();
    assert_eq!((r.frames, r.keys, r.io), (Some(2), 9, true));
    assert_eq!(r.dump, vec![(0x0300_0000, 16)]);
    assert!(parse_rom(&a("x.gba --skip-bios --bogus 1")).is_err());
}

#[test]
fn ppm_expands_bgr555() {
    let mut px = vec![0u16; 240 * 160];
    px[0] = 0x7C1F;
    let img = ppm(&px);
    let head = b"P6\n240 160\n255\n";
    assert_eq!(&img[..head.len()], head);
    assert_eq!(&img[head.len()..head.len() + 3], &[0xFF, 0x00, 0xFF]);
    assert_eq!(img.len(), head.len() + 240 * 160 * 3);
}

#[test]
fn rom_command_pokes_sets_and_jumps() {
    let Some(bin) = oracle("rom_command_pokes_sets_and_jumps") else {
        return;
    };
    let rom = rom_file("cli-jump");
    let px = std::env::temp_dir().join(format!("relift-gba-{}.ppm", std::process::id()));
    let args: Vec<String> = [
        rom.to_str().unwrap(),
        "--skip-bios",
        "--bin",
        bin.to_str().unwrap(),
        "--poke",
        "3000100=022091e0feffffea",
        "--set",
        "r1=ffffffff",
        "--set",
        "r2=1",
        "--jump",
        "3000100",
        "--trace",
        "1",
        "--pixels",
        px.to_str().unwrap(),
    ]
    .iter()
    .map(|s| s.to_string())
    .collect();
    let out = run_rom(&parse_rom(&args).unwrap()).unwrap();
    let t = out.iter().find(|l| l.starts_with("trace ")).unwrap();
    assert!(t.contains(" 03000100 e0912002 "), "{t}");
    assert!(t.ends_with("cpsr=600000df"), "{t}");
    assert_eq!(std::fs::read(&px).unwrap().len(), 15 + 240 * 160 * 3);
    std::fs::remove_file(px).unwrap();
}

#[test]
fn runs_a_program_and_reads_state() {
    let Some(bin) = oracle("runs_a_program_and_reads_state") else {
        return;
    };
    let mut nba = start(&bin, "program");
    nba.run_cycles(2000).unwrap();
    let r = nba.regs().unwrap();
    assert_eq!((reg(&r, "r0"), reg(&r, "r1")), (Some(1), Some(3)));
    assert_eq!(reg(&r, "cpsr").map(|c| c & 0x1F), Some(0x1F));
    assert_eq!(reg(&r, "r13_svc"), Some(0x0300_7FE0));
    assert_eq!(r.pc, 0x0800_0020);
    assert!(r.cycles >= 2000);
    assert_eq!(nba.mem(0x0300_0000, 4).unwrap(), [3, 0, 0, 0]);
    assert_eq!(nba.mem(0x0800_0000, 4).unwrap(), [1, 0, 0xA0, 0xE3]);
    let io = nba.io().unwrap();
    assert_eq!(io.len(), 0x400);
    assert_eq!(&io[0..2], &[0x03, 0x04]);
}

#[test]
fn steps_with_a_trace() {
    let Some(bin) = oracle("steps_with_a_trace") else {
        return;
    };
    let mut nba = start(&bin, "trace");
    let t = nba.step(6).unwrap();
    let insns: Vec<_> = t
        .iter()
        .filter_map(|e| match e {
            Event::Insn {
                pc, opcode, regs, ..
            } if pc & 0xFF00_0000 == 0x0800_0000 => Some((*pc, *opcode, regs[0], regs[1])),
            _ => None,
        })
        .collect();
    assert_eq!(insns[0], (0x0800_0000, 0xE3A0_0001, 1, 0));
    assert_eq!(insns[1], (0x0800_0004, 0xE280_1002, 1, 3));
    let cycles: Vec<u64> = t
        .iter()
        .map(|e| match e {
            Event::Insn { cycles, .. } | Event::Halt { cycles } | Event::Irq { cycles, .. } => {
                *cycles
            }
        })
        .collect();
    assert!(cycles.windows(2).all(|w| w[0] <= w[1]));
}

#[test]
fn jumps_and_sets_registers() {
    let Some(bin) = oracle("jumps_and_sets_registers") else {
        return;
    };
    let mut nba = start(&bin, "jump");
    nba.poke(0x0300_0100, &0xE091_2002u32.to_le_bytes())
        .unwrap();
    nba.set(&[("r1", 0xFFFF_FFFF), ("r2", 1), ("cpsr", 0x1F)])
        .unwrap();
    nba.jump(0x0300_0100, false).unwrap();
    let t = nba.step(1).unwrap();
    match &t[..] {
        [Event::Insn { pc, regs, cpsr, .. }] => {
            assert_eq!(*pc, 0x0300_0100);
            assert_eq!(regs[2], 0);
            assert_eq!(cpsr >> 28, 0b0110);
        }
        t => panic!("{t:?}"),
    }
    assert!(nba.poke(0x0800_0000, &[0]).is_err());
}

#[test]
fn frames_and_audio_have_digests() {
    let Some(bin) = oracle("frames_and_audio_have_digests") else {
        return;
    };
    let mut nba = start(&bin, "frames");
    let c = nba.run_frames(2).unwrap();
    assert_eq!(c.frames, 2);
    let f = nba.frame().unwrap();
    assert_eq!(f.count, 2);
    let black = fnv(FNV_BASIS, &vec![0u8; 240 * 160 * 2]);
    assert_eq!(f.hash, black);
    let px = nba.pixels().unwrap();
    assert_eq!(px.len(), 240 * 160);
    let a = nba.audio().unwrap();
    assert!(a.count > 500);
    assert_eq!(nba.command("RESET").unwrap(), "OK");
    assert_eq!(nba.frame().unwrap().count, 0);
}

#[test]
fn rom_command_prints_state() {
    let Some(bin) = oracle("rom_command_prints_state") else {
        return;
    };
    let rom = rom_file("cli");
    let args: Vec<String> = [
        rom.to_str().unwrap(),
        "--skip-bios",
        "--bin",
        bin.to_str().unwrap(),
        "--cycles",
        "500",
        "--dump",
        "3000000:4",
        "--trace",
        "1",
    ]
    .iter()
    .map(|s| s.to_string())
    .collect();
    let out = run_rom(&parse_rom(&args).unwrap()).unwrap();
    assert!(out.iter().any(|l| l == "mem 03000000 03000000"), "{out:?}");
    assert!(out.iter().any(|l| l.starts_with("trace ")));
    assert!(out.iter().any(|l| l.starts_with("frame 0 ")));
}
