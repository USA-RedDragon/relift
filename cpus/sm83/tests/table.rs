use relift_cpu_sm83::{
    AccessKind, AccessTarget, BASE, CB, Cond, DecodeError, FlagEffect, FlagSet, Immediate,
    Mnemonic, Operand, Prefix, Reg16, decode,
};

const ILLEGAL: [u8; 11] = [
    0xD3, 0xDB, 0xDD, 0xE3, 0xE4, 0xEB, 0xEC, 0xED, 0xF4, 0xFC, 0xFD,
];

const STEPS: &str = include_str!("data/rustyboi_steps.txt");

#[test]
fn tables_are_indexed_by_their_opcode_byte() {
    for i in 0..256 {
        assert_eq!(usize::from(BASE[i].code), i);
        assert_eq!(BASE[i].prefix, Prefix::None);
        assert_eq!(usize::from(CB[i].code), i);
        assert_eq!(CB[i].prefix, Prefix::Cb);
    }
}

#[test]
fn exactly_the_eleven_unused_opcodes_are_illegal() {
    let found: Vec<u8> = BASE
        .iter()
        .filter(|op| op.is_illegal())
        .map(|op| op.code)
        .collect();
    assert_eq!(found, ILLEGAL);
    assert!(CB.iter().all(|op| !op.is_illegal()));
}

#[test]
fn prefix_byte_is_the_only_prefix_entry() {
    let found: Vec<u8> = BASE
        .iter()
        .filter(|op| op.mnemonic == Mnemonic::Prefix)
        .map(|op| op.code)
        .collect();
    assert_eq!(found, [0xCB]);
}

#[test]
fn lengths_are_one_to_three_and_cb_is_two() {
    for op in &BASE {
        assert!((1..=3).contains(&op.length), "{:02X}", op.code);
    }
    for op in &CB {
        assert_eq!(op.length, 2, "CB {:02X}", op.code);
    }
}

#[test]
fn length_matches_operand_kinds() {
    for op in &BASE {
        let imm = |o: Operand| match o {
            Operand::Imm16 | Operand::IndirectImm16 => 2,
            Operand::Imm8 | Operand::SignedImm8 | Operand::HighImm8 | Operand::SpPlusSigned8 => 1,
            _ => 0,
        };
        let extra = imm(op.dst) + imm(op.src);
        let want = if op.code == 0x10 { 2 } else { 1 + extra };
        if op.mnemonic == Mnemonic::Illegal || op.mnemonic == Mnemonic::Prefix {
            continue;
        }
        assert_eq!(op.length, want, "{:02X}", op.code);
    }
}

#[test]
fn only_conditional_branches_have_two_costs() {
    for op in &BASE {
        let conditional = matches!(op.dst, Operand::Cond(_))
            && matches!(
                op.mnemonic,
                Mnemonic::Jr | Mnemonic::Jp | Mnemonic::Call | Mnemonic::Ret
            );
        assert_eq!(op.cycles.is_conditional(), conditional, "{:02X}", op.code);
        if conditional {
            assert!(op.cycles.taken > op.cycles.not_taken);
        }
    }
    for op in &CB {
        assert!(!op.cycles.is_conditional());
    }
}

#[test]
fn known_costs() {
    let cost = |code: usize| (BASE[code].cycles.not_taken, BASE[code].cycles.taken);
    assert_eq!(cost(0x00), (1, 1));
    assert_eq!(cost(0x08), (5, 5));
    assert_eq!(cost(0x18), (3, 3));
    assert_eq!(cost(0x20), (2, 3));
    assert_eq!(cost(0xC0), (2, 5));
    assert_eq!(cost(0xC9), (4, 4));
    assert_eq!(cost(0xC2), (3, 4));
    assert_eq!(cost(0xC3), (4, 4));
    assert_eq!(cost(0xC4), (3, 6));
    assert_eq!(cost(0xCD), (6, 6));
    assert_eq!(cost(0xC5), (4, 4));
    assert_eq!(cost(0xC1), (3, 3));
    assert_eq!(cost(0xC7), (4, 4));
    assert_eq!(cost(0xE8), (4, 4));
    assert_eq!(cost(0xF8), (3, 3));
    assert_eq!(cost(0xF9), (2, 2));
    assert_eq!(cost(0xE9), (1, 1));
    assert_eq!(cost(0x34), (3, 3));
    assert_eq!(cost(0x36), (3, 3));
    assert_eq!(
        (CB[0x06].cycles.not_taken, CB[0x46].cycles.not_taken),
        (4, 3)
    );
    assert_eq!(
        (CB[0x86].cycles.not_taken, CB[0xC6].cycles.not_taken),
        (4, 4)
    );
    assert_eq!(CB[0x00].cycles.not_taken, 2);
}

