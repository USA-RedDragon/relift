//! CUDA cubins built here (a minimal ELF with hand-encoded instruction
//! words), whole and in fat binaries: one linked program per kernel, with
//! the relocations into the layout applied and nothing carved from inside.

use std::path::PathBuf;

use relift_switch_shaders::carve::BRA_SELF;
use relift_switch_shaders::cubin::{self, EM_CUDA, FATBIN_MAGIC};
use relift_switch_shaders::{DirSource, Options, scan};

fn word(op: u32, form: u32, fields: &[(u32, u32, u64)]) -> [u8; 16] {
    let mut w: u128 = u128::from(op) | (u128::from(form) << 9) | (7u128 << 12);
    for &(lo, len, v) in fields {
        let m = ((1u128 << len) - 1) << lo;
        w = (w & !m) | ((u128::from(v) << lo) & m);
    }
    w.to_le_bytes()
}

fn nop() -> [u8; 16] {
    word(0x118, 4, &[])
}

fn halt() -> Vec<u8> {
    let mut t = BRA_SELF.to_vec();
    t.extend_from_slice(&[0x00, 0x00, 0xc0, 0x0f]);
    t
}

fn section(code: &[[u8; 16]]) -> Vec<u8> {
    let mut b: Vec<u8> = code.iter().flatten().copied().collect();
    b.extend(halt());
    while !b.len().is_multiple_of(0x80) {
        b.extend(nop());
    }
    b
}

/// `k`: MOV R20 (return address), MOV R21, CALL.ABS.NOINC f, CALL.ABS.NOINC
/// ext (undefined), EXIT. `f`: RET.ABS.NODEC R20.
fn sections() -> (Vec<u8>, Vec<u8>) {
    let mov = |r: u64| word(0x002, 4, &[(16, 8, r), (72, 4, 0xf)]);
    let call = word(0x143, 4, &[(86, 1, 1), (87, 3, 7)]);
    let k = section(&[mov(20), mov(21), call, call, word(0x14d, 4, &[(87, 3, 7)])]);
    let f = section(&[word(
        0x150,
        4,
        &[(24, 8, 20), (85, 1, 1), (86, 1, 1), (87, 3, 7)],
    )]);
    (k, f)
}

struct Sec {
    name: &'static str,
    kind: u32,
    flags: u64,
    data: Vec<u8>,
    link: u32,
    info: u32,
    align: u64,
    entsize: u64,
}

fn sym(name: u32, info: u8, other: u8, shndx: u16) -> Vec<u8> {
    let mut e = Vec::new();
    e.extend(name.to_le_bytes());
    e.extend([info, other]);
    e.extend(shndx.to_le_bytes());
    e.extend([0u8; 16]);
    e
}

fn rela(off: u64, sym: u64, kind: u64, addend: i64) -> Vec<u8> {
    let mut e = Vec::new();
    e.extend(off.to_le_bytes());
    e.extend(((sym << 32) | kind).to_le_bytes());
    e.extend(addend.to_le_bytes());
    e
}

