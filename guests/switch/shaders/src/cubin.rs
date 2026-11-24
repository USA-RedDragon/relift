//! CUDA modules: ELF cubins (`e_machine` 190) found whole in a buffer or
//! inside fat binaries (`0xBA55ED50`, entries stored plain, LZ4 or zstd).
//!
//! A cubin keeps every function in its own `.text.<name>` section and
//! fills each absolute code address (a `CALL.ABS` target, a return address
//! moved into a register) through a relocation at load time, so carving
//! its bytes gives one fragment per function with unrelocated targets.
//! Instead each kernel becomes one program: a function symbol marked as an
//! entry that has its own `.nv.info.<name>` attributes, which a launch
//! reads. The driver's own entry points the CUDA library carries in cubins
//! (its trap handler, `generic_entry`, `entry_exit`, `cg_entry`,
//! `tex_entry`, the continuation handlers and the syscall stubs) are marked
//! as entries too but have no attributes and no parameter bank: no launch
//! starts them, and they are not programs. A kernel is its section at address 0, then every code section its code
//! reaches through relocations and every one whose address loaded data
//! holds (function pointer tables), each at its alignment, with every
//! relocation into that layout applied. References to anything else (undefined symbols the
//! driver provides, data) stay as the cubin has them and are named in
//! [`Kernel::unresolved`].

use std::collections::BTreeSet;
use std::ops::Range;

use crate::zst;

/// The ELF machine number of CUDA cubins.
pub const EM_CUDA: u16 = 190;
/// The magic of a fat binary, little-endian.
pub const FATBIN_MAGIC: [u8; 4] = [0x50, 0xED, 0x55, 0xBA];
/// The SM versions whose kernels become programs: the console's GPU runs
/// SM88 cubins (a game's own fat binaries hold only those), which use the
/// SM86 instruction set; SM86 and SM87 cubins of the CUDA runtime are kept
/// for the same reason.
pub const SMS: [u32; 3] = [86, 87, 88];

const ELF_MAGIC: [u8; 4] = [0x7F, b'E', b'L', b'F'];
const SHT_SYMTAB: u32 = 2;
const SHT_RELA: u32 = 4;
const SHT_NOBITS: u32 = 8;
const SHT_REL: u32 = 9;
const SHF_ALLOC: u64 = 2;
const SHF_EXECINSTR: u64 = 4;
const STT_FUNC: u8 = 2;
const STO_CUDA_ENTRY: u8 = 0x10;
const R_CUDA_ABS32_LO_32: u32 = 56;
const R_CUDA_ABS32_HI_32: u32 = 57;
const R_CUDA_ABS47_34: u32 = 58;
const FATBIN_ELF: u16 = 2;
const FLAG_LZ4: u64 = 0x2000;
const FLAG_ZSTD: u64 = 0x8000;
const MAX_ELF: u64 = 1 << 30;
const LINE: usize = 16;
const NOP: [u8; 16] = [0x18, 0x79, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0x0F, 0];

/// One kernel linked into a program.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Kernel {
    /// The symbol name.
    pub name: String,
    /// The code: the kernel's section at 0, then its callees.
    pub code: Vec<u8>,
    /// Symbols the code refers to that are not in the layout.
    pub unresolved: Vec<String>,
    /// Constant bank words the loader fills with the address of a symbol
    /// the module does not define (a function of the CUDA driver, such as
    /// `__assertfail`).
    pub externs: Vec<Extern>,
    /// Bytes of static shared memory: the size of the kernel's
    /// `.nv.shared.<name>` section, 0 without one. A launch gives the kernel
    /// these plus its dynamic shared bytes; the translation's own count of
    /// constant shared offsets is no size (guarded and register-indexed
    /// accesses reach past it).
    pub shared_bytes: u32,
}

/// A constant bank word that holds the address of an undefined symbol.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Extern {
    /// The bank (`.nv.constant<bank>`).
    pub bank: u32,
    /// The byte offset of the 64-bit address in the bank.
    pub offset: u32,
    /// The symbol.
    pub name: String,
}

