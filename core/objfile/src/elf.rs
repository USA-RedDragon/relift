//! ELF: 64-bit little-endian files, read from program headers, the
//! dynamic section and, when the file has them, the section table and
//! symbol table.

use crate::{
    Binding, Dynamic, Error, Format, Kind, Machine, Object, Perm, ProgramHeaders, RelocKind,
    RelocTable, Relocation, Section, SectionKind, Segment, Symbol, SymbolKind, Tls, err,
};

const PT_LOAD: u32 = 1;
const PT_DYNAMIC: u32 = 2;
const PT_INTERP: u32 = 3;
const PT_PHDR: u32 = 6;
const PT_TLS: u32 = 7;
const PT_GNU_EH_FRAME: u32 = 0x6474_e550;
const PT_GNU_STACK: u32 = 0x6474_e551;

const SHT_SYMTAB: u32 = 2;
const SHT_NOBITS: u32 = 8;
const SHT_INIT_ARRAY: u32 = 14;
const SHT_FINI_ARRAY: u32 = 15;
const SHT_PREINIT_ARRAY: u32 = 16;
const SHT_DYNSYM: u32 = 11;
const SHF_WRITE: u64 = 1;
const SHF_ALLOC: u64 = 2;
const SHF_EXEC: u64 = 4;
const SHF_TLS: u64 = 0x400;

const DT_NULL: u64 = 0;
const DT_NEEDED: u64 = 1;
const DT_PLTRELSZ: u64 = 2;
const DT_HASH: u64 = 4;
const DT_STRTAB: u64 = 5;
const DT_SYMTAB: u64 = 6;
const DT_RELA: u64 = 7;
const DT_RELASZ: u64 = 8;
const DT_STRSZ: u64 = 10;
const DT_INIT: u64 = 12;
const DT_FINI: u64 = 13;
const DT_SONAME: u64 = 14;
const DT_RPATH: u64 = 15;
const DT_JMPREL: u64 = 23;
const DT_INIT_ARRAY: u64 = 25;
const DT_FINI_ARRAY: u64 = 26;
const DT_INIT_ARRAYSZ: u64 = 27;
const DT_FINI_ARRAYSZ: u64 = 28;
const DT_RUNPATH: u64 = 29;
const DT_FLAGS: u64 = 30;
const DT_PREINIT_ARRAY: u64 = 32;
const DT_PREINIT_ARRAYSZ: u64 = 33;
const DT_RELRSZ: u64 = 35;
const DT_RELR: u64 = 36;
const DT_GNU_HASH: u64 = 0x6fff_fef5;
const DT_FLAGS_1: u64 = 0x6fff_fffb;
const DF_BIND_NOW: u64 = 8;
const DF_1_NOW: u64 = 1;

const EM_AARCH64: u16 = 183;

const R_AARCH64_COPY: u32 = 1024;
const R_AARCH64_ABS64: u32 = 257;
const R_AARCH64_GLOB_DAT: u32 = 1025;
const R_AARCH64_JUMP_SLOT: u32 = 1026;
const R_AARCH64_RELATIVE: u32 = 1027;
const R_AARCH64_TLS_DTPMOD: u32 = 1028;
const R_AARCH64_TLS_DTPREL: u32 = 1029;
const R_AARCH64_TLS_TPREL: u32 = 1030;
const R_AARCH64_TLSDESC: u32 = 1031;
const R_AARCH64_IRELATIVE: u32 = 1032;

fn u16_at(b: &[u8], o: usize) -> Option<u16> {
    Some(u16::from_le_bytes(
        b.get(o..o.checked_add(2)?)?.try_into().ok()?,
    ))
}

fn u32_at(b: &[u8], o: usize) -> Option<u32> {
    Some(u32::from_le_bytes(
        b.get(o..o.checked_add(4)?)?.try_into().ok()?,
    ))
}

fn u64_at(b: &[u8], o: usize) -> Option<u64> {
    Some(u64::from_le_bytes(
        b.get(o..o.checked_add(8)?)?.try_into().ok()?,
    ))
}

fn cstr(table: &[u8], at: usize) -> Option<String> {
    let rest = table.get(at..)?;
    let end = rest.iter().position(|&b| b == 0)?;
    Some(String::from_utf8_lossy(&rest[..end]).into_owned())
}

