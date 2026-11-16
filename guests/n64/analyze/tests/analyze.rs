//! Discovery over synthetic ROMs: small MIPS sequences assembled here and
//! placed in a ROM built by `relift_n64_formats::synth`. No game data.

use relift_n64_analyze::{
    Boot, Options, Origin, Report, RspOrigin, SegmentOrigin, Source, UnresolvedKind, analyze_with,
};
use relift_n64_formats::Cic;
use relift_n64_formats::compress::Format;
use relift_n64_formats::synth::{self, Spec};

const ZERO: u32 = 0;
const AT: u32 = 1;
const V0: u32 = 2;
const A0: u32 = 4;
const A1: u32 = 5;
const A2: u32 = 6;
const T0: u32 = 8;
const T1: u32 = 9;
const T2: u32 = 10;
const K0: u32 = 26;
const SP: u32 = 29;
const RA: u32 = 31;

fn i(op: u32, rs: u32, rt: u32, imm: u16) -> u32 {
    op << 26 | rs << 21 | rt << 16 | u32::from(imm)
}
fn r(rs: u32, rt: u32, rd: u32, sa: u32, funct: u32) -> u32 {
    rs << 21 | rt << 16 | rd << 11 | sa << 6 | funct
}
fn lui(rt: u32, v: u16) -> u32 {
    i(0x0F, 0, rt, v)
}
fn addiu(rt: u32, rs: u32, v: i16) -> u32 {
    i(0x09, rs, rt, v as u16)
}
fn ori(rt: u32, rs: u32, v: u16) -> u32 {
    i(0x0D, rs, rt, v)
}
fn sltiu(rt: u32, rs: u32, v: u16) -> u32 {
    i(0x0B, rs, rt, v)
}
fn lw(rt: u32, off: i16, base: u32) -> u32 {
    i(0x23, base, rt, off as u16)
}
fn sw(rt: u32, off: i16, base: u32) -> u32 {
    i(0x2B, base, rt, off as u16)
}
fn beq(rs: u32, rt: u32, off: i16) -> u32 {
    i(0x04, rs, rt, off as u16)
}
fn jal(t: u32) -> u32 {
    3 << 26 | (t >> 2 & 0x03FF_FFFF)
}
fn jr(rs: u32) -> u32 {
    r(rs, 0, 0, 0, 8)
}
fn jalr(rs: u32) -> u32 {
    r(rs, 0, RA, 0, 9)
}
fn addu(rd: u32, rs: u32, rt: u32) -> u32 {
    r(rs, rt, rd, 0, 0x21)
}
fn and(rd: u32, rs: u32, rt: u32) -> u32 {
    r(rs, rt, rd, 0, 0x24)
}
fn sll(rd: u32, rt: u32, sa: u32) -> u32 {
    r(0, rt, rd, sa, 0)
}
fn mtc0(rt: u32, rd: u32) -> u32 {
    0x10 << 26 | 4 << 21 | rt << 16 | rd << 11
}
fn eret() -> u32 {
    0x4200_0018
}
const NOP: u32 = 0;

/// `lui reg, hi; addiu reg, reg, lo` for a 32-bit constant.
fn li(reg: u32, v: u32) -> [u32; 2] {
    let lo = v as u16 as i16;
    let hi = (v.wrapping_sub(lo as i32 as u32) >> 16) as u16;
    [lui(reg, hi), addiu(reg, reg, lo)]
}

/// A ROM of `len` bytes with zeros after the header and boot area, entry
/// point `entry` in the header.
fn rom(entry: u32, len: usize) -> Vec<u8> {
    let mut b = synth::rom(&Spec {
        entry,
        len,
        ..Spec::default()
    });
    b[0x1000..].fill(0);
    b
}

fn put(rom: &mut [u8], off: usize, words: &[u32]) {
    for (k, w) in words.iter().enumerate() {
        rom[off + 4 * k..off + 4 * k + 4].copy_from_slice(&w.to_be_bytes());
    }
}