/// One cubin.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Cubin {
    /// Where it was found: an offset in the buffer, with the fat binary
    /// entry when it came from one.
    pub at: String,
    /// The SM version from the ELF flags.
    pub sm: u32,
    /// Its kernels (none when `sm` is not in [`SMS`]).
    pub kernels: Vec<Kernel>,
}

/// What [`find`] found in a buffer.
#[derive(Clone, Debug, Default)]
pub struct Found {
    /// Every cubin.
    pub cubins: Vec<Cubin>,
    /// How many fat binaries.
    pub fatbins: u64,
    /// The byte ranges the cubins and fat binaries take; carving skips them.
    pub ranges: Vec<Range<usize>>,
    /// What could not be read, with where.
    pub errors: Vec<(String, String)>,
}

impl Found {
    /// Whether `r` overlaps a cubin or fat binary.
    pub fn covers(&self, r: &Range<usize>) -> bool {
        self.ranges
            .iter()
            .any(|c| c.start < r.end && r.start < c.end)
    }
}

fn u16_at(b: &[u8], at: usize) -> Option<u16> {
    Some(u16::from_le_bytes(
        b.get(at..at.checked_add(2)?)?.try_into().ok()?,
    ))
}

fn u32_at(b: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(
        b.get(at..at.checked_add(4)?)?.try_into().ok()?,
    ))
}

fn u64_at(b: &[u8], at: usize) -> Option<u64> {
    Some(u64::from_le_bytes(
        b.get(at..at.checked_add(8)?)?.try_into().ok()?,
    ))
}

#[derive(Clone, Copy, Debug)]
struct Section {
    name: u32,
    kind: u32,
    flags: u64,
    off: usize,
    size: usize,
    link: u32,
    info: u32,
    align: u64,
}

#[derive(Clone, Copy, Debug)]
struct Sym {
    name: u32,
    info: u8,
    other: u8,
    shndx: u16,
    value: u64,
}

struct Elf<'a> {
    b: &'a [u8],
    sections: Vec<Section>,
    shstr: usize,
}

impl<'a> Elf<'a> {
    /// The ELF at the start of `b` and its extent, if it is a 64-bit
    /// little-endian CUDA one whose tables fit in `b`.
    fn parse(b: &'a [u8]) -> Option<(Elf<'a>, usize)> {
        if !b.starts_with(&ELF_MAGIC) || b.get(4..6)? != [2, 1] || u16_at(b, 0x12)? != EM_CUDA {
            return None;
        }
        let shoff = usize::try_from(u64_at(b, 0x28)?).ok()?;
        let shentsize = usize::from(u16_at(b, 0x3A)?);
        let shnum = usize::from(u16_at(b, 0x3C)?);
        let shstrndx = usize::from(u16_at(b, 0x3E)?);
        if shentsize < 0x40 || shnum == 0 || shstrndx >= shnum {
            return None;
        }
        let table_end = shoff.checked_add(shentsize.checked_mul(shnum)?)?;
        if table_end > b.len() || table_end as u64 > MAX_ELF {
            return None;
        }
        let mut end = table_end.max(0x40);
        let phoff = usize::try_from(u64_at(b, 0x20)?).ok()?;
        let phnum = usize::from(u16_at(b, 0x38)?);
        let phentsize = usize::from(u16_at(b, 0x36)?);
        if phnum > 0 {
            end = end.max(phoff.checked_add(phentsize.checked_mul(phnum)?)?);
        }
        let mut sections = Vec::with_capacity(shnum);
        for i in 0..shnum {
            let s = shoff + i * shentsize;
            let sec = Section {
                name: u32_at(b, s)?,
                kind: u32_at(b, s + 4)?,
                flags: u64_at(b, s + 8)?,
                off: usize::try_from(u64_at(b, s + 0x18)?).ok()?,
                size: usize::try_from(u64_at(b, s + 0x20)?).ok()?,
                link: u32_at(b, s + 0x28)?,
                info: u32_at(b, s + 0x2C)?,
                align: u64_at(b, s + 0x30)?,
            };
            if sec.kind != SHT_NOBITS && i > 0 {
                let e = sec.off.checked_add(sec.size)?;
                if e > b.len() {
                    return None;
                }
                end = end.max(e);
            }
            sections.push(sec);
        }
        let shstr = sections[shstrndx].off;
        Some((Elf { b, sections, shstr }, end))
    }