#[test]
fn accesses_are_ordered_and_inside_the_instruction() {
    for table in [&BASE, &CB] {
        for op in table.iter() {
            for taken in [false, true] {
                let list = op.accesses(taken);
                let limit = op.cycles.get(taken);
                let mut last = 0;
                for access in list {
                    assert!(access.cycle > last, "{:02X} order", op.code);
                    assert!(access.cycle <= limit, "{:02X} range", op.code);
                    last = access.cycle;
                }
                let fetches = list.iter().filter(|a| a.kind == AccessKind::Fetch).count();
                if op.is_illegal() {
                    continue;
                }
                let want = if op.code == 0x10 && op.prefix == Prefix::None {
                    1
                } else {
                    usize::from(op.length)
                };
                assert_eq!(fetches, want, "{:02X} fetch count", op.code);
                assert!(
                    list.iter()
                        .take(fetches)
                        .enumerate()
                        .all(|(i, a)| a.kind == AccessKind::Fetch && usize::from(a.cycle) == i + 1),
                    "{:02X} fetches come first",
                    op.code
                );
            }
        }
    }
}

#[test]
fn memory_timing_of_selected_instructions() {
    let data = |op: &relift_cpu_sm83::Opcode, taken| -> Vec<(u8, AccessKind, AccessTarget)> {
        op.accesses(taken)
            .iter()
            .filter(|a| a.kind != AccessKind::Fetch)
            .map(|a| (a.cycle, a.kind, a.target))
            .collect()
    };
    use AccessKind::{Read, Write};
    use AccessTarget as T;
    assert_eq!(
        data(&BASE[0x34], false),
        [(2, Read, T::Hl), (3, Write, T::Hl)]
    );
    assert_eq!(data(&BASE[0x36], false), [(3, Write, T::Hl)]);
    assert_eq!(
        data(&BASE[0x08], false),
        [(4, Write, T::Imm16), (5, Write, T::Imm16Plus1)]
    );
    assert_eq!(data(&BASE[0xEA], false), [(4, Write, T::Imm16)]);
    assert_eq!(data(&BASE[0xFA], false), [(4, Read, T::Imm16)]);
    assert_eq!(data(&BASE[0xE0], false), [(3, Write, T::HighImm8)]);
    assert_eq!(data(&BASE[0xF2], false), [(2, Read, T::HighC)]);
    assert_eq!(
        data(&BASE[0xC5], false),
        [(3, Write, T::PushHigh), (4, Write, T::PushLow)]
    );
    assert_eq!(
        data(&BASE[0xC1], false),
        [(2, Read, T::PopLow), (3, Read, T::PopHigh)]
    );
    assert_eq!(
        data(&BASE[0xCD], false),
        [(5, Write, T::PushHigh), (6, Write, T::PushLow)]
    );
    assert_eq!(data(&BASE[0xC4], false), []);
    assert_eq!(
        data(&BASE[0xC4], true),
        [(5, Write, T::PushHigh), (6, Write, T::PushLow)]
    );
    assert_eq!(data(&BASE[0xC0], false), []);
    assert_eq!(
        data(&BASE[0xC0], true),
        [(3, Read, T::PopLow), (4, Read, T::PopHigh)]
    );
    assert_eq!(
        data(&BASE[0xC9], false),
        [(2, Read, T::PopLow), (3, Read, T::PopHigh)]
    );
    assert_eq!(
        data(&BASE[0xC7], false),
        [(3, Write, T::PushHigh), (4, Write, T::PushLow)]
    );
    assert_eq!(data(&BASE[0x2A], false), [(2, Read, T::Hl)]);
    assert_eq!(
        data(&CB[0x06], false),
        [(3, Read, T::Hl), (4, Write, T::Hl)]
    );
    assert_eq!(data(&CB[0x46], false), [(3, Read, T::Hl)]);
    assert_eq!(
        data(&CB[0xC6], false),
        [(3, Read, T::Hl), (4, Write, T::Hl)]
    );
    assert_eq!(data(&CB[0x00], false), []);
}

