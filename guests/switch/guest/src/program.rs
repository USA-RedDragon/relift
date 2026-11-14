//! A Switch program as relift sees it: its modules laid out at their load
//! addresses, each turned into the guest-neutral [`Module`] analysis reads,
//! and the C that hands the loaded image to the runtime's loader
//! (`runtime/guests/switch/loader.h`).

use std::fmt::Write as _;

use relift_guest::{Module, Root, RootKind, Section, SectionKind};
use relift_switch_formats::npdm::{AddressSpace, Npdm};
use relift_switch_formats::nso::Nso;

/// Where the first module loads: the start of the code region of the 39-bit
/// and 36-bit address spaces, the runtime's default.
pub const CODE_START: u64 = 0x800_0000;

/// The general registers the runtime sets before it enters the program
/// start (the first module's entry), by number and value. `x0` is 0: rtld's
/// entry takes a nonzero `x0` as an exception to deliver and jumps to the
/// SDK's user exception handler, but the runtime enters the start once,
/// for the main thread (`rl_hos_run_main`), and never delivers an
/// exception (a guest fault ends in a crash report, and
/// `svcReturnFromException` is not implemented).
pub const START_REGS: [(u32, u64); 1] = [(0, 0)];

const PAGE: u64 = 0x1000;
const DT_NULL: u64 = 0;
const DT_PLTRELSZ: u64 = 2;
const DT_RELA: u64 = 7;
const DT_RELASZ: u64 = 8;
const DT_INIT: u64 = 12;
const DT_FINI: u64 = 13;
const DT_JMPREL: u64 = 23;
const DT_RELRSZ: u64 = 35;
const DT_RELR: u64 = 36;
const R_AARCH64_ABS64: u32 = 257;
const R_AARCH64_GLOB_DAT: u32 = 1025;
const R_AARCH64_JUMP_SLOT: u32 = 1026;
const R_AARCH64_RELATIVE: u32 = 1027;
const STT_FUNC: u8 = 2;

/// One module placed in the address space.
#[derive(Clone, Debug)]
pub struct Placed {
    /// The module name.
    pub name: String,
    /// Its load address.
    pub base: u64,
    /// The module.
    pub nso: Nso,
}

/// Places `modules` (in exefs load order) one after another from
/// [`CODE_START`], each page aligned.
pub fn place(modules: Vec<(String, Nso)>) -> Vec<Placed> {
    let mut at = CODE_START;
    modules
        .into_iter()
        .map(|(name, nso)| {
            let base = at;
            let size = nso.image.len() as u64 + u64::from(nso.bss_size);
            at = (base + size).div_ceil(PAGE) * PAGE;
            Placed { name, base, nso }
        })
        .collect()
}

fn u32_at(b: &[u8], o: usize) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(o..o + 4)?.try_into().ok()?))
}

fn u64_at(b: &[u8], o: usize) -> Option<u64> {
    Some(u64::from_le_bytes(b.get(o..o + 8)?.try_into().ok()?))
}