fn run(rom: &[u8]) -> Report {
    analyze_with(
        rom,
        Options {
            cic: Some(Cic::C6102),
        },
    )
    .expect("the analysis runs")
}

/// Whether some unit holds the word at `addr` of a segment linked at a
/// known address.
fn found(rep: &Report, addr: u32) -> bool {
    rep.units.iter().any(|u| {
        u.link.is_some_and(|l| {
            (l & 0x1FFF_FFFF..(l & 0x1FFF_FFFF) + u.len).contains(&(addr & 0x1FFF_FFFF))
        })
    })
}

fn function(rep: &Report, addr: u32) -> Option<Origin> {
    rep.functions
        .iter()
        .find(|f| f.addr == addr)
        .map(|f| f.origin)
}

const ENTRY: u32 = 0x8000_0400;
const BOOT: usize = 0x1000;

/// The ROM offset of a boot image address.
fn at(addr: u32) -> usize {
    BOOT + (addr - ENTRY) as usize
}

#[test]
fn calls_return_and_a_callee_that_never_returns_ends_the_caller() {
    let mut b = rom(ENTRY, 0x10_2000);
    let f = ENTRY + 0x100;
    let g = ENTRY + 0x200;
    put(
        &mut b,
        at(ENTRY),
        &[
            jal(f),
            NOP,
            addiu(T0, ZERO, 1),
            jal(g),
            NOP,
            addiu(T1, ZERO, 2),
        ],
    );
    put(&mut b, at(f), &[addiu(V0, ZERO, 7), jr(RA), NOP]);
    put(&mut b, at(g), &[beq(ZERO, ZERO, -1), NOP]);
    let rep = run(&b);
    assert_eq!(rep.entry, ENTRY);
    assert_eq!(rep.boot, Boot::Standard(Cic::C6102));
    assert_eq!(function(&rep, ENTRY), Some(Origin::Entry));
    assert_eq!(function(&rep, f), Some(Origin::Call));
    assert_eq!(function(&rep, g), Some(Origin::Call));
    assert!(found(&rep, ENTRY + 8), "the code after a returning call");
    assert!(found(&rep, ENTRY + 12));
    assert!(
        !found(&rep, ENTRY + 20),
        "the code after a call that never returns"
    );
    assert!(found(&rep, f + 8), "the delay slot of the return");
}

#[test]
fn the_boot_variant_moves_the_entry_point() {
    let mut b = rom(ENTRY + 0x10_0000, 0x10_2000);
    put(&mut b, at(ENTRY), &[jr(RA), NOP]);
    let rep = analyze_with(
        &b,
        Options {
            cic: Some(Cic::C6103),
        },
    )
    .expect("the analysis runs");
    assert_eq!(rep.entry, ENTRY);
    assert_eq!(rep.segments[0].link, Some(ENTRY));
    assert_eq!(rep.segments[0].offset, 0x1000);
    assert_eq!(rep.boot, Boot::Standard(Cic::C6103));
}

/// `lui t0, 0xA460`, then the PI DMA registers from `dram`, `cart` and
/// `len - 1` in `t1`.
fn pi_dma(dram: u32, cart: u32, len: u32) -> Vec<u32> {
    let mut v = vec![lui(T0, 0xA460)];
    v.extend(li(T1, dram));
    v.push(sw(T1, 0, T0));
    v.extend(li(T1, cart));
    v.push(sw(T1, 4, T0));
    v.extend(li(T1, len - 1));
    v.push(sw(T1, 0xC, T0));
    v
}