#[test]
fn every_non_fetch_access_matches_a_memory_operand() {
    for op in BASE.iter().chain(CB.iter()) {
        let touches = |o: Operand| {
            matches!(
                o,
                Operand::Indirect(_)
                    | Operand::HlInc
                    | Operand::HlDec
                    | Operand::IndirectC
                    | Operand::IndirectImm16
                    | Operand::HighImm8
            )
        };
        let stack = matches!(
            op.mnemonic,
            Mnemonic::Push
                | Mnemonic::Pop
                | Mnemonic::Call
                | Mnemonic::Ret
                | Mnemonic::Reti
                | Mnemonic::Rst
        );
        let has_data = op
            .accesses(true)
            .iter()
            .any(|a| a.kind != AccessKind::Fetch);
        let jp_hl = op.mnemonic == Mnemonic::Jp && op.src == Operand::R16(Reg16::Hl);
        let expect = (touches(op.dst) || touches(op.src) || stack) && !jp_hl;
        assert_eq!(has_data, expect, "{:?} {:02X}", op.prefix, op.code);
    }
}

#[test]
fn flags_of_selected_instructions() {
    use FlagEffect::{Clear, Compute, Keep, Load, Set};
    let f = |op: &relift_cpu_sm83::Opcode| (op.flags.z, op.flags.n, op.flags.h, op.flags.c);
    assert_eq!(f(&BASE[0x00]), (Keep, Keep, Keep, Keep));
    assert_eq!(f(&BASE[0x04]), (Compute, Clear, Compute, Keep));
    assert_eq!(f(&BASE[0x05]), (Compute, Set, Compute, Keep));
    assert_eq!(f(&BASE[0x03]), (Keep, Keep, Keep, Keep));
    assert_eq!(f(&BASE[0x09]), (Keep, Clear, Compute, Compute));
    assert_eq!(f(&BASE[0x07]), (Clear, Clear, Clear, Compute));
    assert_eq!(f(&BASE[0x27]), (Compute, Keep, Clear, Compute));
    assert_eq!(
        BASE[0x27].flags.read,
        FlagSet::N.union(FlagSet::H).union(FlagSet::C)
    );
    assert_eq!(f(&BASE[0x2F]), (Keep, Set, Set, Keep));
    assert_eq!(f(&BASE[0x37]), (Keep, Clear, Clear, Set));
    assert_eq!(f(&BASE[0x3F]), (Keep, Clear, Clear, Compute));
    assert_eq!(BASE[0x3F].flags.read, FlagSet::C);
    assert_eq!(f(&BASE[0x80]), (Compute, Clear, Compute, Compute));
    assert_eq!(f(&BASE[0x88]), (Compute, Clear, Compute, Compute));
    assert_eq!(BASE[0x88].flags.read, FlagSet::C);
    assert_eq!(f(&BASE[0x98]), (Compute, Set, Compute, Compute));
    assert_eq!(BASE[0x98].flags.read, FlagSet::C);
    assert_eq!(f(&BASE[0xA0]), (Compute, Clear, Set, Clear));
    assert_eq!(f(&BASE[0xA8]), (Compute, Clear, Clear, Clear));
    assert_eq!(f(&BASE[0xB8]), (Compute, Set, Compute, Compute));
    assert_eq!(f(&BASE[0xE8]), (Clear, Clear, Compute, Compute));
    assert_eq!(f(&BASE[0xF8]), (Clear, Clear, Compute, Compute));
    assert_eq!(f(&BASE[0xF1]), (Load, Load, Load, Load));
    assert_eq!(f(&BASE[0xC1]), (Keep, Keep, Keep, Keep));
    assert_eq!(f(&CB[0x00]), (Compute, Clear, Clear, Compute));
    assert_eq!(f(&CB[0x10]), (Compute, Clear, Clear, Compute));
    assert_eq!(CB[0x10].flags.read, FlagSet::C);
    assert_eq!(CB[0x00].flags.read, FlagSet::NONE);
    assert_eq!(f(&CB[0x30]), (Compute, Clear, Clear, Clear));
    assert_eq!(f(&CB[0x40]), (Compute, Clear, Set, Keep));
    assert_eq!(f(&CB[0x80]), (Keep, Keep, Keep, Keep));
    assert_eq!(f(&CB[0xC0]), (Keep, Keep, Keep, Keep));
}

