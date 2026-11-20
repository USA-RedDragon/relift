//! Executable and library files as relift reads them: one neutral
//! [`Object`] with segments, sections, symbols, dynamic tags, relocations,
//! the entry point, the thread-local storage template, the unwind table
//! and the code addresses the file itself names ([`Seed`]).
//!
//! The model holds file formats only and no operating system concepts:
//! it says where bytes load and what the file says about them, never how a
//! system starts a program. ELF is the one format read so far ([`elf`]);
//! PE and Mach-O fit the same model. Addresses are the file's own virtual
//! addresses; [`Object::rebase`] moves a position-independent file to the
//! address a guest places it at.

pub mod elf;
pub mod synth;
pub mod unwind;

use std::fmt;

/// Why a file could not be read.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Error(pub String);

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

pub(crate) fn err<T>(msg: impl Into<String>) -> Result<T, Error> {
    Err(Error(msg.into()))
}

/// The container format of a file.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Format {
    /// ELF.
    Elf,
}

/// The instruction set a file holds code for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Machine {
    /// 64-bit Arm (A64).
    Aarch64,
    /// Another machine, by its format's code.
    Other(u32),
}

/// What kind of file it is.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    /// Linked to run at fixed addresses.
    Fixed,
    /// Position independent: runs at any base, either a shared library or
    /// a position-independent executable.
    Relocatable,
}

/// Access rights of a segment.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Perm {
    /// Readable.
    pub read: bool,
    /// Writable.
    pub write: bool,
    /// Executable.
    pub exec: bool,
}

/// One block of memory the file loads.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Segment {
    /// The virtual address.
    pub addr: u64,
    /// The size in memory in bytes; bytes past `data` read as zero.
    pub mem_size: u64,
    /// The bytes the file holds for it, at most `mem_size`.
    pub data: Vec<u8>,
    /// The access rights.
    pub perm: Perm,
    /// The alignment of `addr` the file asks for.
    pub align: u64,
}

impl Segment {
    /// Whether `addr` lies in the segment's memory.
    pub fn contains(&self, addr: u64) -> bool {
        addr >= self.addr && addr - self.addr < self.mem_size
    }
}

/// What a section holds.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SectionKind {
    /// Bytes in the file.
    Data,
    /// Zero-filled memory with no bytes in the file.
    Zero,
    /// An array of code addresses to call before the program's `main`
    /// (preinit and init arrays) or after (fini arrays).
    CodeAddresses,
    /// Anything else the model has no use for.
    Other,
}

/// One named part of the file's memory image.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Section {
    /// The name, empty when the file has no names.
    pub name: String,
    /// The virtual address.
    pub addr: u64,
    /// The size in bytes.
    pub size: u64,
    /// What it holds.
    pub kind: SectionKind,
    /// Whether it holds code.
    pub exec: bool,
    /// Whether it is writable.
    pub write: bool,
    /// Whether it is thread-local storage.
    pub tls: bool,
}

/// What a symbol names.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SymbolKind {
    /// Code.
    Function,
    /// Data.
    Object,
    /// Thread-local data.
    Tls,
    /// An indirect function: its value is the resolver's address.
    Indirect,
    /// Anything else.
    Other,
}

/// How widely a symbol binds.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Binding {
    /// Visible in its own file only.
    Local,
    /// Visible to other files.
    Global,
    /// Global, and another file's definition wins.
    Weak,
}

/// One symbol.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Symbol {
    /// The name.
    pub name: String,
    /// The address, or for a thread-local symbol its offset in the
    /// thread-local template; meaningless when undefined.
    pub value: u64,
    /// The size in bytes.
    pub size: u64,
    /// What it names.
    pub kind: SymbolKind,
    /// How widely it binds.
    pub bind: Binding,
    /// Whether the file defines it; an undefined symbol is an import.
    pub defined: bool,
}

/// What a relocation computes, for the machine of the file.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RelocKind {
    /// The load base plus the addend.
    Relative,
    /// The symbol's address plus the addend.
    Absolute,
    /// A global offset table word: the symbol's address.
    GlobalData,
    /// A procedure linkage table word: the symbol's address.
    JumpSlot,
    /// The load base plus the result of calling the resolver at the addend.
    Indirect,
    /// The module number of the symbol's file, for thread-local access.
    TlsModule,
    /// The symbol's offset in its file's thread-local template, plus the
    /// addend.
    TlsOffset,
    /// The symbol's offset from the thread pointer, plus the addend.
    TlsThreadOffset,
    /// A thread-local descriptor.
    TlsDescriptor,
    /// A copy of the symbol's data from another file.
    Copy,
    /// Anything else, by the format's code.
    Other(u32),
}