#[test]
fn a_pi_dma_with_constants_loads_code_that_is_then_called() {
    let mut b = rom(ENTRY, 0x20_0000);
    let mut code = pi_dma(0x0020_0000, 0x1018_0000, 0x100);
    code.extend([jal(0x8020_0000), NOP, jr(RA), NOP]);
    put(&mut b, at(ENTRY), &code);
    put(
        &mut b,
        0x18_0000,
        &[addiu(V0, ZERO, 3), jal(0x8020_0010), NOP, jr(RA), NOP],
    );
    put(&mut b, 0x18_0010 + 4, &[jr(RA), NOP]);
    let rep = run(&b);
    assert_eq!(rep.pi_dma.len(), 1);
    assert_eq!(rep.pi_dma[0].cart, 0x1018_0000);
    assert_eq!(rep.pi_dma[0].dram, 0x0020_0000);
    assert_eq!(rep.pi_dma[0].len, 0x100);
    let seg = rep
        .segments
        .iter()
        .position(|s| matches!(s.origin, SegmentOrigin::PiDma { .. }))
        .expect("a segment from the DMA");
    assert_eq!(rep.segments[seg].offset, 0x18_0000);
    assert_eq!(function(&rep, 0x8020_0000), Some(Origin::Call));
    assert_eq!(function(&rep, 0x8020_0010), Some(Origin::Call));
    assert!(found(&rep, 0x8020_0018));
    assert!(rep.stats.rounds >= 2);
}

#[test]
fn a_dma_through_a_wrapper_resolves_at_its_call() {
    let mut b = rom(ENTRY, 0x20_0000);
    let wrap = ENTRY + 0x400;
    let mut code = vec![];
    code.extend(li(A0, 0x8030_0000));
    code.extend(li(A1, 0x1019_0000));
    code.extend([jal(wrap), addiu(A2, ZERO, 0x40)]);
    code.extend([jal(0x8030_0000), NOP, jr(RA), NOP]);
    put(&mut b, at(ENTRY), &code);
    put(
        &mut b,
        at(wrap),
        &[
            lui(T0, 0xA460),
            lui(T1, 0x1FFF),
            ori(T1, T1, 0xFFFF),
            and(T1, A0, T1),
            sw(T1, 0, T0),
            sw(A1, 4, T0),
            addiu(A2, A2, -1),
            jr(RA),
            sw(A2, 0xC, T0),
        ],
    );
    put(&mut b, 0x19_0000, &[jr(RA), addiu(V0, ZERO, 9)]);
    let rep = run(&b);
    let d = rep
        .pi_dma
        .iter()
        .find(|d| d.cart == 0x1019_0000)
        .expect("the DMA");
    assert_eq!((d.dram, d.len), (0x0030_0000, 0x40));
    assert!(found(&rep, 0x8030_0004));
}

#[test]
fn a_jump_table_follows_its_entries() {
    let mut b = rom(ENTRY, 0x10_2000);
    let table = ENTRY + 0x800;
    let cases = [ENTRY + 0x100, ENTRY + 0x140, ENTRY + 0x180];
    let mut code = vec![sltiu(AT, A0, 3), beq(AT, ZERO, 8), sll(T0, A0, 2)];
    code.extend(li(T1, table).iter().copied().take(1));
    code.extend([
        addu(T1, T1, T0),
        lw(T1, (table & 0xFFFF) as u16 as i16, T1),
        jr(T1),
        NOP,
    ]);
    code.extend([jr(RA), NOP]);
    put(&mut b, at(ENTRY), &code);
    for (k, c) in cases.iter().enumerate() {
        put(&mut b, at(*c), &[addiu(V0, ZERO, k as i16), jr(RA), NOP]);
    }
    put(&mut b, at(table), &cases);
    let rep = run(&b);
    for c in cases {
        assert!(found(&rep, c), "case {c:#x}");
    }
}

#[test]
fn eret_goes_to_the_address_written_to_epc() {
    let mut b = rom(ENTRY, 0x10_2000);
    let t = ENTRY + 0x300;
    let mut code = li(K0, t).to_vec();
    code.extend([mtc0(K0, 14), NOP, eret()]);
    put(&mut b, at(ENTRY), &code);
    put(&mut b, at(t), &[addiu(V0, ZERO, 1), jr(RA), NOP]);
    let rep = run(&b);
    assert!(found(&rep, t + 4));
}