struct Phdr {
    kind: u32,
    flags: u32,
    offset: u64,
    vaddr: u64,
    filesz: u64,
    memsz: u64,
    align: u64,
}

struct Shdr {
    name: u32,
    kind: u32,
    flags: u64,
    addr: u64,
    offset: u64,
    size: u64,
    link: u32,
    entsize: u64,
}

fn slice(file: &[u8], offset: u64, size: u64) -> Option<&[u8]> {
    let start = usize::try_from(offset).ok()?;
    file.get(start..start.checked_add(usize::try_from(size).ok()?)?)
}

/// Whether `bytes` start with the ELF magic number.
pub fn is_elf(bytes: &[u8]) -> bool {
    bytes.starts_with(b"\x7fELF")
}

/// Reads the 64-bit little-endian ELF file `file`.
///
/// # Errors
///
/// The bytes are not a well-formed executable or shared library of a
/// 64-bit little-endian ELF file.
pub fn parse(file: &[u8]) -> Result<Object, Error> {
    if !is_elf(file) || file.len() < 64 {
        return err("not an ELF file");
    }
    if file[4] != 2 || file[5] != 1 {
        return err("only 64-bit little-endian ELF files are read");
    }
    let kind = match u16_at(file, 16) {
        Some(2) => Kind::Fixed,
        Some(3) => Kind::Relocatable,
        Some(t) => {
            return err(format!(
                "ELF file type {t} is neither an executable nor a shared object"
            ));
        }
        None => return err("truncated ELF header"),
    };
    let machine = match u16_at(file, 18) {
        Some(EM_AARCH64) => Machine::Aarch64,
        Some(m) => Machine::Other(u32::from(m)),
        None => return err("truncated ELF header"),
    };
    let h = |o: usize| u64_at(file, o).ok_or_else(|| Error("truncated ELF header".into()));
    let entry = h(24)?;
    let (phoff, shoff) = (h(32)?, h(40)?);
    let phentsize = u64::from(u16_at(file, 54).unwrap_or(0));
    let phnum = u64::from(u16_at(file, 56).unwrap_or(0));
    let shentsize = u64::from(u16_at(file, 58).unwrap_or(0));
    let shnum = u64::from(u16_at(file, 60).unwrap_or(0));
    let shstrndx = usize::from(u16_at(file, 62).unwrap_or(0));
    if phnum > 0 && phentsize < 56 {
        return err("program header entries are too small");
    }
    let mut phdrs = Vec::new();
    for i in 0..phnum {
        let o = usize::try_from(phoff + i * phentsize).or_else(|_| err("program header offset"))?;
        let get = |k: usize| u64_at(file, o + k);
        let (Some(kind), Some(flags)) = (u32_at(file, o), u32_at(file, o + 4)) else {
            return err("truncated program header table");
        };
        let (Some(offset), Some(vaddr), Some(filesz), Some(memsz), Some(align)) =
            (get(8), get(16), get(32), get(40), get(48))
        else {
            return err("truncated program header table");
        };
        phdrs.push(Phdr {
            kind,
            flags,
            offset,
            vaddr,
            filesz,
            memsz,
            align,
        });
    }
    let mut segments = Vec::new();
    for p in phdrs.iter().filter(|p| p.kind == PT_LOAD) {
        if p.filesz > p.memsz {
            return err("a segment holds more bytes in the file than in memory");
        }
        let Some(data) = slice(file, p.offset, p.filesz) else {
            return err("a segment lies outside the file");
        };
        if p.vaddr.checked_add(p.memsz).is_none() {
            return err("a segment wraps the address space");
        }
        segments.push(Segment {
            addr: p.vaddr,
            mem_size: p.memsz,
            data: data.to_vec(),
            perm: Perm {
                read: p.flags & 4 != 0,
                write: p.flags & 2 != 0,
                exec: p.flags & 1 != 0,
            },
            align: p.align,
            offset: p.offset,
        });
    }
    segments.sort_by_key(|s| s.addr);
    for w in segments.windows(2) {
        if w[0].addr + w[0].mem_size > w[1].addr {
            return err("two segments overlap");
        }
    }
    let mut o = Object {
        format: Format::Elf,
        machine,
        kind,
        entry,
        segments,
        sections: Vec::new(),
        symbols: Vec::new(),
        dynamic_symbols: Vec::new(),
        dynamic: None,
        relocations: Vec::new(),
        tls: None,
        interpreter: None,
        program_headers: None,
        unwind: None,
        stack_size: 0,
        bias: 0,
        exec_stack: false,
    };
    for p in &phdrs {
        match p.kind {
            PT_TLS => {
                o.tls = Some(Tls {
                    addr: p.vaddr,
                    file_size: p.filesz,
                    mem_size: p.memsz,
                    align: p.align.max(1),
                });
            }
            PT_INTERP => {
                let bytes = slice(file, p.offset, p.filesz).unwrap_or_default();
                o.interpreter = cstr(bytes, 0);
            }
            PT_PHDR => {
                o.program_headers = Some(ProgramHeaders {
                    addr: p.vaddr,
                    count: phnum,
                    entry_size: phentsize,
                });
            }
            PT_GNU_EH_FRAME => o.unwind = Some((p.vaddr, p.memsz)),
            PT_GNU_STACK => {
                o.stack_size = p.memsz;
                o.exec_stack = p.flags & 1 != 0;
            }
            _ => {}
        }
    }
    if o.program_headers.is_none() && phnum > 0 {
        let table = phnum * phentsize;
        o.program_headers = phdrs
            .iter()
            .find(|p| {
                p.kind == PT_LOAD && p.offset <= phoff && phoff + table <= p.offset + p.filesz
            })
            .map(|p| ProgramHeaders {
                addr: p.vaddr + (phoff - p.offset),
                count: phnum,
                entry_size: phentsize,
            });
    }
    let sections = read_sections(file, shoff, shentsize, shnum, shstrndx);
    for (name, s) in &sections {
        if s.flags & SHF_ALLOC == 0 {
            continue;
        }
        let kind = match s.kind {
            SHT_NOBITS => SectionKind::Zero,
            SHT_INIT_ARRAY | SHT_FINI_ARRAY | SHT_PREINIT_ARRAY => SectionKind::CodeAddresses,
            1 => SectionKind::Data,
            _ => SectionKind::Other,
        };
        o.sections.push(Section {
            name: name.clone(),
            addr: s.addr,
            size: s.size,
            kind,
            exec: s.flags & SHF_EXEC != 0,
            write: s.flags & SHF_WRITE != 0,
            tls: s.flags & SHF_TLS != 0,
        });
    }
    o.sections.sort_by_key(|s| s.addr);
    if o.unwind.is_none()
        && let Some(s) = o.sections.iter().find(|s| s.name == ".eh_frame")
    {
        o.unwind = Some((s.addr, s.size));
    }
    if let Some((_, s)) = sections.iter().find(|(_, s)| s.kind == SHT_SYMTAB)
        && let Some((_, strtab)) = sections.get(s.link as usize)
        && let (Some(syms), Some(strs)) = (
            slice(file, s.offset, s.size),
            slice(file, strtab.offset, strtab.size),
        )
    {
        o.symbols = symbols(syms, strs, false);
    }
    let dyn_symbols_section = sections.iter().find(|(_, s)| s.kind == SHT_DYNSYM);
    if let Some(d) = phdrs.iter().find(|p| p.kind == PT_DYNAMIC) {
        read_dynamic(&mut o, d, dyn_symbols_section.map(|(_, s)| s))?;
    }
    Ok(o)
}

