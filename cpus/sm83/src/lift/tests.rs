use relift_ir::{IoMap, Op};

use super::*;
use crate::{BASE, CB, decode};

const LIFTED: &[Mnemonic] = &[
    Mnemonic::Ld,
    Mnemonic::Push,
    Mnemonic::Pop,
    Mnemonic::Inc,
    Mnemonic::Dec,
    Mnemonic::Add,
    Mnemonic::Adc,
    Mnemonic::Sub,
    Mnemonic::Sbc,
    Mnemonic::And,
    Mnemonic::Xor,
    Mnemonic::Or,
    Mnemonic::Cp,
    Mnemonic::Cpl,
    Mnemonic::Scf,
    Mnemonic::Ccf,
    Mnemonic::Rlca,
    Mnemonic::Rrca,
    Mnemonic::Rla,
    Mnemonic::Rra,
    Mnemonic::Rlc,
    Mnemonic::Rrc,
    Mnemonic::Rl,
    Mnemonic::Rr,
    Mnemonic::Sla,
    Mnemonic::Sra,
    Mnemonic::Swap,
    Mnemonic::Srl,
    Mnemonic::Bit,
    Mnemonic::Res,
    Mnemonic::Set,
];

fn bytes_of(prefix: bool, code: u8) -> Vec<u8> {
    let mut v = if prefix { vec![0xCB, code] } else { vec![code] };
    v.resize(3, 0x12);
    v
}

fn devices() -> IoMap {
    IoMap {
        ranges: Vec::new(),
        default: relift_ir::AddrClass::Device,
    }
}

fn lift(prefix: bool, code: u8) -> Result<Block, LiftError> {
    let ins = decode(&bytes_of(prefix, code)).unwrap();
    lift_instruction(&ins, 0x0150, &devices())
}

fn all() -> impl Iterator<Item = (bool, u8)> {
    (0..=255u8)
        .flat_map(|c| [(false, c), (true, c)])
        .filter(|&(prefix, code)| prefix || code != 0xCB)
}

#[test]
fn every_opcode_in_scope_lifts_and_the_rest_report_why() {
    let mut lifted = 0;
    for (prefix, code) in all() {
        let table = if prefix { &CB } else { &BASE };
        let m = table[usize::from(code)].mnemonic;
        match lift(prefix, code) {
            Ok(_) => {
                assert!(LIFTED.contains(&m), "{m:?} {code:#x} lifted unexpectedly");
                lifted += 1;
            }
            Err(LiftError::NotYetLifted(x)) => {
                assert_eq!(x, m);
                assert!(!LIFTED.contains(&m));
            }
            Err(LiftError::Illegal(c)) => assert_eq!((m, c), (Mnemonic::Illegal, code)),
            Err(e) => panic!("{m:?} {code:#x}: {e}"),
        }
    }
    assert_eq!(lifted, 464);
}

#[test]
fn ticks_total_the_table_cost_and_place_accesses_at_their_cycle() {
    for (prefix, code) in all() {
        let Ok(block) = lift(prefix, code) else {
            continue;
        };
        let table = if prefix { &CB } else { &BASE };
        let op = &table[usize::from(code)];
        let mut clock = 0u32;
        let mut seen = Vec::new();
        for inst in &block.insts {
            match inst.op {
                Op::Tick(n) => clock += n,
                Op::IoLoad { .. } | Op::IoStore { .. } => seen.push(clock),
                _ => {}
            }
        }
        assert_eq!(clock, 4 * u32::from(op.cycles.not_taken), "{code:#x}");
        let want: Vec<u32> = op
            .accesses(false)
            .iter()
            .filter(|a| a.kind != AccessKind::Fetch)
            .map(|a| 4 * (u32::from(a.cycle) - 1))
            .collect();
        assert_eq!(seen, want, "{prefix} {code:#x}");
    }
}

#[test]
fn constant_high_addresses_become_device_accesses() {
    use relift_ir::{AddrClass, IoRange};
    let map = IoMap {
        ranges: vec![IoRange {
            first: 0xFF00,
            last: 0xFFFF,
            read: AddrClass::Device,
            write: AddrClass::Device,
        }],
        default: AddrClass::Memory,
    };
    let ins = decode(&[0xE0, 0x40]).unwrap();
    let block = lift_instruction(&ins, 0x0100, &map).unwrap();
    assert!(block.insts.iter().any(|i| matches!(
        i.op,
        Op::IoStore {
            kind: relift_ir::IoKind::Device,
            ..
        }
    )));
    let ins = decode(&[0xEA, 0x00, 0xC0]).unwrap();
    let block = lift_instruction(&ins, 0x0100, &map).unwrap();
    assert!(block.insts.iter().any(|i| matches!(i.op, Op::Store { .. })));
}

#[test]
fn the_frontend_lifts_and_reports_failures() {
    use relift_guest::Frontend;
    let fe = crate::Sm83::new(IoMap::memory());
    assert!(fe.lift(&[0x3C], 0x100).is_ok());
    let e = fe.lift(&[0xC3, 0x00, 0x01], 0x100).unwrap_err();
    assert!(e.message.contains("not lifted yet"), "{}", e.message);
    assert!(fe.lift(&[0xC5], 0x1_0000).is_err());
    assert!(fe.lift(&[0xEA, 0x00], 0x100).is_err());
}