#[test]
fn stored_code_addresses_are_roots_and_load_bases_are_not() {
    let mut b = rom(ENTRY, 0x10_2000);
    let cb = ENTRY + 0x200;
    let data = ENTRY + 0x300;
    let mut code = li(T0, cb).to_vec();
    code.extend(li(T1, 0x8020_0000));
    code.push(sw(T0, 0, T1));
    code.extend(li(T2, data));
    code.extend([lw(V0, 0, T2), jr(RA), NOP]);
    put(&mut b, at(ENTRY), &code);
    put(&mut b, at(cb), &[addiu(V0, ZERO, 5), jr(RA), NOP]);
    put(&mut b, at(data), &[addiu(V0, ZERO, 6), jr(RA), NOP]);
    let rep = run(&b);
    assert_eq!(function(&rep, cb), Some(Origin::AddressTaken));
    assert_eq!(function(&rep, data), None);
}

#[test]
fn unknown_targets_are_listed() {
    let mut b = rom(ENTRY, 0x10_2000);
    put(&mut b, at(ENTRY), &[lw(T0, 0, A0), jr(T0), NOP]);
    let rep = run(&b);
    assert!(
        rep.unresolved
            .iter()
            .any(|u| u.site == ENTRY + 4 && u.kind == UnresolvedKind::Jump)
    );
}

#[test]
fn an_exception_trampoline_roots_its_handler() {
    let mut b = rom(ENTRY, 0x10_2000);
    let h = ENTRY + 0x500;
    put(&mut b, at(ENTRY), &[jr(RA), NOP]);
    let mut stub = li(K0, h).to_vec();
    stub.extend([jr(K0), NOP]);
    put(&mut b, at(ENTRY + 0x100), &stub);
    put(&mut b, at(h), &[addiu(V0, ZERO, 1), eret()]);
    let rep = run(&b);
    assert_eq!(function(&rep, h), Some(Origin::Exception));
    let off = at(ENTRY + 0x100) as u32;
    for v in [0x8000_0000, 0x8000_0080, 0x8000_0100, 0x8000_0180] {
        let entry = if v == 0x8000_0100 { 0xA000_0100 } else { v };
        assert_eq!(function(&rep, entry), Some(Origin::Exception), "{v:08x}");
        assert!(
            rep.units
                .iter()
                .any(|u| u.link == Some(v) && u.offset == off && u.len == 16),
            "the trampoline is a unit linked at {v:08x}"
        );
    }
}

/// A run of functions linked at `base` that call each other: each has a
/// frame, filler, a call to the next one and a return. The filler length
/// follows a pseudo-random sequence, as function sizes do, so that no
/// spacing between starts repeats often enough to outvote the link
/// address.
fn overlay(base: u32, n: usize) -> Vec<u32> {
    let fill = |k: usize| ((k * k * 7 + k * 3 + 5) % 13) as u32;
    let mut starts = Vec::new();
    let mut at = 0u32;
    for k in 0..n {
        starts.push(base + at);
        at += 4 * (7 + fill(k));
    }
    let mut out = Vec::new();
    for k in 0..n {
        let next = starts[(k + 1) % n];
        out.extend([addiu(SP, SP, -24), sw(RA, 20, SP)]);
        for f in 0..fill(k) {
            out.push(addiu(T0, T0, f as i16 + 1));
        }
        out.extend([jal(next), NOP, lw(RA, 20, SP), jr(RA), addiu(SP, SP, 24)]);
    }
    out
}