    fn sm(&self) -> u32 {
        let flags = u32_at(self.b, 0x30).unwrap_or(0);
        if self.b.get(8).is_some_and(|&v| v >= 8) {
            (flags >> 8) & 0xFF
        } else {
            flags & 0xFF
        }
    }

    /// Whether a section is named `name`.
    fn has_section(&self, name: &str) -> bool {
        self.sections
            .iter()
            .any(|s| self.cstr(self.shstr + s.name as usize) == name)
    }

    fn cstr(&self, at: usize) -> String {
        let s = self.b.get(at..).unwrap_or(&[]);
        let n = s.iter().position(|&c| c == 0).unwrap_or(s.len());
        String::from_utf8_lossy(&s[..n]).into_owned()
    }

    fn data(&self, i: usize) -> &'a [u8] {
        let s = self.sections[i];
        self.b.get(s.off..s.off + s.size).unwrap_or(&[])
    }

    fn symbols(&self, i: usize) -> Vec<Sym> {
        let d = self.data(i);
        d.as_chunks::<24>()
            .0
            .iter()
            .map(|e| Sym {
                name: u32_at(e, 0).unwrap_or(0),
                info: e[4],
                other: e[5],
                shndx: u16_at(e, 6).unwrap_or(0),
                value: u64_at(e, 8).unwrap_or(0),
            })
            .collect()
    }

    fn is_code(&self, i: usize) -> bool {
        self.sections
            .get(i)
            .is_some_and(|s| s.flags & SHF_EXECINSTR != 0 && s.kind != SHT_NOBITS)
    }
}

struct Reloc {
    off: usize,
    kind: u32,
    sym: usize,
    addend: i64,
}

/// The relocations of each section, with the symbol table they use.
fn relocs(elf: &Elf) -> Vec<Vec<(usize, Reloc)>> {
    let mut v: Vec<Vec<(usize, Reloc)>> = (0..elf.sections.len()).map(|_| Vec::new()).collect();
    for (i, s) in elf.sections.iter().enumerate() {
        let target = s.info as usize;
        if target >= v.len() || !matches!(s.kind, SHT_REL | SHT_RELA) {
            continue;
        }
        let ent = if s.kind == SHT_RELA { 24 } else { 16 };
        for e in elf.data(i).chunks_exact(ent) {
            let info = u64_at(e, 8).unwrap_or(0);
            v[target].push((
                s.link as usize,
                Reloc {
                    off: u64_at(e, 0).unwrap_or(0) as usize,
                    kind: info as u32,
                    sym: (info >> 32) as usize,
                    addend: if ent == 24 {
                        u64_at(e, 16).unwrap_or(0) as i64
                    } else {
                        0
                    },
                },
            ));
        }
    }
    v
}

/// A NOP line, whatever its scheduling bits.
fn is_nop(line: &[u8]) -> bool {
    line[..2] == NOP[..2] && line[2..12].iter().all(|&x| x == 0)
}

fn put_field(code: &mut [u8], at: usize, lo: u32, width: u32, v: u64) -> bool {
    let Some(line) = code.get_mut(at..at + LINE) else {
        return false;
    };
    let mut w = u128::from_le_bytes(line.try_into().unwrap_or([0; 16]));
    let mask = ((1u128 << width) - 1) << lo;
    w = (w & !mask) | ((u128::from(v) << lo) & mask);
    line.copy_from_slice(&w.to_le_bytes());
    true
}