/// The analysis view of `p`: its sections and the entries its symbols,
/// unwind table and relocations name.
pub fn module(p: &Placed) -> Module {
    let nso = &p.nso;
    let seg = |i: usize| nso.segments[i];
    let mut sections = vec![
        Section {
            addr: p.base + u64::from(seg(0).memory_offset),
            size: u64::from(seg(0).size),
            kind: SectionKind::Code,
        },
        Section {
            addr: p.base + u64::from(seg(1).memory_offset),
            size: u64::from(seg(1).size),
            kind: SectionKind::ReadOnly,
        },
    ];
    sections.push(Section {
        addr: p.base + u64::from(seg(2).memory_offset),
        size: u64::from(seg(2).size) + u64::from(nso.bss_size),
        kind: SectionKind::Data,
    });
    let mut roots = vec![Root {
        addr: p.base + u64::from(seg(0).memory_offset),
        kind: RootKind::Entry,
        name: None,
    }];
    let symbols = nso.symbols().unwrap_or_default();
    for s in &symbols {
        if s.is_defined() && s.info & 0xf == STT_FUNC {
            roots.push(Root {
                addr: p.base + s.value,
                kind: RootKind::Symbol,
                name: Some(s.name.clone()),
            });
        }
    }
    let img = &nso.image;
    let mut pads = Vec::new();
    if let Ok(Some(m)) = nso.mod0() {
        pads = crate::eh::landing_pads(img, m.eh_frame_hdr_start as usize, p.base);
        for a in unwind_starts(img, m.eh_frame_hdr_start as usize) {
            roots.push(Root {
                addr: p.base.wrapping_add(a),
                kind: RootKind::Unwind,
                name: None,
            });
        }
        let text = nso.segments[0];
        for a in inline_literal_targets(img, text.memory_offset as usize, text.size as usize) {
            roots.push(Root {
                addr: p.base.wrapping_add(a),
                kind: RootKind::Pointer,
                name: None,
            });
        }
        for a in reloc_targets(img, m.dynamic as usize, &symbols) {
            roots.push(Root {
                addr: p.base.wrapping_add(a),
                kind: RootKind::Pointer,
                name: None,
            });
        }
    }
    Module {
        name: p.name.clone(),
        base: p.base,
        image: nso.image.clone(),
        sections,
        roots,
        pads,
    }
}

/// The function starts of the `.eh_frame_hdr` search table at image offset
/// `hdr`, as image offsets. Only the table encoding toolchains emit
/// (`DW_EH_PE_datarel | DW_EH_PE_sdata4`) is read.
fn unwind_starts(img: &[u8], hdr: usize) -> Vec<u64> {
    let (Some(&ver), Some(&count_enc), Some(&table_enc)) =
        (img.get(hdr), img.get(hdr + 2), img.get(hdr + 3))
    else {
        return Vec::new();
    };
    if ver != 1 || table_enc != 0x3b || count_enc != 0x03 {
        return Vec::new();
    }
    let Some(count) = u32_at(img, hdr + 8) else {
        return Vec::new();
    };
    (0..count as usize)
        .filter_map(|i| {
            let loc = u32_at(img, hdr + 12 + i * 8)? as i32;
            u64::try_from(hdr as i64 + i64::from(loc)).ok()
        })
        .collect()
}

/// The code addresses position-independent start-up code computes with an
/// inline literal, as image offsets: `bl` over one data word, then
/// `ldr wN, [x30]`, `sxtw xN, wN`, `add xN, xN, x30` give the word's
/// address plus the word. Code before relocation uses this (the dynamic
/// loader and the runtime start-up) because it has no GOT to read yet.
fn inline_literal_targets(img: &[u8], text: usize, size: usize) -> Vec<u64> {
    const BL_PLUS_8: u32 = 0x9400_0002;
    let end = (text + size).min(img.len());
    let mut out = Vec::new();
    let mut pc = text;
    while pc + 20 <= end {
        let (Some(bl), Some(word), Some(ldr), Some(sxtw), Some(add)) = (
            u32_at(img, pc),
            u32_at(img, pc + 4),
            u32_at(img, pc + 8),
            u32_at(img, pc + 12),
            u32_at(img, pc + 16),
        ) else {
            break;
        };
        let n = ldr & 0x1f;
        if bl == BL_PLUS_8
            && ldr == 0xb940_03c0 | n
            && sxtw == 0x9340_7c00 | (n << 5) | n
            && add == 0x8b1e_0000 | (n << 5) | n
        {
            let at = (pc + 4) as i64 + i64::from(word as i32);
            if let Ok(t) = u64::try_from(at)
                && (text as u64..end as u64).contains(&t)
                && t % 4 == 0
            {
                out.push(t);
            }
        }
        pc += 4;
    }
    out
}