#[test]
fn a_code_region_elsewhere_in_the_rom_is_linked_by_its_own_calls() {
    let mut b = rom(ENTRY, 0x40_0000);
    put(&mut b, at(ENTRY), &[jr(RA), NOP]);
    let base = 0x8034_5600;
    let ovl = overlay(base, 24);
    put(&mut b, 0x30_0000, &ovl);
    let rep = run(&b);
    let seg = rep
        .segments
        .iter()
        .find(|s| matches!(s.origin, SegmentOrigin::Region { .. }) && s.source == Source::Rom)
        .expect("a region segment");
    let lo = seg.offset;
    assert!(lo <= 0x30_0000 && 0x30_0000 < lo + seg.len, "{seg:?}");
    assert_eq!(seg.link, Some(base - (0x30_0000 - lo)), "{seg:?}");
    assert!(found(&rep, base + 4));
    assert_eq!(function(&rep, base), Some(Origin::Prologue));
    let u = rep
        .units
        .iter()
        .find(|u| u.offset <= 0x30_0000 && 0x30_0000 < u.offset + u.len);
    assert_eq!(
        u.map(|u| u.reloc),
        Some(0),
        "linked inside RDRAM: no relocations"
    );
}

#[test]
fn code_in_the_boot_image_linked_elsewhere_gets_its_own_segment() {
    let mut b = rom(ENTRY, 0x10_2000);
    put(&mut b, at(ENTRY), &[jr(RA), NOP]);
    let base = 0x8034_5600;
    put(&mut b, 0x8_0000, &overlay(base, 24));
    let rep = run(&b);
    let seg = rep
        .segments
        .iter()
        .find(|s| {
            matches!(s.origin, SegmentOrigin::Region { .. })
                && s.offset <= 0x8_0000
                && 0x8_0000 < s.offset + s.len
        })
        .expect("a region segment inside the boot image");
    assert_eq!(seg.link, Some(base - (0x8_0000 - seg.offset)), "{seg:?}");
    assert_eq!(function(&rep, base), Some(Origin::Prologue));
}

#[test]
fn code_linked_outside_rdram_has_its_relocation_fields_masked() {
    let base = 0x8600_0000;
    let ovl = overlay(base, 24);
    let mut moved = overlay(0x8700_0000, 24);
    let mut hashes = Vec::new();
    for words in [&ovl, &mut moved] {
        let mut b = rom(ENTRY, 0x40_0000);
        put(&mut b, at(ENTRY), &[jr(RA), NOP]);
        put(&mut b, 0x30_0000, words);
        let rep = run(&b);
        let u = rep
            .units
            .iter()
            .find(|u| {
                u.source == Source::Rom && u.offset <= 0x30_0000 && 0x30_0000 < u.offset + u.len
            })
            .expect("a unit of the region")
            .to_owned();
        assert_eq!(u.reloc, 24, "every JAL points into the region");
        hashes.push((u.offset, u.len, u.hash));
    }
    assert_eq!(
        hashes[0], hashes[1],
        "the masked hash does not depend on the link address"
    );
}

#[test]
fn code_in_a_compressed_stream_is_found() {
    let mut b = rom(ENTRY, 0x40_0000);
    put(&mut b, at(ENTRY), &[jr(RA), NOP]);
    let ovl = overlay(0x8034_5600, 24);
    let raw: Vec<u8> = ovl.iter().flat_map(|w| w.to_be_bytes()).collect();
    for (k, f) in [Format::Yay0, Format::Yaz0, Format::Mio0]
        .into_iter()
        .enumerate()
    {
        let enc = synth::encode(f, &raw);
        let off = 0x20_0000 + 0x4_0000 * k;
        b[off..off + enc.len()].copy_from_slice(&enc);
    }
    let rep = run(&b);
    assert_eq!(rep.streams.len(), 3);
    for n in 0..3 {
        let u = rep
            .units
            .iter()
            .filter(|u| u.source == Source::Stream(n))
            .map(|u| u.len)
            .sum::<u32>();
        assert!(
            u >= 4 * ovl.len() as u32 - 8,
            "stream {n}: {u} bytes of code"
        );
    }
}

