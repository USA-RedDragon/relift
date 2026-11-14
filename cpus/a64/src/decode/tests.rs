use super::*;

fn enc(word: u32) -> Option<Encoding> {
    decode(word).map(|i| i.encoding)
}

fn named(name: &str) -> Encoding {
    Encoding::all()
        .find(|e| e.name() == name)
        .unwrap_or_else(|| panic!("no encoding {name}"))
}

fn expect(word: u32, name: &str, fields: &[(&str, u32)]) {
    let insn = decode(word).unwrap_or_else(|| panic!("{word:#010x} did not decode"));
    assert_eq!(
        insn.encoding.name(),
        name,
        "word {word:#010x} decoded to the wrong encoding"
    );
    for &(f, v) in fields {
        assert_eq!(insn.get(f), v, "field {f} of {name} in {word:#010x}");
    }
}

#[test]
fn counts_and_names_are_consistent() {
    assert_eq!(Encoding::COUNT, 4429);
    assert_eq!(Encoding::all().count(), Encoding::COUNT);
    for (i, e) in Encoding::all().enumerate() {
        assert_eq!(e as usize, i);
        assert!(!e.name().is_empty());
        assert!(!e.mnemonic().is_empty());
        assert!(!e.path().is_empty());
        assert_eq!(e.path()[0], e.group());
        assert_eq!(e.value() & !e.mask(), 0);
    }
}

#[test]
fn add_immediate() {
    let (sh, imm12, rn, rd) = (1u32, 0xabc, 17, 30);
    let word = 0x9100_0000 | sh << 22 | imm12 << 10 | rn << 5 | rd;
    expect(
        word,
        "ADD_64_addsub_imm",
        &[("sh", sh), ("imm12", imm12), ("Rn", rn), ("Rd", rd)],
    );
    let e = Encoding::ADD_64_addsub_imm;
    assert_eq!(e.mnemonic(), "ADD");
    assert_eq!(e.group(), "dpimm");
    assert!(!e.is_sve_or_sme());
    assert_eq!(
        e.field("Rd"),
        Some(Field {
            name: "Rd",
            lo: 0,
            width: 5
        })
    );
    assert_eq!(e.field("nope"), None);
    assert_eq!(decode(word).unwrap().field("nope"), None);
}

#[test]
#[should_panic(expected = "ADD_64_addsub_imm has no field nope")]
fn get_panics_on_missing_field() {
    decode(0x9100_0000).unwrap().get("nope");
}

#[test]
fn data_processing_immediate() {
    expect(
        0x7100_0000 | 0x20 << 10 | 1 << 5 | 7,
        "SUBS_32S_addsub_imm",
        &[("imm12", 0x20), ("Rn", 1), ("Rd", 7)],
    );
    expect(
        0xf200_0000 | 1 << 22 | 5 << 16 | 9 << 10 | 3 << 5 | 4,
        "ANDS_64S_log_imm",
        &[("N", 1), ("immr", 5), ("imms", 9), ("Rn", 3), ("Rd", 4)],
    );
    expect(
        0xd280_0000 | 2 << 21 | 0xbeef << 5 | 9,
        "MOVZ_64_movewide",
        &[("hw", 2), ("imm16", 0xbeef), ("Rd", 9)],
    );
    expect(
        0x7280_0000 | 1 << 21 | 0x1234 << 5 | 2,
        "MOVK_32_movewide",
        &[("hw", 1), ("imm16", 0x1234), ("Rd", 2)],
    );
    expect(
        0x9000_0000 | 2 << 29 | 0x7_ffff << 5 | 8,
        "ADRP_only_pcreladdr",
        &[("immlo", 2), ("immhi", 0x7_ffff), ("Rd", 8)],
    );
    expect(
        0xd340_0000 | 3 << 16 | 63 << 10 | 1 << 5 | 2,
        "UBFM_64M_bitfield",
        &[("immr", 3), ("imms", 63), ("Rn", 1), ("Rd", 2)],
    );
    expect(
        0x1300_0000 | 3 << 16 | 31 << 10 | 1 << 5 | 2,
        "SBFM_32M_bitfield",
        &[("immr", 3), ("imms", 31)],
    );
}