/// The values the module's relocations store, as image offsets. A PLT
/// slot also names the word it holds before binding: with lazy binding the
/// loader only adds the base to it, so it is the PLT header's address, code
/// that nothing else reaches.
fn reloc_targets(
    img: &[u8],
    dynamic: usize,
    symbols: &[relift_switch_formats::nso::Symbol],
) -> Vec<u64> {
    let mut out = Vec::new();
    for r in relocs(img, dynamic) {
        match r {
            Reloc::Value(v) => out.push(v),
            Reloc::Symbol {
                at,
                sym,
                addend,
                kind,
            } => {
                if kind == SlotKind::Plt
                    && let Some(lazy) = u64_at(img, at as usize).filter(|&w| w != 0)
                {
                    out.push(lazy);
                }
                if let Some(s) = sym
                    .checked_sub(1)
                    .and_then(|k| symbols.get(k))
                    .filter(|s| s.is_defined())
                {
                    out.push(s.value.wrapping_add(addend));
                }
            }
        }
    }
    out
}

/// One relocation, as image offsets.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Reloc {
    /// A relative relocation storing this value.
    Value(u64),
    /// A symbol's address (plus `addend`) stored at `at`.
    Symbol {
        at: u64,
        sym: usize,
        addend: u64,
        kind: SlotKind,
    },
}

/// The relocations of the module whose dynamic section is at image offset
/// `dynamic`.
fn relocs(img: &[u8], dynamic: usize) -> Vec<Reloc> {
    let mut tables = Vec::new();
    let (mut rela, mut relasz, mut jmprel, mut pltsz) = (None, 0, None, 0);
    let (mut relr, mut relrsz) = (None, 0);
    let mut at = dynamic;
    while let (Some(tag), Some(v)) = (u64_at(img, at), u64_at(img, at + 8)) {
        match tag {
            DT_NULL => break,
            DT_RELA => rela = Some(v),
            DT_RELASZ => relasz = v,
            DT_JMPREL => jmprel = Some(v),
            DT_PLTRELSZ => pltsz = v,
            DT_RELR => relr = Some(v),
            DT_RELRSZ => relrsz = v,
            _ => {}
        }
        at += 16;
    }
    if let Some(r) = rela {
        tables.push((r, relasz));
    }
    if let Some(r) = jmprel {
        tables.push((r, pltsz));
    }
    let mut out = Vec::new();
    if let Some(r) = relr {
        out.extend(relr_targets(img, r, relrsz).into_iter().map(Reloc::Value));
    }
    for (start, size) in tables {
        for i in 0..size / 24 {
            let o = (start + i * 24) as usize;
            let (Some(off), Some(info), Some(addend)) =
                (u64_at(img, o), u64_at(img, o + 8), u64_at(img, o + 16))
            else {
                break;
            };
            let sym = (info >> 32) as usize;
            let kind = match info as u32 {
                R_AARCH64_RELATIVE => {
                    out.push(Reloc::Value(addend));
                    continue;
                }
                R_AARCH64_ABS64 => SlotKind::Data,
                R_AARCH64_GLOB_DAT => SlotKind::Got,
                R_AARCH64_JUMP_SLOT => SlotKind::Plt,
                _ => continue,
            };
            out.push(Reloc::Symbol {
                at: off,
                sym,
                addend,
                kind,
            });
        }
    }
    out
}

/// What kind of word a symbol relocation fills.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SlotKind {
    /// A PLT entry's GOT word (`R_AARCH64_JUMP_SLOT`), read only by its
    /// PLT stub.
    Plt,
    /// A GOT word (`R_AARCH64_GLOB_DAT`).
    Got,
    /// Any other word (`R_AARCH64_ABS64`), which may carry an addend.
    Data,
}

/// A word the loader fills with a symbol's address.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Slot {
    /// Its guest address.
    pub addr: u64,
    /// The symbol's name.
    pub name: String,
    /// The kind of word.
    pub kind: SlotKind,
}