fn read_sections(
    file: &[u8],
    shoff: u64,
    entsize: u64,
    num: u64,
    names: usize,
) -> Vec<(String, Shdr)> {
    if shoff == 0 || num == 0 || entsize < 64 {
        return Vec::new();
    }
    let mut out = Vec::new();
    for i in 0..num {
        let Ok(o) = usize::try_from(shoff + i * entsize) else {
            return Vec::new();
        };
        let (Some(name), Some(kind), Some(flags), Some(addr), Some(offset), Some(size)) = (
            u32_at(file, o),
            u32_at(file, o + 4),
            u64_at(file, o + 8),
            u64_at(file, o + 16),
            u64_at(file, o + 24),
            u64_at(file, o + 32),
        ) else {
            return Vec::new();
        };
        let (Some(link), Some(entsize)) = (u32_at(file, o + 40), u64_at(file, o + 56)) else {
            return Vec::new();
        };
        out.push((
            String::new(),
            Shdr {
                name,
                kind,
                flags,
                addr,
                offset,
                size,
                link,
                entsize,
            },
        ));
    }
    let strs = out
        .get(names)
        .and_then(|(_, s)| (s.kind != SHT_NOBITS).then(|| slice(file, s.offset, s.size)))
        .flatten()
        .map(<[u8]>::to_vec)
        .unwrap_or_default();
    for (name, s) in &mut out {
        *name = cstr(&strs, s.name as usize).unwrap_or_default();
    }
    out
}