#[test]
fn branches() {
    expect(
        0x1400_0000 | 0x155_5555,
        "B_only_branch_imm",
        &[("imm26", 0x155_5555)],
    );
    expect(
        0x9400_0000 | 0x3ff_ffff,
        "BL_only_branch_imm",
        &[("imm26", 0x3ff_ffff)],
    );
    expect(
        0x5400_0000 | 0x1234 << 5 | 0xb,
        "B_only_condbranch",
        &[("imm19", 0x1234), ("cond", 0xb)],
    );
    expect(
        0xb400_0000 | 0x77 << 5 | 5,
        "CBZ_64_compbranch",
        &[("imm19", 0x77), ("Rt", 5)],
    );
    expect(
        0x3700_0000 | 1 << 31 | 7 << 19 | 0x1fff << 5 | 3,
        "TBNZ_only_testbranch",
        &[("b5", 1), ("b40", 7), ("imm14", 0x1fff), ("Rt", 3)],
    );
    expect(0xd61f_0000 | 16 << 5, "BR_64_branch_reg", &[("Rn", 16)]);
    expect(0xd63f_0000 | 8 << 5, "BLR_64_branch_reg", &[("Rn", 8)]);
    expect(0xd65f_0000 | 30 << 5, "RET_64R_branch_reg", &[("Rn", 30)]);
}

#[test]
fn loads_and_stores() {
    expect(
        0xf940_0000 | 0xfff << 10 | 3 << 5 | 4,
        "LDR_64_ldst_pos",
        &[("imm12", 0xfff), ("Rn", 3), ("Rt", 4)],
    );
    expect(
        0xb820_0800 | 5 << 16 | 3 << 13 | 1 << 12 | 2 << 5 | 1,
        "STR_32_ldst_regoff",
        &[("Rm", 5), ("option", 3), ("S", 1), ("Rn", 2), ("Rt", 1)],
    );
    expect(
        0xa940_0000 | 0x7f << 15 | 2 << 10 | 3 << 5 | 4,
        "LDP_64_ldstpair_off",
        &[("imm7", 0x7f), ("Rt2", 2), ("Rn", 3), ("Rt", 4)],
    );
    expect(
        0xa980_0000 | 0x40 << 15 | 2 << 10 | 31 << 5 | 4,
        "STP_64_ldstpair_pre",
        &[("imm7", 0x40), ("Rn", 31)],
    );
    expect(
        0x3940_0000 | 9 << 10 | 1 << 5 | 2,
        "LDRB_32_ldst_pos",
        &[("imm12", 9)],
    );
    expect(
        0xb980_0000 | 9 << 10 | 1 << 5 | 2,
        "LDRSW_64_ldst_pos",
        &[("imm12", 9)],
    );
    expect(
        0xc85f_fc00 | 6 << 5 | 7,
        "LDAXR_LR64_ldstexclr",
        &[("Rn", 6), ("Rt", 7)],
    );
    expect(
        0xc800_fc00 | 5 << 16 | 6 << 5 | 7,
        "STLXR_SR64_ldstexclr",
        &[("Rs", 5), ("Rn", 6), ("Rt", 7)],
    );
}