/// The loader-visible linkage of placed module `p`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Linkage {
    /// The words the loader fills with a symbol's address, by name.
    pub slots: Vec<Slot>,
    /// The guest addresses relative relocations store.
    pub values: Vec<u64>,
    /// The functions the module defines for other modules, by name and
    /// guest address.
    pub exports: Vec<(String, u64)>,
}

/// The linkage of `p`: what the loader resolves by name and the pointers
/// it relocates. The loader binds a symbol slot to a definition of that
/// name in some module, so a symbol's possible targets are all its
/// definitions in the program.
pub fn linkage(p: &Placed) -> Linkage {
    let symbols = p.nso.symbols().unwrap_or_default();
    let mut out = Linkage {
        exports: symbols
            .iter()
            .filter(|s| s.is_defined() && s.info & 0xf == STT_FUNC)
            .map(|s| (s.name.clone(), p.base + s.value))
            .collect(),
        ..Linkage::default()
    };
    let Ok(Some(m)) = p.nso.mod0() else {
        return out;
    };
    // The loader calls DT_INIT and DT_FINI by the address the dynamic
    // section holds, which no relocation names.
    let mut at = m.dynamic as usize;
    while let (Some(tag), Some(v)) = (u64_at(&p.nso.image, at), u64_at(&p.nso.image, at + 8)) {
        match tag {
            DT_NULL => break,
            DT_INIT | DT_FINI => out.values.push(p.base.wrapping_add(v)),
            _ => {}
        }
        at += 16;
    }
    for r in relocs(&p.nso.image, m.dynamic as usize) {
        match r {
            Reloc::Value(v) => out.values.push(p.base.wrapping_add(v)),
            Reloc::Symbol {
                at,
                sym,
                addend,
                kind,
            } => {
                let Some(s) = sym.checked_sub(1).and_then(|k| symbols.get(k)) else {
                    continue;
                };
                // A symbol word with an addend points into its target.
                let kind = if addend == 0 { kind } else { SlotKind::Data };
                out.slots.push(Slot {
                    addr: p.base.wrapping_add(at),
                    name: s.name.clone(),
                    kind,
                });
            }
        }
    }
    out
}

/// The values the packed relative relocations (`DT_RELR`) at image offset
/// `start` adjust: each relocated word holds its target as an image offset.
fn relr_targets(img: &[u8], start: u64, size: u64) -> Vec<u64> {
    let mut out = Vec::new();
    let mut at: u64 = 0;
    let word = |o: u64| usize::try_from(o).ok().and_then(|o| u64_at(img, o));
    for i in 0..size / 8 {
        let Some(e) = word(start + i * 8) else {
            break;
        };
        if e & 1 == 0 {
            out.extend(word(e));
            at = e + 8;
        } else {
            for bit in 1..64 {
                if e >> bit & 1 == 1 {
                    out.extend(word(at + (bit - 1) * 8));
                }
            }
            at += 63 * 8;
        }
    }
    out
}

/// The host identifier of the function at `addr` in placed module `p`,
/// whose guest symbol is `symbol` (docs/abi.md section 11): the Itanium
/// rewrite into `guest::<module>`, or the address name when there is no
/// symbol or it cannot be rewritten exactly.
pub fn host_name(p: &Placed, symbol: Option<&str>, addr: u64) -> String {
    let module = c_module(&p.name);
    symbol
        .and_then(|s| relift_switch_names::host_name(&module, s))
        .unwrap_or_else(|| relift_switch_names::fallback_name(&module, addr - p.base))
}

/// The module name as a C identifier, for host names.
pub fn c_module(name: &str) -> String {
    let s: String = name
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '_' })
        .collect();
    if s.starts_with(|c: char| c.is_ascii_digit()) || s.is_empty() {
        format!("m{s}")
    } else {
        s
    }
}