fn read_dynamic(o: &mut Object, p: &Phdr, dynsym: Option<&Shdr>) -> Result<(), Error> {
    let mut tags: Vec<(u64, u64)> = Vec::new();
    for i in 0..p.filesz / 16 {
        let (Some(tag), Some(val)) = (
            o.read_u64(p.vaddr + i * 16),
            o.read_u64(p.vaddr + i * 16 + 8),
        ) else {
            return err("truncated dynamic section");
        };
        if tag == DT_NULL {
            break;
        }
        tags.push((tag, val));
    }
    let get = |t: u64| tags.iter().find(|(k, _)| *k == t).map(|(_, v)| *v);
    let pair = |a: u64, s: u64| get(a).map(|addr| (addr, get(s).unwrap_or(0)));
    let strtab = get(DT_STRTAB).zip(get(DT_STRSZ));
    let name = |off: u64| -> Option<String> {
        let (addr, size) = strtab?;
        let bytes = o.read(addr, usize::try_from(size).ok()?)?;
        cstr(bytes, usize::try_from(off).ok()?)
    };
    let d = Dynamic {
        needed: tags
            .iter()
            .filter(|(k, _)| *k == DT_NEEDED)
            .filter_map(|(_, v)| name(*v))
            .collect(),
        soname: get(DT_SONAME).and_then(name),
        search_path: get(DT_RUNPATH).or(get(DT_RPATH)).and_then(name),
        rpath: get(DT_RUNPATH).is_none() && get(DT_RPATH).is_some(),
        init: get(DT_INIT),
        fini: get(DT_FINI),
        init_array: pair(DT_INIT_ARRAY, DT_INIT_ARRAYSZ),
        fini_array: pair(DT_FINI_ARRAY, DT_FINI_ARRAYSZ),
        preinit_array: pair(DT_PREINIT_ARRAY, DT_PREINIT_ARRAYSZ),
        bind_now: get(DT_FLAGS).is_some_and(|f| f & DF_BIND_NOW != 0)
            || get(DT_FLAGS_1).is_some_and(|f| f & DF_1_NOW != 0),
    };
    if let (Some(symtab), Some((saddr, ssize))) = (get(DT_SYMTAB), strtab) {
        let count = if let Some(s) = dynsym {
            s.size / s.entsize.max(24)
        } else if let Some(h) = get(DT_HASH) {
            o.read(h + 4, 4).map_or(0, |b| {
                u64::from(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
            })
        } else if let Some(g) = get(DT_GNU_HASH) {
            gnu_hash_count(o, g)
        } else {
            0
        };
        let mut raw = Vec::new();
        for i in 0..count * 3 {
            if let Some(w) = o.read_u64(symtab + i * 8) {
                raw.extend_from_slice(&w.to_le_bytes());
            }
        }
        let strs = o
            .read(saddr, usize::try_from(ssize).unwrap_or(0))
            .map(<[u8]>::to_vec)
            .unwrap_or_default();
        o.dynamic_symbols = symbols(&raw, &strs, true);
    }
    let mut relocs = Vec::new();
    if let Some(r) = get(DT_RELA) {
        relocs.extend(read_rela(
            o,
            r,
            get(DT_RELASZ).unwrap_or(0),
            RelocTable::Addend,
        ));
    }
    if let Some(r) = get(DT_JMPREL) {
        relocs.extend(read_rela(
            o,
            r,
            get(DT_PLTRELSZ).unwrap_or(0),
            RelocTable::Plt,
        ));
    }
    if let Some(r) = get(DT_RELR) {
        relocs.extend(read_relr(o, r, get(DT_RELRSZ).unwrap_or(0)));
    }
    o.relocations = relocs;
    o.dynamic = Some(d);
    Ok(())
}

/// The symbols of a table of 24-byte entries. The symbol table of a file
/// skips its empty first entry and the section and file entries; the
/// dynamic table keeps every entry so relocation indexes address it.
fn symbols(table: &[u8], strs: &[u8], dynamic: bool) -> Vec<Symbol> {
    let mut out = Vec::new();
    for i in 0..table.len() / 24 {
        let o = i * 24;
        let (Some(name), Some(&info), Some(shndx), Some(value), Some(size)) = (
            u32_at(table, o),
            table.get(o + 4),
            u16_at(table, o + 6),
            u64_at(table, o + 8),
            u64_at(table, o + 16),
        ) else {
            break;
        };
        if !dynamic && (i == 0 || matches!(info & 0xf, 3 | 4)) {
            continue;
        }
        let kind = match info & 0xf {
            1 => SymbolKind::Object,
            2 => SymbolKind::Function,
            6 => SymbolKind::Tls,
            10 => SymbolKind::Indirect,
            _ => SymbolKind::Other,
        };
        let bind = match info >> 4 {
            0 => Binding::Local,
            2 => Binding::Weak,
            _ => Binding::Global,
        };
        out.push(Symbol {
            name: cstr(strs, name as usize).unwrap_or_default(),
            value,
            size,
            kind,
            bind,
            defined: shndx != 0,
        });
    }
    out
}

fn gnu_hash_count(o: &Object, at: u64) -> u64 {
    let word = |a: u64| {
        o.read(a, 4)
            .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    };
    let (Some(nbuckets), Some(symoffset), Some(bloom)) = (word(at), word(at + 4), word(at + 8))
    else {
        return 0;
    };
    let buckets = at + 16 + u64::from(bloom) * 8;
    let chains = buckets + u64::from(nbuckets) * 4;
    let last = (0..u64::from(nbuckets))
        .filter_map(|i| word(buckets + i * 4))
        .max()
        .unwrap_or(0);
    if last < symoffset {
        return u64::from(symoffset);
    }
    let mut i = u64::from(last);
    loop {
        let Some(h) = word(chains + (i - u64::from(symoffset)) * 4) else {
            return i + 1;
        };
        i += 1;
        if h & 1 != 0 {
            return i;
        }
    }
}

fn reloc_kind(r#type: u32) -> RelocKind {
    match r#type {
        R_AARCH64_RELATIVE => RelocKind::Relative,
        R_AARCH64_ABS64 => RelocKind::Absolute,
        R_AARCH64_GLOB_DAT => RelocKind::GlobalData,
        R_AARCH64_JUMP_SLOT => RelocKind::JumpSlot,
        R_AARCH64_IRELATIVE => RelocKind::Indirect,
        R_AARCH64_TLS_DTPMOD => RelocKind::TlsModule,
        R_AARCH64_TLS_DTPREL => RelocKind::TlsOffset,
        R_AARCH64_TLS_TPREL => RelocKind::TlsThreadOffset,
        R_AARCH64_TLSDESC => RelocKind::TlsDescriptor,
        R_AARCH64_COPY => RelocKind::Copy,
        t => RelocKind::Other(t),
    }
}