#[test]
fn conditional_branches_read_the_tested_flag() {
    for (code, flag) in [
        (0x20, FlagSet::Z),
        (0x28, FlagSet::Z),
        (0x30, FlagSet::C),
        (0x38, FlagSet::C),
        (0xC0, FlagSet::Z),
        (0xC8, FlagSet::Z),
        (0xD0, FlagSet::C),
        (0xD8, FlagSet::C),
        (0xC2, FlagSet::Z),
        (0xCA, FlagSet::Z),
        (0xD2, FlagSet::C),
        (0xDA, FlagSet::C),
        (0xC4, FlagSet::Z),
        (0xCC, FlagSet::Z),
        (0xD4, FlagSet::C),
        (0xDC, FlagSet::C),
    ] {
        assert_eq!(BASE[code].flags.read, flag, "{code:02X}");
    }
    assert_eq!(BASE[0x20].dst, Operand::Cond(Cond::Nz));
    assert_eq!(BASE[0xDC].dst, Operand::Cond(Cond::C));
}

#[test]
fn only_flag_writers_listed_write_flags() {
    for op in &BASE {
        let writes = !op.flags.written().is_empty();
        let expect = match op.mnemonic {
            Mnemonic::Inc | Mnemonic::Dec => {
                matches!(op.dst, Operand::R8(_) | Operand::Indirect(_))
            }
            Mnemonic::Add => true,
            Mnemonic::Adc
            | Mnemonic::Sub
            | Mnemonic::Sbc
            | Mnemonic::And
            | Mnemonic::Xor
            | Mnemonic::Or
            | Mnemonic::Cp
            | Mnemonic::Rlca
            | Mnemonic::Rrca
            | Mnemonic::Rla
            | Mnemonic::Rra
            | Mnemonic::Daa
            | Mnemonic::Cpl
            | Mnemonic::Scf
            | Mnemonic::Ccf => true,
            Mnemonic::Pop => op.code == 0xF1,
            Mnemonic::Ld => op.code == 0xF8,
            _ => false,
        };
        assert_eq!(writes, expect, "{:02X}", op.code);
    }
}

#[test]
fn rst_vectors() {
    for y in 0..8u8 {
        let op = &BASE[usize::from(0xC7 + y * 8)];
        assert_eq!(op.mnemonic, Mnemonic::Rst);
        assert_eq!(op.dst, Operand::Vector(y * 8));
    }
}

#[test]
fn cb_operands_cover_every_register_and_bit() {
    for code in 0..=255u8 {
        let op = &CB[usize::from(code)];
        let z = code & 7;
        let y = (code >> 3) & 7;
        let target = if code < 0x40 { op.dst } else { op.src };
        assert_eq!(
            target == Operand::Indirect(Reg16::Hl),
            z == 6,
            "CB {code:02X}"
        );
        if code >= 0x40 {
            assert_eq!(op.dst, Operand::Bit(y));
        }
    }
}

#[test]
fn table_agrees_with_rustyboi_stepping() {
    let mut rows = 0;
    for line in STEPS.lines().filter(|l| !l.starts_with('#')) {
        let f: Vec<&str> = line.split_whitespace().collect();
        let table = if f[0] == "cb" { &CB } else { &BASE };
        let code = usize::from(u8::from_str_radix(f[1], 16).unwrap());
        let advance: u32 = f[2].parse().unwrap();
        let not_taken: u8 = f[3].parse().unwrap();
        let taken: u8 = f[4].parse().unwrap();
        let op = &table[code];
        let name = format!("{} {:02X}", f[0], code);
        let rst = f[0] == "base" && code & 0xC7 == 0xC7;
        let ret = f[0] == "base" && matches!(code, 0xC9 | 0xD9);
        if !rst && !ret {
            assert_eq!(advance, u32::from(op.length), "{name} length");
        }
        if f[0] == "base" && code == 0x10 {
            assert_eq!((not_taken, taken), (2, 2), "STOP as measured");
            assert_eq!(op.cycles.not_taken, 1, "STOP as documented");
            continue;
        }
        assert_eq!(not_taken, op.cycles.not_taken, "{name} not taken");
        assert_eq!(taken, op.cycles.taken, "{name} taken");
        rows += 1;
    }
    assert_eq!(rows, 500 - 1);
}