/// A guest libc routine a host override replaces, recognized by its code.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Known {
    /// The host function (`relift/a64/abi.h`).
    pub host_fn: &'static str,
    /// Whether it may return `RL_PENDING`.
    pub may_unwind: bool,
    /// Whether a call to it is a resume point (`setjmp`).
    pub resume_point: bool,
}

const BTI_C: u32 = 0xd503_245f;

/// newlib's AArch64 `setjmp`: x19 to x30, sp and d8 to d15 into the buffer
/// at x0 (the layout `rl_a64_jmp_newlib` reads), then 0.
const NEWLIB_SETJMP: [u32; 15] = [
    0x9100_03f0,
    0xa900_5013,
    0xa901_5815,
    0xa902_6017,
    0xa903_6819,
    0xa904_701b,
    0xa905_781d,
    0xf900_3010,
    0x6d07_2408,
    0x6d08_2c0a,
    0x6d09_340c,
    0x6d0a_3c0e,
    0x5280_0000,
    0xd65f_03c0,
    0xd503_201f,
];

/// newlib's AArch64 `longjmp`.
const NEWLIB_LONGJMP: [u32; 16] = [
    0xa940_5013,
    0xa941_5815,
    0xa942_6017,
    0xa943_6819,
    0xa944_701b,
    0xa945_781d,
    0xf940_3010,
    0x6d47_2408,
    0x6d48_2c0a,
    0x6d49_340c,
    0x6d4a_3c0e,
    0x9100_021f,
    0x7100_003f,
    0x1a81_1420,
    0xd61f_03c0,
    0xd503_201f,
];

/// The override for the code at `bytes`, if it is a routine this knows.
pub fn known(bytes: &[u8]) -> Option<Known> {
    let words: Vec<u32> = bytes
        .as_chunks::<4>()
        .0
        .iter()
        .take(17)
        .map(|w| u32::from_le_bytes(*w))
        .collect();
    let body = match words.first() {
        Some(&BTI_C) => &words[1..],
        _ => &words[..],
    };
    let is =
        |pat: &[u32]| body.len() >= pat.len() - 1 && body[..pat.len() - 1] == pat[..pat.len() - 1];
    if is(&NEWLIB_SETJMP) {
        return Some(Known {
            host_fn: "rl_a64_setjmp",
            may_unwind: false,
            resume_point: true,
        });
    }
    if is(&NEWLIB_LONGJMP) {
        return Some(Known {
            host_fn: "rl_a64_longjmp",
            may_unwind: true,
            resume_point: false,
        });
    }
    None
}

/// One file of the program description: its name in the C directory and
/// its bytes.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ProgramFile {
    /// The file name, relative to the C directory.
    pub name: String,
    /// The contents.
    pub bytes: Vec<u8>,
}

impl ProgramFile {
    /// Whether the build compiles this file.
    pub fn is_c(&self) -> bool {
        self.name.ends_with(".c")
    }
}

/// The C file that holds module `i`'s image: the raw bytes go in
/// `image<i>.bin` and the C pulls them in with `#embed`, so the compiler
/// reads them as data rather than parsing an initializer of millions of
/// numbers.
fn image_c(i: usize) -> String {
    format!(
        "#include <stdint.h>\n#pragma clang diagnostic ignored \"-Wc23-extensions\"\nextern const uint8_t rl_image{i}[];\nconst uint8_t rl_image{i}[] = {{\n#embed \"image{i}.bin\"\n}};\n"
    )
}