#[test]
fn rsp_code_is_listed_from_an_sp_dma_and_from_the_scan() {
    let mut b = rom(ENTRY, 0x10_2000);
    let ucode = ENTRY + 0x1000;
    let mut code = vec![lui(T0, 0xA404)];
    code.extend(li(T1, 0x1000));
    code.push(sw(T1, 0, T0));
    code.extend(li(T1, ucode & 0x1FFF_FFFF));
    code.push(sw(T1, 4, T0));
    code.extend(li(T1, 0x0FF));
    code.extend([sw(T1, 8, T0), jr(RA), NOP]);
    put(&mut b, at(ENTRY), &code);
    let vmulf = |vd: u32, vs: u32, vt: u32| 0x4A00_0000 | vt << 16 | vs << 11 | vd << 6;
    let body: Vec<u32> = (0..64)
        .map(|k| {
            if k % 2 == 0 {
                vmulf(k % 32, 1, 2)
            } else {
                addiu(T0, T0, 1)
            }
        })
        .collect();
    put(&mut b, at(ucode), &body);
    let rep = run(&b);
    let dma = rep
        .rsp
        .iter()
        .find(|i| matches!(i.origin, RspOrigin::SpDma { .. }))
        .expect("the IMEM upload");
    assert_eq!(
        (dma.source, dma.offset, dma.len, dma.imem),
        (Source::Rom, at(ucode) as u32, 0x100, Some(0))
    );
    let scan = rep
        .rsp
        .iter()
        .find(|i| i.origin == RspOrigin::Scan)
        .expect("the scan");
    assert!(scan.offset <= at(ucode) as u32 && scan.offset + scan.len >= at(ucode) as u32 + 0x100);
}

#[test]
fn an_elf_image_is_booted_by_its_segments() {
    let mut b = rom(ENTRY, 0x10_2000);
    let elf = 0x1100;
    let text: u32 = 0x8004_0000;
    let mut h = vec![0u8; 0x54];
    h[..4].copy_from_slice(b"\x7fELF");
    h[4] = 1;
    h[5] = 2;
    h[6] = 1;
    h[0x10..0x12].copy_from_slice(&2u16.to_be_bytes());
    h[0x12..0x14].copy_from_slice(&8u16.to_be_bytes());
    h[0x18..0x1C].copy_from_slice(&text.to_be_bytes());
    h[0x1C..0x20].copy_from_slice(&0x34u32.to_be_bytes());
    h[0x2A..0x2C].copy_from_slice(&0x20u16.to_be_bytes());
    h[0x2C..0x2E].copy_from_slice(&1u16.to_be_bytes());
    let ph = 0x34;
    h[ph..ph + 4].copy_from_slice(&1u32.to_be_bytes());
    h[ph + 4..ph + 8].copy_from_slice(&0x100u32.to_be_bytes());
    h[ph + 8..ph + 12].copy_from_slice(&text.to_be_bytes());
    h[ph + 0x10..ph + 0x14].copy_from_slice(&0x40u32.to_be_bytes());
    h[ph + 0x14..ph + 0x18].copy_from_slice(&0x80u32.to_be_bytes());
    b[elf..elf + h.len()].copy_from_slice(&h);
    put(
        &mut b,
        elf + 0x100,
        &[
            addiu(V0, ZERO, 1),
            jal(text + 0x10),
            NOP,
            jr(RA),
            NOP,
            jr(RA),
            NOP,
        ],
    );
    let rep = analyze_with(&b, Options::default()).expect("the analysis runs");
    assert_eq!(rep.boot, Boot::Elf(elf as u32));
    assert_eq!(rep.entry, text);
    assert_eq!(rep.segments[0].origin, SegmentOrigin::Elf);
    assert_eq!(rep.segments[0].offset, elf as u32 + 0x100);
    assert_eq!(function(&rep, text + 0x10), Some(Origin::Call));
}