#[test]
fn oracle_data_covers_every_legal_opcode() {
    let mut seen = std::collections::HashSet::new();
    for line in STEPS.lines().filter(|l| !l.starts_with('#')) {
        let f: Vec<&str> = line.split_whitespace().collect();
        seen.insert((f[0].to_string(), u8::from_str_radix(f[1], 16).unwrap()));
    }
    for code in 0..=255u8 {
        assert!(seen.contains(&("cb".to_string(), code)));
        let skip = ILLEGAL.contains(&code) || code == 0xCB;
        assert_eq!(
            seen.contains(&("base".to_string(), code)),
            !skip,
            "{code:02X}"
        );
    }
}

#[test]
fn decode_reads_immediates() {
    let i = decode(&[0x01, 0x34, 0x12, 0xFF]).unwrap();
    assert_eq!(i.opcode.mnemonic, Mnemonic::Ld);
    assert_eq!(i.immediate, Immediate::U16(0x1234));
    assert_eq!(i.length(), 3);

    let i = decode(&[0x18, 0xFE]).unwrap();
    assert_eq!(i.immediate, Immediate::I8(-2));
    assert_eq!(i.branch_target(0x0150), Some(0x0150));

    let i = decode(&[0x20, 0x05]).unwrap();
    assert_eq!(i.branch_target(0x0100), Some(0x0107));

    let i = decode(&[0xC3, 0x00, 0x40]).unwrap();
    assert_eq!(i.branch_target(0x0100), Some(0x4000));
    let i = decode(&[0xCD, 0x00, 0x20]).unwrap();
    assert_eq!(i.branch_target(0x0100), Some(0x2000));
    let i = decode(&[0xEF]).unwrap();
    assert_eq!(i.branch_target(0x0100), Some(0x28));
    assert_eq!(decode(&[0xC9]).unwrap().branch_target(0), None);
    assert_eq!(decode(&[0xE9]).unwrap().branch_target(0), None);

    let i = decode(&[0xE0, 0x80]).unwrap();
    assert_eq!(i.immediate, Immediate::U8(0x80));
    let i = decode(&[0xEA, 0x00, 0xC0]).unwrap();
    assert_eq!(i.immediate, Immediate::U16(0xC000));
    let i = decode(&[0x08, 0x00, 0xC0]).unwrap();
    assert_eq!(i.immediate, Immediate::U16(0xC000));
    let i = decode(&[0xE8, 0xFF]).unwrap();
    assert_eq!(i.immediate, Immediate::I8(-1));
    let i = decode(&[0xF8, 0x7F]).unwrap();
    assert_eq!(i.immediate, Immediate::I8(127));
    let i = decode(&[0x10, 0x00]).unwrap();
    assert_eq!(i.opcode.mnemonic, Mnemonic::Stop);
    assert_eq!(i.length(), 2);
    let i = decode(&[0xCB, 0x7C, 0xFF]).unwrap();
    assert_eq!(i.opcode.mnemonic, Mnemonic::Bit);
    assert_eq!(i.immediate, Immediate::None);
    assert_eq!(i.length(), 2);
    let i = decode(&[0xD3]).unwrap();
    assert!(i.opcode.is_illegal());
}

#[test]
fn decode_reports_truncation() {
    assert_eq!(decode(&[]), Err(DecodeError::Truncated { needed: 1 }));
    assert_eq!(decode(&[0xCB]), Err(DecodeError::Truncated { needed: 2 }));
    assert_eq!(
        decode(&[0xC3, 0x00]),
        Err(DecodeError::Truncated { needed: 3 })
    );
    assert_eq!(decode(&[0x10]), Err(DecodeError::Truncated { needed: 2 }));
    assert!(decode(&[0x00]).is_ok());
}

#[test]
fn decode_every_opcode_with_padding() {
    let pad = [0u8; 3];
    for code in 0..=255u8 {
        let mut bytes = vec![code];
        bytes.extend_from_slice(&pad);
        let i = decode(&bytes).unwrap();
        if code == 0xCB {
            assert_eq!(i.opcode.prefix, Prefix::Cb);
        } else {
            assert_eq!(i.opcode.code, code);
        }
        let mut bytes = vec![0xCB, code];
        bytes.extend_from_slice(&pad);
        assert_eq!(decode(&bytes).unwrap().opcode.code, code);
    }
}