/// The files that describe the placed modules to the runtime's loader and
/// hold their images, with the process parameters from `npdm` and the
/// lifted unit tables `units` (C symbols of `const rl_unit`) to register:
/// `program.c`, and per module `image<i>.c` with its `image<i>.bin`.
/// `fp_flags` turns on FPSR flag production in the runtime before the
/// program starts (the build's FPSR census found a read that can be used).
pub fn program_files(
    placed: &[Placed],
    npdm: Option<&Npdm>,
    units: &[String],
    fp_flags: bool,
) -> Vec<ProgramFile> {
    let mut out = Vec::new();
    for (i, p) in placed.iter().enumerate() {
        out.push(ProgramFile {
            name: format!("image{i}.bin"),
            bytes: p.nso.image.clone(),
        });
        out.push(ProgramFile {
            name: format!("image{i}.c"),
            bytes: image_c(i).into_bytes(),
        });
    }
    out.push(ProgramFile {
        name: "program.c".into(),
        bytes: program_c(placed, npdm, units, fp_flags).into_bytes(),
    });
    out
}

/// `program.c`: the module table, the process parameters and `main`. The
/// images live in the `image<i>.c` files [`program_files`] adds.
fn program_c(placed: &[Placed], npdm: Option<&Npdm>, units: &[String], fp_flags: bool) -> String {
    let mut t = String::from("#include <stdint.h>\n#include \"loader.h\"\n");
    for u in units {
        let _ = writeln!(t, "extern const rl_unit {u};");
    }
    t.push_str("static const rl_unit *const static_units[] = {");
    for u in units {
        let _ = write!(t, " &{u},");
    }
    if units.is_empty() {
        t.push_str(" 0");
    }
    let _ = writeln!(t, " }};");
    for i in 0..placed.len() {
        let _ = writeln!(t, "extern const uint8_t rl_image{i}[];");
    }
    t.push_str("static const rl_switch_module modules[] = {\n");
    for (i, p) in placed.iter().enumerate() {
        let s = p.nso.segments;
        let _ = writeln!(
            t,
            "    {{ {:?}, UINT64_C({:#x}), rl_image{i}, {}u, {}u, {}u, {}u, {}u, {}u, {}u, {}u }},",
            p.name,
            p.base,
            p.nso.image.len(),
            s[0].memory_offset,
            s[0].size,
            s[1].memory_offset,
            s[1].size,
            s[2].memory_offset,
            s[2].size,
            p.nso.bss_size
        );
    }
    let (space, prio, core, stack, pid) = match npdm {
        Some(n) => (
            match n.address_space {
                AddressSpace::As32Bit => 0,
                AddressSpace::As64BitOld => 1,
                AddressSpace::As32BitNoReserved => 2,
                AddressSpace::As64Bit => 3,
            },
            i32::from(n.main_thread_priority),
            i32::from(n.main_thread_core),
            u64::from(n.main_thread_stack_size),
            n.program_id,
        ),
        None => (3, 44, 0, 0, 0),
    };
    let entry = placed
        .first()
        .map(|p| p.base + u64::from(p.nso.segments[0].memory_offset))
        .unwrap_or(CODE_START);
    let _ = write!(
        t,
        "}};\nconst rl_switch_program rl_program = {{ modules, {}u, UINT64_C({entry:#x}), {space}u, {prio}, {core}, UINT64_C({stack:#x}), UINT64_C({pid:#x}), static_units, {}u }};\n",
        placed.len(),
        units.len()
    );
    t.push_str("RL_TEXT_BOUNDS_DEFINE()\n");
    if fp_flags {
        t.push_str("#include \"a64.h\"\nint main(int argc, char **argv) { rl_a64_set_fp_flags(1); return rl_switch_main(&rl_program, argc, argv); }\n");
    } else {
        t.push_str(
            "int main(int argc, char **argv) { return rl_switch_main(&rl_program, argc, argv); }\n",
        );
    }
    t
}

#[cfg(test)]
mod tests {
    use super::*;
    use relift_switch_formats::nso::{NsoSpec, build};

    fn nso(text: Vec<u8>) -> Nso {
        Nso::parse(&build(&NsoSpec {
            text,
            rodata: vec![0; 16],
            data: vec![0; 16],
            bss: 0x20,
            ..NsoSpec::default()
        }))
        .expect("parses")
    }