/// Which table a relocation came from.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RelocTable {
    /// The general table with explicit addends.
    Addend,
    /// The procedure linkage table's.
    Plt,
    /// The packed table of relative relocations.
    Packed,
}

/// One relocation: a word the loader fills in.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Relocation {
    /// The address of the word.
    pub offset: u64,
    /// What it computes.
    pub kind: RelocKind,
    /// The symbol, as an index into [`Object::dynamic_symbols`]; `None`
    /// for a relocation without one.
    pub symbol: Option<usize>,
    /// The addend: an address for [`RelocKind::Relative`].
    pub addend: i64,
    /// Which table it came from.
    pub table: RelocTable,
}

/// What the dynamic section says, for a file that has one.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Dynamic {
    /// The libraries it needs, by name, in order.
    pub needed: Vec<String>,
    /// Its own library name.
    pub soname: Option<String>,
    /// The library search path it asks for, from its run path or path
    /// entry.
    pub search_path: Option<String>,
    /// The initialization function.
    pub init: Option<u64>,
    /// The finalization function.
    pub fini: Option<u64>,
    /// The array of initialization functions: address and size in bytes.
    pub init_array: Option<(u64, u64)>,
    /// The array of finalization functions: address and size in bytes.
    pub fini_array: Option<(u64, u64)>,
    /// The array of functions that run before the others' initialization:
    /// address and size in bytes.
    pub preinit_array: Option<(u64, u64)>,
    /// Whether the file asks for every binding at load time.
    pub bind_now: bool,
}

/// The thread-local storage template.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Tls {
    /// The address of the initialization image.
    pub addr: u64,
    /// The bytes of the image, which every thread's copy starts with.
    pub file_size: u64,
    /// The size of a thread's copy; the rest is zero.
    pub mem_size: u64,
    /// The alignment of a thread's copy.
    pub align: u64,
}

/// Where the program headers sit in memory, for the system's auxiliary
/// vector.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ProgramHeaders {
    /// The address of the table.
    pub addr: u64,
    /// The number of entries.
    pub count: u64,
    /// The size of an entry.
    pub entry_size: u64,
}

/// Why a code address is in the file.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum SeedKind {
    /// The entry point.
    Entry,
    /// A function symbol.
    Symbol,
    /// The start of an unwind table entry.
    Unwind,
    /// A code address a relocation stores.
    Relocation,
    /// A function of an initialization or finalization list.
    Initializer,
    /// A code address stored in data of a file with no relocations to
    /// find it by (a file linked for fixed addresses): an aligned word
    /// that holds the address of an instruction.
    Pointer,
}

/// A code address the file names itself.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Seed {
    /// The address.
    pub addr: u64,
    /// Why it is there.
    pub kind: SeedKind,
    /// The symbol name, when there is one.
    pub name: Option<String>,
}

/// A file.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Object {
    /// The container format.
    pub format: Format,
    /// The machine of its code.
    pub machine: Machine,
    /// Fixed or position independent.
    pub kind: Kind,
    /// The entry point.
    pub entry: u64,
    /// The memory blocks it loads, by address.
    pub segments: Vec<Segment>,
    /// The named parts of the image, by address; empty for a file with no
    /// section table.
    pub sections: Vec<Section>,
    /// The symbols of the symbol table, which a stripped file lacks.
    pub symbols: Vec<Symbol>,
    /// The symbols the loader binds by name (the dynamic symbol table); the
    /// first entry is the empty symbol, so an index of a relocation
    /// addresses it directly.
    pub dynamic_symbols: Vec<Symbol>,
    /// The dynamic section's tags, for a file that has one.
    pub dynamic: Option<Dynamic>,
    /// The relocations of the dynamic tables.
    pub relocations: Vec<Relocation>,
    /// The thread-local storage template.
    pub tls: Option<Tls>,
    /// The program interpreter the file names, for a dynamically linked
    /// program.
    pub interpreter: Option<String>,
    /// Where the program headers sit in memory.
    pub program_headers: Option<ProgramHeaders>,
    /// The unwind table: address and size of the `.eh_frame_hdr` search
    /// table when there is one, else of `.eh_frame`.
    pub unwind: Option<(u64, u64)>,
    /// The stack size the file asks for, when it does; 0 is the system's
    /// default.
    pub stack_size: u64,
    /// Whether the file asks for an executable stack.
    pub exec_stack: bool,
}

/// The loaded bytes of an object in one block, from its lowest address.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Image {
    /// The address of `bytes[0]`.
    pub base: u64,
    /// The bytes up to the end of the last segment's file data, zero where
    /// no segment has any.
    pub bytes: Vec<u8>,
}