/// The code sections the relocations of `secs` name through `symtab`.
fn code_refs<'a>(
    elf: &'a Elf,
    rels: &'a [Vec<(usize, Reloc)>],
    symtab: usize,
    syms: &'a [Sym],
    secs: impl Iterator<Item = usize> + 'a,
) -> impl Iterator<Item = usize> + 'a {
    secs.flat_map(move |d| rels[d].iter())
        .filter(move |(tab, _)| *tab == symtab)
        .filter_map(move |(_, r)| syms.get(r.sym).map(|s| s.shndx as usize))
        .filter(move |&sec| elf.is_code(sec))
}

/// Links kernel symbol `k` of symbol table `symtab`; `taken` are the code
/// sections whose address the module's data holds.
fn link(
    elf: &Elf,
    rels: &[Vec<(usize, Reloc)>],
    taken: &[usize],
    symtab: usize,
    syms: &[Sym],
    k: &Sym,
) -> Kernel {
    let mut order = vec![k.shndx as usize];
    let mut placed = vec![false; elf.sections.len()];
    placed[k.shndx as usize] = true;
    let mut i = 0;
    for pass in 0..2 {
        if pass == 1 {
            for &sec in taken {
                if !std::mem::replace(&mut placed[sec], true) {
                    order.push(sec);
                }
            }
        }
        while i < order.len() {
            let found: Vec<usize> =
                code_refs(elf, rels, symtab, syms, std::iter::once(order[i])).collect();
            for sec in found {
                if !std::mem::replace(&mut placed[sec], true) {
                    order.push(sec);
                }
            }
            i += 1;
        }
    }
    let mut code: Vec<u8> = Vec::new();
    let mut place = Vec::with_capacity(order.len());
    for &sec in &order {
        let align = elf.sections[sec].align.clamp(LINE as u64, 1 << 12) as usize;
        while !code.len().is_multiple_of(align) {
            code.extend_from_slice(&NOP);
        }
        place.push((sec, code.len()));
        code.extend_from_slice(elf.data(sec));
        while !code.len().is_multiple_of(LINE) {
            code.push(0);
        }
    }
    let base_of = |sec: usize| place.iter().find(|p| p.0 == sec).map(|p| p.1 as u64);
    let mut unresolved = BTreeSet::new();
    for &(sec, at) in &place {
        for (tab, r) in &rels[sec] {
            let Some(s) = syms.get(r.sym).filter(|_| *tab == symtab) else {
                continue;
            };
            let Some(b) = base_of(s.shndx as usize) else {
                unresolved.insert(sym_name(elf, symtab, s));
                continue;
            };
            let v = b.wrapping_add(s.value).wrapping_add(r.addend as u64);
            let line = at + (r.off & !(LINE - 1));
            let byte = (r.off % LINE) as u32 * 8;
            let ok = match r.kind {
                R_CUDA_ABS32_LO_32 => put_field(&mut code, line, byte + 32, 32, v & 0xFFFF_FFFF),
                R_CUDA_ABS32_HI_32 => put_field(&mut code, line, byte + 32, 32, v >> 32),
                R_CUDA_ABS47_34 => put_field(&mut code, line, byte + 34, 47, v >> 2),
                _ => false,
            };
            if !ok {
                unresolved.insert(format!(
                    "{} (relocation type {})",
                    sym_name(elf, symtab, s),
                    r.kind
                ));
            }
        }
    }
    while code.len() >= LINE && is_nop(&code[code.len() - LINE..]) {
        code.truncate(code.len() - LINE);
    }
    Kernel {
        name: sym_name(elf, symtab, k),
        code,
        unresolved: unresolved.into_iter().collect(),
        externs: Vec::new(),
        shared_bytes: shared_bytes(elf, &sym_name(elf, symtab, k)),
    }
}

/// The size of the `.nv.shared.<name>` section of kernel `name`, 0 without
/// one.
fn shared_bytes(elf: &Elf, name: &str) -> u32 {
    let want = format!(".nv.shared.{name}");
    elf.sections
        .iter()
        .find(|s| s.kind == SHT_NOBITS && elf.cstr(elf.shstr + s.name as usize) == want)
        .map_or(0, |s| u32::try_from(s.size).unwrap_or(u32::MAX))
}