    #[test]
    fn modules_are_placed_page_aligned_from_the_code_start() {
        let p = place(vec![
            ("a".into(), nso(vec![0; 8])),
            ("b".into(), nso(vec![0; 8])),
        ]);
        assert_eq!(p[0].base, CODE_START);
        assert!(p[1].base > CODE_START && p[1].base.is_multiple_of(PAGE));
        let m = module(&p[0]);
        assert_eq!(m.roots[0].addr, CODE_START);
        assert_eq!(m.roots[0].kind, RootKind::Entry);
        assert!(m.is_code(CODE_START + 4));
        assert_eq!(m.sections[2].kind, SectionKind::Data);
    }

    #[test]
    fn unwind_table_and_relocations_give_entries() {
        let mut img = vec![0u8; 0x100];
        img[0x40..0x44].copy_from_slice(&[1, 0x1b, 0x03, 0x3b]);
        img[0x48..0x4c].copy_from_slice(&2u32.to_le_bytes());
        img[0x4c..0x50].copy_from_slice(&(-0x30i32).to_le_bytes());
        img[0x54..0x58].copy_from_slice(&(-0x20i32).to_le_bytes());
        assert_eq!(unwind_starts(&img, 0x40), [0x10, 0x20]);
        let dynamic = 0x80;
        for (k, (tag, v)) in [(DT_RELA, 0xb0u64), (DT_RELASZ, 24), (DT_NULL, 0)]
            .into_iter()
            .enumerate()
        {
            img[dynamic + k * 16..dynamic + k * 16 + 8].copy_from_slice(&tag.to_le_bytes());
            img[dynamic + k * 16 + 8..dynamic + k * 16 + 16].copy_from_slice(&v.to_le_bytes());
        }
        img[0xb8..0xc0].copy_from_slice(&u64::from(R_AARCH64_RELATIVE).to_le_bytes());
        img[0xc0..0xc8].copy_from_slice(&0x24u64.to_le_bytes());
        assert_eq!(reloc_targets(&img, dynamic, &[]), [0x24]);
    }

    #[test]
    fn symbol_relocations_name_their_slots() {
        let mut img = vec![0u8; 0x200];
        let put =
            |img: &mut Vec<u8>, o: usize, v: u64| img[o..o + 8].copy_from_slice(&v.to_le_bytes());
        for (k, (tag, v)) in [(DT_RELA, 0x100u64), (DT_RELASZ, 72), (DT_NULL, 0)]
            .into_iter()
            .enumerate()
        {
            put(&mut img, k * 16, tag);
            put(&mut img, k * 16 + 8, v);
        }
        let rel = [
            (0x40, R_AARCH64_GLOB_DAT, 1, 0),
            (0x48, R_AARCH64_JUMP_SLOT, 2, 0),
            (0x50, R_AARCH64_ABS64, 1, 8),
        ];
        for (i, (at, kind, sym, addend)) in rel.into_iter().enumerate() {
            let o = 0x100 + i * 24;
            put(&mut img, o, at);
            put(&mut img, o + 8, (sym << 32) | u64::from(kind));
            put(&mut img, o + 16, addend);
        }
        let r = relocs(&img, 0);
        assert_eq!(
            r,
            [
                Reloc::Symbol {
                    at: 0x40,
                    sym: 1,
                    addend: 0,
                    kind: SlotKind::Got
                },
                Reloc::Symbol {
                    at: 0x48,
                    sym: 2,
                    addend: 0,
                    kind: SlotKind::Plt
                },
                Reloc::Symbol {
                    at: 0x50,
                    sym: 1,
                    addend: 8,
                    kind: SlotKind::Data
                },
            ]
        );
        // Unbound, the PLT slot holds the PLT header's address: an entry.
        put(&mut img, 0x48, 0x30);
        assert_eq!(reloc_targets(&img, 0, &[]), [0x30]);
    }