fn read_rela(o: &Object, at: u64, size: u64, table: RelocTable) -> Vec<Relocation> {
    let mut out = Vec::new();
    for i in 0..size / 24 {
        let a = at + i * 24;
        let (Some(offset), Some(info), Some(addend)) =
            (o.read_u64(a), o.read_u64(a + 8), o.read_u64(a + 16))
        else {
            break;
        };
        let sym = (info >> 32) as usize;
        out.push(Relocation {
            offset,
            kind: reloc_kind(info as u32),
            symbol: (sym != 0).then_some(sym),
            addend: addend as i64,
            table,
        });
    }
    out
}

fn read_relr(o: &Object, at: u64, size: u64) -> Vec<Relocation> {
    let mut out = Vec::new();
    let mut next = 0u64;
    let mut add = |offset: u64| {
        if let Some(v) = o.read_u64(offset) {
            out.push(Relocation {
                offset,
                kind: RelocKind::Relative,
                symbol: None,
                addend: v as i64,
                table: RelocTable::Packed,
            });
        }
    };
    for i in 0..size / 8 {
        let Some(e) = o.read_u64(at + i * 8) else {
            break;
        };
        if e & 1 == 0 {
            add(e);
            next = e + 8;
        } else {
            for bit in 1..64 {
                if e >> bit & 1 == 1 {
                    add(next + (bit - 1) * 8);
                }
            }
            next += 63 * 8;
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::SeedKind;
    use crate::synth::{Builder, SectionSpec, SegmentSpec, SymbolSpec};

    fn words(w: &[u64]) -> Vec<u8> {
        w.iter().flat_map(|x| x.to_le_bytes()).collect()
    }

    fn text(addr: u64) -> SegmentSpec {
        SegmentSpec {
            addr,
            data: [0x1f, 0x20, 0x03, 0xd5].repeat(64),
            mem_size: 256,
            flags: 5,
        }
    }

    #[test]
    fn a_static_executable_has_segments_entry_and_symbols() {
        let b = Builder {
            entry: 0x1000,
            segments: vec![
                text(0x1000),
                SegmentSpec {
                    addr: 0x2000,
                    data: vec![1, 2, 3],
                    mem_size: 0x100,
                    flags: 6,
                },
            ],
            stack: Some(0x80_0000),
            sections: vec![SectionSpec {
                name: ".text".into(),
                kind: 1,
                flags: 6,
                addr: 0x1000,
                size: 256,
            }],
            symbols: vec![SymbolSpec {
                name: "main".into(),
                value: 0x1010,
                size: 8,
                kind: 2,
                bind: 1,
            }],
            ..Builder::default()
        };
        let o = parse(&b.build()).expect("parses");
        assert_eq!(o.kind, Kind::Fixed);
        assert_eq!(o.machine, Machine::Aarch64);
        assert_eq!(o.entry, 0x1000);
        assert_eq!(o.segments.len(), 2);
        assert!(o.segments[0].perm.exec && !o.segments[0].perm.write);
        assert!(o.segments[1].perm.write);
        assert_eq!(o.read(0x2001, 2), Some(&[2u8, 3][..]));
        assert_eq!(o.stack_size, 0x80_0000);
        assert!(!o.exec_stack);
        assert_eq!(o.functions().len(), 1);
        assert_eq!(o.functions()[0].name, "main");
        assert!(o.is_code(0x1010));
        assert!(!o.is_code(0x2000));
        let seeds = o.seeds();
        assert_eq!(
            seeds.iter().map(|s| (s.addr, s.kind)).collect::<Vec<_>>(),
            [(0x1000, SeedKind::Entry), (0x1010, SeedKind::Symbol)]
        );
        assert_eq!(
            o.image(1 << 20).expect("image").bytes.len(),
            0x2003 - 0x1000
        );
        assert!(o.image(16).is_err());
    }

    #[test]
    fn a_file_without_sections_takes_code_from_executable_segments() {
        let b = Builder {
            entry: 0x1004,
            segments: vec![text(0x1000)],
            ..Builder::default()
        };
        let o = parse(&b.build()).expect("parses");
        assert!(o.sections.is_empty());
        assert!(o.is_code(0x10fc));
        assert!(!o.is_code(0x1100));
    }

    fn dynamic_file() -> Vec<u8> {
        let strtab = b"\0libc.so\0main\0puts\0";
        let mut sym = vec![0u8; 24];
        for (name, info, shndx, value) in [(9u32, 0x12u8, 1u16, 0x1010u64), (14, 0x12, 0, 0)] {
            sym.extend_from_slice(&name.to_le_bytes());
            sym.push(info);
            sym.push(0);
            sym.extend_from_slice(&shndx.to_le_bytes());
            sym.extend_from_slice(&value.to_le_bytes());
            sym.extend_from_slice(&0u64.to_le_bytes());
        }
        let mut data = vec![0u8; 0x400];
        let mut put = |at: usize, b: &[u8]| data[at..at + b.len()].copy_from_slice(b);
        put(0x100, strtab);
        put(0x120, &sym);
        let rela: Vec<u8> = [
            [0x2008u64, 1027, 0x1020],
            [0x2010, (2u64 << 32) | 1025, 0],
            [0x2018, 1032, 0x1030],
        ]
        .iter()
        .flat_map(|r| words(r))
        .collect();
        put(0x180, &rela);
        put(0x1e0, &words(&[0x1040, 0]));
        put(0x1f0, &[1, 0, 0, 0, 3, 0, 0, 0]);
        put(0x200, &words(&[0x2008, 0b101]));
        let dynamic = words(&[
            1,
            1,
            5,
            0x2100,
            6,
            0x2120,
            10,
            strtab.len() as u64,
            7,
            0x2180,
            8,
            72,
            36,
            0x2200,
            35,
            16,
            25,
            0x21e0,
            27,
            8,
            12,
            0x1040,
            4,
            0x21f0,
            0,
            0,
        ]);
        put(0x300, &dynamic);
        let b = Builder {
            shared: true,
            entry: 0x1000,
            segments: vec![
                text(0x1000),
                SegmentSpec {
                    addr: 0x2000,
                    data,
                    mem_size: 0x400,
                    flags: 6,
                },
            ],
            dynamic: Some((0x2300, 16 * 14)),
            interpreter: Some("/lib/ld.so".into()),
            tls: Some((0x2000, 8, 16, 8)),
            ..Builder::default()
        };
        b.build()
    }

    #[test]
    fn the_dynamic_section_gives_needed_libraries_symbols_and_relocations() {
        let mut o = parse(&dynamic_file()).expect("parses");
        assert_eq!(o.kind, Kind::Relocatable);
        assert_eq!(o.interpreter.as_deref(), Some("/lib/ld.so"));
        assert_eq!(o.tls.map(|t| (t.mem_size, t.align)), Some((16, 8)));
        let d = o.dynamic.clone().expect("dynamic");
        assert_eq!(d.needed, ["libc.so"]);
        assert_eq!(d.init, Some(0x1040));
        assert_eq!(o.dynamic_symbols.len(), 3);
        assert_eq!(o.dynamic_symbols[1].name, "main");
        assert!(o.dynamic_symbols[1].defined);
        assert_eq!(o.exports().count(), 1);
        assert_eq!(
            o.imports().map(|s| s.name.as_str()).collect::<Vec<_>>(),
            ["puts"]
        );
        let kinds: Vec<RelocKind> = o.relocations.iter().map(|r| r.kind).collect();
        assert_eq!(
            kinds,
            [
                RelocKind::Relative,
                RelocKind::GlobalData,
                RelocKind::Indirect,
                RelocKind::Relative,
                RelocKind::Relative
            ]
        );
        assert_eq!(o.relocations[1].symbol, Some(2));
        let packed: Vec<u64> = o
            .relocations
            .iter()
            .filter(|r| r.table == RelocTable::Packed)
            .map(|r| r.offset)
            .collect();
        assert_eq!(packed, [0x2008, 0x2018]);
        let before: Vec<u64> = o.seeds().iter().map(|s| s.addr).collect();
        assert_eq!(before, [0x1000, 0x1010, 0x1020, 0x1030, 0x1040]);
        o.rebase(0x10_0000).expect("moves");
        assert_eq!(o.entry, 0x10_1000);
        assert_eq!(o.segments[1].addr, 0x10_2000);
        let after: Vec<u64> = o.seeds().iter().map(|s| s.addr).collect();
        assert_eq!(
            after,
            before.iter().map(|a| a + 0x10_0000).collect::<Vec<_>>()
        );
    }

    #[test]
    fn a_fixed_executable_does_not_move() {
        let b = Builder {
            entry: 0x1000,
            segments: vec![text(0x1000)],
            ..Builder::default()
        };
        let mut o = parse(&b.build()).expect("parses");
        assert!(o.rebase(0x1000).is_err());
        assert!(o.rebase(0).is_ok());
    }

    #[test]
    fn malformed_files_are_refused() {
        assert!(parse(b"MZ").is_err());
        assert!(parse(&[0x7f, b'E', b'L', b'F', 1, 1, 1, 0]).is_err());
        let good = Builder {
            entry: 0x1000,
            segments: vec![text(0x1000)],
            ..Builder::default()
        }
        .build();
        assert!(parse(&good[..good.len() - 100]).is_err());
        let mut bad = good.clone();
        bad[16] = 1;
        assert!(parse(&bad).is_err());
        let other = Builder {
            machine: 62,
            entry: 0x1000,
            segments: vec![text(0x1000)],
            ..Builder::default()
        };
        assert_eq!(
            parse(&other.build()).expect("parses").machine,
            Machine::Other(62)
        );
    }
}