#[test]
fn register_data_processing_and_system() {
    expect(
        0x6b00_0000 | 1 << 22 | 4 << 16 | 33 << 10 | 2 << 5 | 3,
        "SUBS_32_addsub_shift",
        &[("shift", 1), ("Rm", 4), ("imm6", 33), ("Rn", 2), ("Rd", 3)],
    );
    expect(
        0x9a80_0000 | 4 << 16 | 0xc << 12 | 2 << 5 | 3,
        "CSEL_64_condsel",
        &[("Rm", 4), ("cond", 0xc)],
    );
    expect(
        0x1a80_0400 | 4 << 16 | 0x3 << 12,
        "CSINC_32_condsel",
        &[("cond", 3)],
    );
    expect(
        0x9b00_0000 | 4 << 16 | 5 << 10 | 2 << 5 | 3,
        "MADD_64A_dp_3src",
        &[("Rm", 4), ("Ra", 5)],
    );
    expect(
        0x9ac0_0800 | 4 << 16 | 2 << 5 | 3,
        "UDIV_64_dp_2src",
        &[("Rm", 4)],
    );
    expect(
        0x1ac0_0c00 | 4 << 16 | 2 << 5 | 3,
        "SDIV_32_dp_2src",
        &[("Rm", 4)],
    );
    expect(0xd503_305f | 0xf << 8, "CLREX_BN_barriers", &[("CRm", 0xf)]);
    expect(0xd503_201f, "NOP_HI_hints", &[]);
}

#[test]
fn field_constraints_pick_the_other_leaf() {
    let base = 0x3c20_0800u32 | 5 << 16 | 1 << 5 | 2;
    expect(base | 0b010 << 13, "STR_B_ldst_regoff", &[("option", 2)]);
    expect(base | 0b011 << 13, "STR_BL_ldst_regoff", &[("option", 3)]);
    assert!(!Encoding::STR_B_ldst_regoff.matches(base | 0b011 << 13));
    assert!(Encoding::STR_BL_ldst_regoff.matches(base | 0b011 << 13));
    assert_eq!(
        matching(base | 0b011 << 13),
        vec![Encoding::STR_BL_ldst_regoff]
    );
}

#[test]
fn unallocated_and_udf() {
    assert_eq!(enc(0x0000_0000), Some(Encoding::UDF_only_perm_undef));
    assert_eq!(enc(0x0000_ffff), Some(Encoding::UDF_only_perm_undef));
    assert_eq!(enc(0x0001_0000), None);
    assert_eq!(enc(0x0200_0000), None);
    assert_eq!(enc(0xffff_ffff), None);
}

#[test]
fn sve_words() {
    let insn = decode(0x0420_0000 | 3 << 16 | 2 << 5 | 1).unwrap();
    assert!(insn.encoding.is_sve_or_sme(), "{}", insn.encoding.name());
    assert_eq!(insn.encoding.group(), "sve");
    assert_eq!(insn.encoding.mnemonic(), "ADD");
}

#[test]
fn every_encoding_decodes_consistently() {
    let mut state = 0x9e37_79b9_7f4a_7c15u64;
    let mut next = move || {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        (state >> 16) as u32
    };
    let mut violations = Vec::new();
    let mut matched = 0usize;
    for e in Encoding::all() {
        for sample in 0..8 {
            let free = if sample == 0 { 0 } else { next() & !e.mask() };
            let word = e.value() | free;
            if !e.matches(word) {
                continue;
            }
            matched += 1;
            let got = decode(word).expect("a matching word must decode");
            let g = got.encoding;
            let ok = g == e
                || (g.mask() & e.mask() == e.mask()
                    && g.mask().count_ones() > e.mask().count_ones());
            if !ok {
                violations.push(format!("{} {word:#010x} -> {}", e.name(), g.name()));
            }
        }
    }
    assert!(
        violations.is_empty(),
        "{} violations: {:?}",
        violations.len(),
        &violations[..violations.len().min(10)]
    );
    assert!(matched > Encoding::COUNT * 6);
}

#[test]
fn encodings_without_ties_among_matches() {
    let mut state = 0x1234_5678_9abc_def1u64;
    let mut next = move || {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        (state >> 16) as u32
    };
    for _ in 0..200_000 {
        let word = next();
        let all = matching(word);
        if let [a, b, ..] = all[..] {
            assert!(
                a.mask().count_ones() >= b.mask().count_ones(),
                "{word:#010x}"
            );
        }
        assert_eq!(decode(word).map(|i| i.encoding), all.first().copied());
    }
}

#[test]
fn named_lookup_helper_finds_encodings() {
    assert_eq!(named("NOP_HI_hints"), Encoding::NOP_HI_hints);
}