    #[test]
    fn inline_literals_name_code_addresses() {
        let mut img = Vec::new();
        for w in [
            0xd503_201fu32, // 0x00 nop
            0x9400_0002,    // 0x04 bl 0x0c
            0x0000_0010,    // 0x08 .word 0x10 -> 0x18
            0xb940_03c2,    // 0x0c ldr w2, [x30]
            0x9340_7c42,    // 0x10 sxtw x2, w2
            0x8b1e_0042,    // 0x14 add x2, x2, x30
            0x9400_0002,    // 0x18 bl 0x20
            0x0000_1000,    // 0x1c .word: outside the text
            0xb940_03c3,    // 0x20 ldr w3, [x30]
            0x9340_7c63,    // 0x24 sxtw x3, w3
            0x8b1e_0063,    // 0x28 add x3, x3, x30
        ] {
            img.extend_from_slice(&w.to_le_bytes());
        }
        assert_eq!(inline_literal_targets(&img, 0, img.len()), [0x18]);
    }

    #[test]
    fn packed_relative_relocations() {
        let mut img = vec![0u8; 0x200];
        let put =
            |img: &mut Vec<u8>, o: usize, v: u64| img[o..o + 8].copy_from_slice(&v.to_le_bytes());
        put(&mut img, 0x100, 0x11);
        put(&mut img, 0x108, 0x22);
        put(&mut img, 0x118, 0x33);
        put(&mut img, 0x180, 0x100);
        put(&mut img, 0x188, 0b1011);
        assert_eq!(relr_targets(&img, 0x180, 16), [0x11, 0x22, 0x33]);
    }

    #[test]
    fn newlib_setjmp_and_longjmp_are_recognized() {
        let bytes = |w: &[u32]| w.iter().flat_map(|x| x.to_le_bytes()).collect::<Vec<u8>>();
        let mut sj = vec![BTI_C];
        sj.extend_from_slice(&NEWLIB_SETJMP);
        assert_eq!(known(&bytes(&sj)).map(|k| k.host_fn), Some("rl_a64_setjmp"));
        let lj = known(&bytes(&NEWLIB_LONGJMP)).expect("longjmp");
        assert!(lj.may_unwind && !lj.resume_point);
        sj[3] ^= 1;
        assert_eq!(known(&bytes(&sj)), None);
    }

    #[test]
    fn host_names_fall_back_to_module_offsets() {
        let p = &place(vec![("main".into(), nso(vec![0; 8]))])[0];
        assert_eq!(
            host_name(p, None, CODE_START + 0x40),
            "_ZN5guest4main11fn_00000040E"
        );
        assert_eq!(
            host_name(p, Some("_ZN3foo3barEv"), CODE_START),
            "_ZN5guest4main3foo3barEv"
        );
    }

    #[test]
    fn program_c_names_the_loader_contract() {
        let p = place(vec![("main".into(), nso(vec![1, 2, 3, 4]))]);
        let files = program_files(&p, None, &["rl_unit_main".into()], false);
        let names: Vec<&str> = files.iter().map(|f| f.name.as_str()).collect();
        assert_eq!(names, ["image0.bin", "image0.c", "program.c"]);
        assert_eq!(files[0].bytes, p[0].nso.image);
        let img = String::from_utf8_lossy(&files[1].bytes);
        assert!(img.contains("#embed \"image0.bin\""), "{img}");
        assert!(img.contains("const uint8_t rl_image0[] = {"), "{img}");
        let c = String::from_utf8_lossy(&files[2].bytes);
        assert!(c.contains("static const rl_unit *const static_units[] = { &rl_unit_main, };"));
        assert!(c.contains("extern const uint8_t rl_image0[];"));
        assert!(c.contains("UINT64_C(0x8000000), rl_image0, "));
        assert!(c.contains(
            "const rl_switch_program rl_program = { modules, 1u, UINT64_C(0x8000000), 3u, 44, 0,"
        ));
        assert!(c.contains("rl_switch_main(&rl_program, argc, argv)"));
    }
}