/// The undefined symbols relocated into constant banks through `symtab`.
fn bank_externs(
    elf: &Elf,
    rels: &[Vec<(usize, Reloc)>],
    symtab: usize,
    syms: &[Sym],
) -> Vec<Extern> {
    let mut v = Vec::new();
    for (sec, rs) in rels.iter().enumerate() {
        let name = elf.cstr(elf.shstr + elf.sections[sec].name as usize);
        let Some(bank) = name
            .strip_prefix(".nv.constant")
            .and_then(|b| b.parse::<u32>().ok())
        else {
            continue;
        };
        for (tab, r) in rs {
            let Some(s) = syms.get(r.sym).filter(|s| *tab == symtab && s.shndx == 0) else {
                continue;
            };
            v.push(Extern {
                bank,
                offset: r.off as u32,
                name: sym_name(elf, symtab, s),
            });
        }
    }
    v.sort_by_key(|e| (e.bank, e.offset));
    v
}

/// A symbol's name, or for a section symbol its section's name.
fn sym_name(elf: &Elf, symtab: usize, s: &Sym) -> String {
    let strtab = elf.sections[symtab].link as usize;
    let name = match elf.sections.get(strtab) {
        Some(st) if s.name != 0 => elf.cstr(st.off + s.name as usize),
        _ => String::new(),
    };
    if !name.is_empty() {
        return name;
    }
    match elf.sections.get(s.shndx as usize) {
        Some(sec) if s.shndx != 0 => elf.cstr(elf.shstr + sec.name as usize),
        _ => format!("symbol at {:#x}", s.value),
    }
}

/// The kernels of the cubin at the start of `b`, its SM version and its
/// extent; `None` when `b` does not start with one.
pub fn cubin(b: &[u8]) -> Option<(u32, Vec<Kernel>, usize)> {
    let (elf, end) = Elf::parse(b)?;
    let sm = elf.sm();
    if !SMS.contains(&sm) {
        return Some((sm, Vec::new(), end));
    }
    let mut kernels = Vec::new();
    let rels = relocs(&elf);
    for (t, s) in elf.sections.iter().enumerate() {
        if s.kind != SHT_SYMTAB {
            continue;
        }
        let syms = elf.symbols(t);
        let data = (0..elf.sections.len())
            .filter(|&d| elf.sections[d].flags & SHF_ALLOC != 0 && !elf.is_code(d));
        let mut taken: Vec<usize> = code_refs(&elf, &rels, t, &syms, data).collect();
        taken.sort_unstable();
        taken.dedup();
        let externs = bank_externs(&elf, &rels, t, &syms);
        for k in &syms {
            if k.info & 0xF == STT_FUNC
                && k.other & STO_CUDA_ENTRY != 0
                && elf.is_code(k.shndx as usize)
                && elf.has_section(&format!(".nv.info.{}", sym_name(&elf, t, k)))
            {
                let mut kernel = link(&elf, &rels, &taken, t, &syms, k);
                kernel.externs.clone_from(&externs);
                kernels.push(kernel);
            }
        }
    }
    Some((sm, kernels, end))
}

/// Decodes one LZ4 block of at most `cap` bytes.
pub fn lz4_block(src: &[u8], cap: usize) -> Option<Vec<u8>> {
    let mut out: Vec<u8> = Vec::with_capacity(cap.min(MAX_ELF as usize));
    let mut i = 0;
    let len = |i: &mut usize, mut n: usize| -> Option<usize> {
        if n == 15 {
            loop {
                let x = *src.get(*i)?;
                *i += 1;
                n = n.checked_add(usize::from(x))?;
                if x != 255 {
                    break;
                }
            }
        }
        Some(n)
    };
    while i < src.len() {
        let t = src[i];
        i += 1;
        let lit = len(&mut i, usize::from(t >> 4))?;
        out.extend_from_slice(src.get(i..i.checked_add(lit)?)?);
        i += lit;
        if out.len() > cap {
            return None;
        }
        if i >= src.len() {
            break;
        }
        let off = usize::from(u16_at(src, i)?);
        i += 2;
        let m = len(&mut i, usize::from(t & 15))?.checked_add(4)?;
        if off == 0 || off > out.len() || out.len() + m > cap {
            return None;
        }
        let from = out.len() - off;
        for k in 0..m {
            let c = out[from + k];
            out.push(c);
        }
    }
    Some(out)
}