impl Object {
    /// The lowest address any segment loads at.
    pub fn low(&self) -> Option<u64> {
        self.segments.iter().map(|s| s.addr).min()
    }

    /// The address after the last byte any segment loads.
    pub fn high(&self) -> Option<u64> {
        self.segments.iter().map(|s| s.addr + s.mem_size).max()
    }

    /// The segment that holds `addr`.
    pub fn segment_at(&self, addr: u64) -> Option<&Segment> {
        self.segments.iter().find(|s| s.contains(addr))
    }

    /// `n` bytes of the file's data at `addr`, when one segment's data
    /// holds them all.
    pub fn read(&self, addr: u64, n: usize) -> Option<&[u8]> {
        let s = self.segment_at(addr)?;
        let off = usize::try_from(addr - s.addr).ok()?;
        s.data.get(off..off.checked_add(n)?)
    }

    /// The little-endian 64-bit word of the file's data at `addr`.
    pub fn read_u64(&self, addr: u64) -> Option<u64> {
        Some(u64::from_le_bytes(self.read(addr, 8)?.try_into().ok()?))
    }

    /// Whether `addr` lies in code: a section that holds code, or without
    /// a section table an executable segment.
    pub fn is_code(&self, addr: u64) -> bool {
        if self.sections.is_empty() {
            self.segments
                .iter()
                .any(|s| s.perm.exec && s.contains(addr))
        } else {
            self.sections
                .iter()
                .any(|s| s.exec && addr >= s.addr && addr - s.addr < s.size)
        }
    }

    /// The code addresses stored as aligned 64-bit words in data the file
    /// does not execute. Only a file linked for fixed addresses needs this:
    /// a position-independent file's relocations name its code pointers
    /// exactly. A word counts when it is the address of a 4-byte aligned
    /// byte in code, so the result may include integers that happen to
    /// look like one.
    pub fn data_pointers(&self) -> Vec<u64> {
        let mut out = Vec::new();
        for s in self.segments.iter().filter(|s| !s.perm.exec) {
            let first = s.addr.next_multiple_of(8);
            let mut a = first;
            while let Some(end) = a.checked_add(8)
                && end <= s.addr + s.data.len() as u64
            {
                let at = (a - s.addr) as usize;
                let v = u64::from_le_bytes(s.data[at..at + 8].try_into().unwrap_or_default());
                if v % 4 == 0 && self.is_code(v) {
                    out.push(v);
                }
                a = end;
            }
        }
        out.sort_unstable();
        out.dedup();
        out
    }

    /// The symbol table's function symbols and the dynamic table's, both
    /// defined, without duplicates by address and name.
    pub fn functions(&self) -> Vec<&Symbol> {
        let mut out: Vec<&Symbol> = self
            .symbols
            .iter()
            .chain(&self.dynamic_symbols)
            .filter(|s| s.defined && matches!(s.kind, SymbolKind::Function | SymbolKind::Indirect))
            .collect();
        out.sort_by(|a, b| (a.value, &a.name).cmp(&(b.value, &b.name)));
        out.dedup_by(|a, b| a.value == b.value && a.name == b.name);
        out
    }

    /// The symbols the file needs another file to define.
    pub fn imports(&self) -> impl Iterator<Item = &Symbol> {
        self.dynamic_symbols
            .iter()
            .skip(1)
            .filter(|s| !s.defined && !s.name.is_empty())
    }

    /// The symbols the file defines for other files.
    pub fn exports(&self) -> impl Iterator<Item = &Symbol> {
        self.dynamic_symbols
            .iter()
            .skip(1)
            .filter(|s| s.defined && s.bind != Binding::Local)
    }

    /// The name of the symbol a relocation uses.
    pub fn relocation_symbol(&self, r: &Relocation) -> Option<&Symbol> {
        self.dynamic_symbols.get(r.symbol?)
    }

    /// The loaded bytes in one block; `Err` when the gaps between segments
    /// make the block larger than `limit` bytes.
    pub fn image(&self, limit: u64) -> Result<Image, Error> {
        let (Some(base), Some(_)) = (self.low(), self.high()) else {
            return Ok(Image {
                base: 0,
                bytes: Vec::new(),
            });
        };
        let end = self
            .segments
            .iter()
            .map(|s| s.addr + s.data.len() as u64)
            .max()
            .unwrap_or(base);
        if end - base > limit {
            return err(format!(
                "the segments span {} bytes of address space, more than the limit of {limit}",
                end - base
            ));
        }
        let mut bytes = vec![0u8; (end - base) as usize];
        for s in &self.segments {
            let at = (s.addr - base) as usize;
            bytes[at..at + s.data.len()].copy_from_slice(&s.data);
        }
        Ok(Image { base, bytes })
    }