/// A cubin for `sm` with kernel `k` and function `f`.
fn elf(sm: u32) -> Vec<u8> {
    let (k, f) = sections();
    let strtab = b"\0k\0f\0ext\0".to_vec();
    let mut symtab = sym(0, 0, 0, 0);
    symtab.extend(sym(0, 3, 0, 4));
    symtab.extend(sym(1, 0x12, 0x10, 4));
    symtab.extend(sym(3, 0x12, 0, 5));
    symtab.extend(sym(5, 0x10, 0, 0));
    let mut relas = rela(0x00, 1, 56, 0x40);
    relas.extend(rela(0x20, 3, 58, 0));
    relas.extend(rela(0x30, 4, 58, 0));
    let mut secs = vec![
        Sec {
            name: ".shstrtab",
            kind: 3,
            flags: 0,
            data: Vec::new(),
            link: 0,
            info: 0,
            align: 1,
            entsize: 0,
        },
        Sec {
            name: ".strtab",
            kind: 3,
            flags: 0,
            data: strtab,
            link: 0,
            info: 0,
            align: 1,
            entsize: 0,
        },
        Sec {
            name: ".symtab",
            kind: 2,
            flags: 0,
            data: symtab,
            link: 2,
            info: 3,
            align: 8,
            entsize: 24,
        },
        Sec {
            name: ".text.k",
            kind: 1,
            flags: 6,
            data: k,
            link: 3,
            info: 2,
            align: 0x80,
            entsize: 0,
        },
        Sec {
            name: ".text.f",
            kind: 1,
            flags: 6,
            data: f,
            link: 3,
            info: 3,
            align: 0x80,
            entsize: 0,
        },
        Sec {
            name: ".rela.text.k",
            kind: 4,
            flags: 0x40,
            data: relas,
            link: 3,
            info: 4,
            align: 8,
            entsize: 24,
        },
    ];
    let mut shstr = vec![0u8];
    let mut names = Vec::new();
    for s in &secs {
        names.push(shstr.len() as u32);
        shstr.extend(s.name.as_bytes());
        shstr.push(0);
    }
    secs[0].data = shstr;
    let mut b = vec![0u8; 0x40];
    let mut offs = Vec::new();
    for s in &secs {
        while !(b.len() as u64).is_multiple_of(s.align.max(1)) {
            b.push(0);
        }
        offs.push(b.len() as u64);
        b.extend(&s.data);
    }
    while !b.len().is_multiple_of(8) {
        b.push(0);
    }
    let shoff = b.len() as u64;
    b.extend([0u8; 0x40]);
    for (i, s) in secs.iter().enumerate() {
        let mut h = Vec::new();
        h.extend(names[i].to_le_bytes());
        h.extend(s.kind.to_le_bytes());
        h.extend(s.flags.to_le_bytes());
        h.extend(0u64.to_le_bytes());
        h.extend(offs[i].to_le_bytes());
        h.extend((s.data.len() as u64).to_le_bytes());
        h.extend(s.link.to_le_bytes());
        h.extend(s.info.to_le_bytes());
        h.extend(s.align.to_le_bytes());
        h.extend(s.entsize.to_le_bytes());
        b.extend(h);
    }
    b[..4].copy_from_slice(b"\x7fELF");
    b[4..9].copy_from_slice(&[2, 1, 1, 0x33, 8]);
    b[0x10..0x12].copy_from_slice(&2u16.to_le_bytes());
    b[0x12..0x14].copy_from_slice(&EM_CUDA.to_le_bytes());
    b[0x14..0x18].copy_from_slice(&1u32.to_le_bytes());
    b[0x28..0x30].copy_from_slice(&shoff.to_le_bytes());
    b[0x30..0x34].copy_from_slice(&(sm << 8 | 0x4).to_le_bytes());
    b[0x34..0x36].copy_from_slice(&0x40u16.to_le_bytes());
    b[0x3a..0x3c].copy_from_slice(&0x40u16.to_le_bytes());
    b[0x3c..0x3e].copy_from_slice(&(secs.len() as u16 + 1).to_le_bytes());
    b[0x3e..0x40].copy_from_slice(&1u16.to_le_bytes());
    b
}

fn get(line: &[u8], lo: u32, len: u32) -> u64 {
    let w = u128::from_le_bytes(line.try_into().unwrap_or([0; 16]));
    ((w >> lo) & ((1u128 << len) - 1)) as u64
}

fn linked() -> Vec<u8> {
    let (k, f) = sections();
    let mut want = k;
    want.extend(&f[..0x20]);
    want[4..8].copy_from_slice(&0x40u32.to_le_bytes());
    let mut l = u128::from_le_bytes(want[0x20..0x30].try_into().unwrap_or([0; 16]));
    l |= u128::from(0x80u64 / 4) << 34;
    want[0x20..0x30].copy_from_slice(&l.to_le_bytes());
    want
}

#[test]
fn kernel_is_linked_with_its_callee() {
    let e = elf(86);
    let found = cubin::find(&e);
    assert_eq!(found.cubins.len(), 1);
    let c = &found.cubins[0];
    assert_eq!(c.sm, 86);
    assert_eq!(c.kernels.len(), 1, "only the entry is a kernel");
    let k = &c.kernels[0];
    assert_eq!(k.name, "k");
    assert_eq!(get(&k.code[0x00..0x10], 32, 32), 0x40, "return address");
    assert_eq!(get(&k.code[0x20..0x30], 34, 47) * 4, 0x80, "call target");
    assert_eq!(get(&k.code[0x30..0x40], 34, 47), 0, "external call");
    assert_eq!(k.unresolved, vec!["ext".to_string()]);
    assert_eq!(k.code, linked());
    assert!(found.covers(&(0..e.len())));
}