#[test]
fn a_function_pointer_call_through_a_table_reaches_each_entry() {
    let mut b = rom(ENTRY, 0x10_2000);
    let table = ENTRY + 0x700;
    let fs = [ENTRY + 0x100, ENTRY + 0x180];
    let mut code = vec![sll(T0, A0, 2)];
    code.push(lui(T1, (table >> 16) as u16));
    code.extend([
        addu(T1, T1, T0),
        lw(T1, (table & 0xFFFF) as u16 as i16, T1),
        jalr(T1),
        NOP,
        jr(RA),
        NOP,
    ]);
    put(&mut b, at(ENTRY), &code);
    for f in fs {
        put(&mut b, at(f), &[addiu(V0, ZERO, 2), jr(RA), NOP]);
    }
    put(&mut b, at(table), &fs);
    let rep = run(&b);
    for f in fs {
        assert_eq!(function(&rep, f), Some(Origin::Call), "{f:#x}");
    }
    assert!(found(&rep, ENTRY + 24), "the code after the call");
}

#[test]
fn a_root_that_reaches_an_undecodable_word_is_rejected() {
    let mut b = rom(ENTRY, 0x10_2000);
    let bad = ENTRY + 0x200;
    let mut code = li(T0, bad).to_vec();
    code.extend(li(T1, 0x8020_0000));
    code.extend([sw(T0, 0, T1), jr(RA), NOP]);
    put(&mut b, at(ENTRY), &code);
    put(&mut b, at(bad), &[addiu(V0, ZERO, 1), 0x4C00_0000 | 0x3F]);
    let rep = run(&b);
    assert_eq!(function(&rep, bad), None);
    assert!(rep.stats.rejected >= 1);
}

#[test]
fn scalar_rsp_code_with_cop0_moves_is_found_by_the_scan() {
    let mut b = rom(ENTRY, 0x10_2000);
    put(&mut b, at(ENTRY), &[jr(RA), NOP]);
    let loader = ENTRY + 0x2000;
    let mtc0 = |rt: u32, rd: u32| 0x4080_0000 | rt << 16 | rd << 11;
    let mfc0 = |rt: u32, rd: u32| 0x4000_0000 | rt << 16 | rd << 11;
    let body: Vec<u32> = (0..48)
        .map(|k| match k % 4 {
            0 => mtc0(T0, k % 8),
            1 => mfc0(T1, 6),
            2 => addiu(T0, T0, 8),
            _ => lw(T2, 0x10, T0),
        })
        .chain([0x0000_000D])
        .collect();
    put(&mut b, at(loader), &body);
    let rep = run(&b);
    assert!(
        rep.rsp.iter().any(|i| i.origin == RspOrigin::Scan
            && i.offset <= at(loader) as u32
            && i.offset + i.len >= at(loader) as u32 + 4 * 49),
        "{:?}",
        rep.rsp
    );
}

#[test]
fn a_segment_table_entry_links_an_overlay() {
    let mut b = rom(ENTRY, 0x12_0000);
    put(&mut b, at(ENTRY), &[jr(RA), NOP]);
    let ovl = 0x10_8000;
    let link = 0x8030_0000;
    let mut code = vec![
        addiu(SP, SP, -0x18),
        sw(RA, 0x10, SP),
        jal(link + 0x40),
        NOP,
    ];
    code.extend([lw(RA, 0x10, SP), jr(RA), addiu(SP, SP, 0x18)]);
    code.resize(16, NOP);
    code.extend([
        addiu(SP, SP, -8),
        addiu(V0, ZERO, 1),
        jr(RA),
        addiu(SP, SP, 8),
    ]);
    for k in 0..12 {
        code.push(addiu(T0, T0, k));
    }
    code.extend([jr(RA), NOP]);
    put(&mut b, ovl, &code);
    let len = 4 * code.len() as u32;
    put(&mut b, 0x10_4000, &[ovl as u32, ovl as u32 + len, link]);
    let rep = run(&b);
    assert!(
        rep.segments.iter().any(|s| s.link == Some(link)
            && s.offset == ovl as u32
            && matches!(s.origin, SegmentOrigin::Table { at: 0x10_4000 })),
        "{:?}",
        rep.segments
    );
    assert!(
        rep.units
            .iter()
            .any(|u| u.link == Some(link) && u.offset == ovl as u32),
        "the overlay is code at its table link"
    );
}