    /// Every code address the file names itself: the entry point, function
    /// symbols, unwind table entries, code addresses that relocations
    /// store and the functions of initialization and finalization lists.
    /// Sorted by address, one seed per address and kind.
    pub fn seeds(&self) -> Vec<Seed> {
        let mut out = Vec::new();
        let mut add = |addr: u64, kind: SeedKind, name: Option<String>| {
            out.push(Seed { addr, kind, name });
        };
        if self.is_code(self.entry) {
            add(self.entry, SeedKind::Entry, None);
        }
        for s in self.functions() {
            if self.is_code(s.value) {
                add(s.value, SeedKind::Symbol, Some(s.name.clone()));
            }
        }
        if let Some((addr, _)) = self.unwind
            && let Ok(img) = self.image(u64::MAX)
            && let Some(start) = addr.checked_sub(img.base)
        {
            for off in unwind::fde_starts(&img.bytes, start as usize) {
                let a = img.base.wrapping_add(off);
                if self.is_code(a) {
                    add(a, SeedKind::Unwind, None);
                }
            }
        }
        for r in &self.relocations {
            let target = match r.kind {
                RelocKind::Relative => Some(r.addend as u64),
                RelocKind::Indirect => Some(r.addend as u64),
                RelocKind::Absolute | RelocKind::GlobalData | RelocKind::JumpSlot => self
                    .relocation_symbol(r)
                    .filter(|s| s.defined && s.kind != SymbolKind::Tls)
                    .map(|s| s.value.wrapping_add(r.addend as u64)),
                _ => None,
            };
            if let Some(t) = target.filter(|&t| self.is_code(t)) {
                add(t, SeedKind::Relocation, None);
            }
        }
        if self.kind == Kind::Fixed && self.relocations.is_empty() {
            for a in self.data_pointers() {
                add(a, SeedKind::Pointer, None);
            }
        }
        let mut lists: Vec<(u64, u64)> = self
            .sections
            .iter()
            .filter(|s| s.kind == SectionKind::CodeAddresses)
            .map(|s| (s.addr, s.size))
            .collect();
        if let Some(d) = &self.dynamic {
            lists.extend(
                [d.init_array, d.fini_array, d.preinit_array]
                    .into_iter()
                    .flatten(),
            );
            for a in [d.init, d.fini].into_iter().flatten() {
                if self.is_code(a) {
                    add(a, SeedKind::Initializer, None);
                }
            }
        }
        for (addr, size) in lists {
            for i in 0..size / 8 {
                if let Some(a) = self.read_u64(addr + i * 8)
                    && self.is_code(a)
                {
                    add(a, SeedKind::Initializer, None);
                }
            }
        }
        out.sort_by(|a, b| (a.addr, a.kind, &a.name).cmp(&(b.addr, b.kind, &b.name)));
        out.dedup_by(|a, b| a.addr == b.addr && a.kind == b.kind);
        out
    }

    /// The object moved up by `bias` bytes: every address, the addends
    /// that are addresses and the symbol values of addresses. Only a
    /// position-independent file moves.
    ///
    /// # Errors
    ///
    /// The file is linked for fixed addresses.
    pub fn rebase(&mut self, bias: u64) -> Result<(), Error> {
        if self.kind == Kind::Fixed && bias != 0 {
            return err("the file is linked for fixed addresses and cannot move");
        }
        let up = |a: &mut u64| *a = a.wrapping_add(bias);
        up(&mut self.entry);
        for s in &mut self.segments {
            up(&mut s.addr);
        }
        for s in &mut self.sections {
            up(&mut s.addr);
        }
        for s in self.symbols.iter_mut().chain(&mut self.dynamic_symbols) {
            if s.defined && s.kind != SymbolKind::Tls {
                up(&mut s.value);
            }
        }
        for r in &mut self.relocations {
            up(&mut r.offset);
            if matches!(r.kind, RelocKind::Relative | RelocKind::Indirect) {
                r.addend = (r.addend as u64).wrapping_add(bias) as i64;
            }
        }
        if let Some(t) = &mut self.tls {
            up(&mut t.addr);
        }
        if let Some(p) = &mut self.program_headers {
            up(&mut p.addr);
        }
        if let Some((a, _)) = &mut self.unwind {
            up(a);
        }
        if let Some(d) = &mut self.dynamic {
            for a in [&mut d.init, &mut d.fini].into_iter().flatten() {
                up(a);
            }
            for (a, _) in [&mut d.init_array, &mut d.fini_array, &mut d.preinit_array]
                .into_iter()
                .flatten()
            {
                up(a);
            }
        }
        Ok(())
    }
}