#[test]
fn other_architectures_have_no_kernels() {
    let found = cubin::find(&elf(75));
    assert_eq!(found.cubins.len(), 1);
    assert!(found.cubins[0].kernels.is_empty());
}

fn fatbin(entries: &[(u64, Vec<u8>, usize)]) -> Vec<u8> {
    let mut body = Vec::new();
    for (flags, payload, raw) in entries {
        let mut h = vec![0u8; 0x40];
        h[..2].copy_from_slice(&2u16.to_le_bytes());
        h[2..4].copy_from_slice(&0x101u16.to_le_bytes());
        h[4..8].copy_from_slice(&0x40u32.to_le_bytes());
        let padded = payload.len().next_multiple_of(8);
        h[8..16].copy_from_slice(&(padded as u64).to_le_bytes());
        if *flags != 0 {
            h[0x10..0x14].copy_from_slice(&(payload.len() as u32).to_le_bytes());
            h[0x38..0x40].copy_from_slice(&(*raw as u64).to_le_bytes());
        }
        h[0x1c..0x20].copy_from_slice(&86u32.to_le_bytes());
        h[0x28..0x30].copy_from_slice(&(0x11 | flags).to_le_bytes());
        body.extend(h);
        body.extend(payload);
        body.resize(body.len().next_multiple_of(8), 0);
    }
    let mut b = FATBIN_MAGIC.to_vec();
    b.extend(1u16.to_le_bytes());
    b.extend(16u16.to_le_bytes());
    b.extend((body.len() as u64).to_le_bytes());
    b.extend(body);
    b
}

/// An LZ4 block of literals only.
fn lz4_literals(b: &[u8]) -> Vec<u8> {
    let mut o = vec![0xF0];
    let mut n = b.len() - 15;
    while n >= 255 {
        o.push(255);
        n -= 255;
    }
    o.push(n as u8);
    o.extend(b);
    o
}

#[test]
fn fat_binary_entries_plain_lz4_and_zstd() {
    let e = elf(86);
    let z = zstd::bulk::compress(&e, 3).unwrap_or_default();
    let b = fatbin(&[
        (0, e.clone(), e.len()),
        (0x2000, lz4_literals(&e), e.len()),
        (0x8000, z, e.len()),
    ]);
    let found = cubin::find(&b);
    assert_eq!(found.fatbins, 1);
    assert_eq!(found.cubins.len(), 3, "{:?}", found.errors);
    for c in &found.cubins {
        assert_eq!(c.kernels[0].code, linked());
    }
}

#[test]
fn lz4_matches_copy_back() {
    let src = [0x44, b'a', b'b', b'c', b'd', 0x04, 0x00, 0x10, b'z'];
    assert_eq!(
        cubin::lz4_block(&src, 64).as_deref(),
        Some(&b"abcdabcdabcdz"[..])
    );
    assert_eq!(cubin::lz4_block(&[0x0F, 0x09, 0x00], 64), None);
}

#[test]
fn scan_gives_the_kernel_and_carves_nothing_inside() {
    let dir = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("cubin-scan");
    let _ = std::fs::remove_dir_all(&dir);
    let input = dir.join("in");
    std::fs::create_dir_all(&input).unwrap_or_else(|e| panic!("{e}"));
    let mut blob = vec![0x5Au8; 0x30];
    blob.extend(fatbin(&[(0, elf(86), 0)]));
    relift_cache::write_if_changed(&input.join("module.dat"), &blob)
        .unwrap_or_else(|e| panic!("{e}"));
    relift_cache::write_if_changed(&input.join("k.cubin"), &elf(86))
        .unwrap_or_else(|e| panic!("{e}"));
    let out = dir.join("out");
    let r =
        scan(&DirSource::new(&input), &out, &Options::default()).unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(r.programs.len(), 1, "{:?}", r.programs);
    let (sha, p) = r.programs.iter().next().unwrap_or_else(|| panic!("none"));
    assert_eq!(p.n, 2);
    assert_eq!(p.size, linked().len());
    let bytes = std::fs::read(out.join(format!("{sha}.bin"))).unwrap_or_default();
    assert_eq!(bytes, linked());
    assert_eq!(r.containers.get("cubin"), Some(&2));
    let e = r.errors.get("cubin: not linked into the kernel: ext");
    assert_eq!(e.map(|e| e.count), Some(2));
}