/// An ELF entry of a fat binary: its offset and its payload, decompressed.
type Entry = (usize, Vec<u8>);

/// The ELF entries of the fat binary at the start of `b` and its extent.
fn fatbin(b: &[u8], errors: &mut Vec<String>) -> Option<(Vec<Entry>, usize)> {
    if !b.starts_with(&FATBIN_MAGIC) {
        return None;
    }
    let hs = usize::from(u16_at(b, 6)?);
    let size = usize::try_from(u64_at(b, 8)?).ok()?;
    let end = hs.checked_add(size)?;
    if hs < 16 || end > b.len() {
        return None;
    }
    let mut out = Vec::new();
    let mut p = hs;
    while p + 0x40 <= end {
        let kind = u16_at(b, p)?;
        let ehs = usize::try_from(u32_at(b, p + 4)?).ok()?;
        let psz = usize::try_from(u64_at(b, p + 8)?).ok()?;
        let body = p.checked_add(ehs)?;
        let next = body.checked_add(psz)?;
        if ehs < 0x40 || next > end {
            errors.push(format!("fatbin entry at {p:#x} runs past the end"));
            break;
        }
        if kind == FATBIN_ELF {
            let flags = u64_at(b, p + 0x28)?;
            let csz = u32_at(b, p + 0x10)? as usize;
            let usz = usize::try_from(u64_at(b, p + 0x38)?).ok()?;
            let packed = &b[body..body + if csz > 0 { csz.min(psz) } else { psz }];
            let data = if flags & FLAG_LZ4 != 0 {
                lz4_block(packed, usz)
            } else if flags & FLAG_ZSTD != 0 {
                zst::decode(packed, &zst::Dicts::new()).ok()
            } else {
                Some(b[body..next].to_vec())
            };
            match data {
                Some(d) => out.push((p, d)),
                None => errors.push(format!("fatbin entry at {p:#x} does not decompress")),
            }
        }
        p = next;
    }
    Some((out, end))
}

fn add(found: &mut Found, at: String, b: &[u8]) -> Option<usize> {
    let (sm, kernels, end) = cubin(b)?;
    found.cubins.push(Cubin { at, sm, kernels });
    Some(end)
}

/// Every cubin in `b`: whole, and in the ELF entries of fat binaries.
pub fn find(b: &[u8]) -> Found {
    let mut found = Found::default();
    for at in crate::carve::find_all(b, &FATBIN_MAGIC) {
        if found.ranges.iter().any(|r| r.contains(&at)) {
            continue;
        }
        let mut errs = Vec::new();
        let Some((entries, end)) = fatbin(&b[at..], &mut errs) else {
            continue;
        };
        found.fatbins += 1;
        found.ranges.push(at..at + end);
        for e in errs {
            found.errors.push((e, format!("{at:#x}")));
        }
        for (p, d) in entries {
            let label = format!("{at:#x}/{p:#x}");
            if add(&mut found, label.clone(), &d).is_none() {
                found
                    .errors
                    .push(("fatbin entry is not a cubin".into(), label));
            }
        }
    }
    for at in crate::carve::find_all(b, &ELF_MAGIC) {
        if found.ranges.iter().any(|r| r.contains(&at)) {
            continue;
        }
        if let Some(end) = add(&mut found, format!("{at:#x}"), &b[at..]) {
            found.ranges.push(at..at + end);
        }
    }
    found
}
