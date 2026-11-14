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

const PAGE: u64 = 0x1000;
const DT_NULL: u64 = 0;
const DT_PLTRELSZ: u64 = 2;
const DT_RELA: u64 = 7;
const DT_RELASZ: u64 = 8;
const DT_JMPREL: u64 = 23;
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
    if let Ok(Some(m)) = nso.mod0() {
        for a in unwind_starts(img, m.eh_frame_hdr_start as usize) {
            roots.push(Root {
                addr: p.base.wrapping_add(a),
                kind: RootKind::Unwind,
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

/// The values the module's relocations store, as image offsets.
fn reloc_targets(
    img: &[u8],
    dynamic: usize,
    symbols: &[relift_switch_formats::nso::Symbol],
) -> Vec<u64> {
    let mut tables = Vec::new();
    let (mut rela, mut relasz, mut jmprel, mut pltsz) = (None, 0, None, 0);
    let mut at = dynamic;
    while let (Some(tag), Some(v)) = (u64_at(img, at), u64_at(img, at + 8)) {
        match tag {
            DT_NULL => break,
            DT_RELA => rela = Some(v),
            DT_RELASZ => relasz = v,
            DT_JMPREL => jmprel = Some(v),
            DT_PLTRELSZ => pltsz = v,
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
    for (start, size) in tables {
        for i in 0..size / 24 {
            let o = (start + i * 24) as usize;
            let (Some(info), Some(addend)) = (u64_at(img, o + 8), u64_at(img, o + 16)) else {
                break;
            };
            let sym = (info >> 32) as usize;
            match info as u32 {
                R_AARCH64_RELATIVE => out.push(addend),
                R_AARCH64_ABS64 | R_AARCH64_GLOB_DAT | R_AARCH64_JUMP_SLOT => {
                    if let Some(s) = sym.checked_sub(1).and_then(|k| symbols.get(k))
                        && s.is_defined()
                    {
                        out.push(s.value.wrapping_add(addend));
                    }
                }
                _ => {}
            }
        }
    }
    out
}

/// The C file that describes the placed modules to the runtime's loader
/// and holds their images, with the process parameters from `npdm` and the
/// lifted unit tables `units` (C symbols of `const rl_unit`) to register.
pub fn program_c(placed: &[Placed], npdm: Option<&Npdm>, units: &[String]) -> String {
    let mut t = String::from("#include <stdint.h>\n#include \"loader.h\"\n");
    for u in units {
        let _ = writeln!(t, "extern const rl_unit {u};");
    }
    t.push_str("const rl_unit *const rl_static_units[] = {");
    for u in units {
        let _ = write!(t, " &{u},");
    }
    if units.is_empty() {
        t.push_str(" 0");
    }
    let _ = writeln!(
        t,
        " }};\nconst uint32_t rl_static_unit_count = {}u;",
        units.len()
    );
    for (i, p) in placed.iter().enumerate() {
        let _ = write!(t, "static const uint8_t image{i}[] = {{");
        for (k, b) in p.nso.image.iter().enumerate() {
            if k % 24 == 0 {
                t.push('\n');
            }
            let _ = write!(t, "{b},");
        }
        t.push_str("\n};\n");
    }
    t.push_str("static const rl_switch_module modules[] = {\n");
    for (i, p) in placed.iter().enumerate() {
        let s = p.nso.segments;
        let _ = writeln!(
            t,
            "    {{ {:?}, UINT64_C({:#x}), image{i}, {}u, {}u, {}u, {}u, {}u, {}u, {}u, {}u }},",
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
        "}};\nconst rl_switch_program rl_program = {{ modules, {}u, UINT64_C({entry:#x}), {space}u, {prio}, {core}, UINT64_C({stack:#x}), UINT64_C({pid:#x}) }};\n",
        placed.len()
    );
    t.push_str(
        "int main(int argc, char **argv) { return rl_switch_main(&rl_program, argc, argv); }\n",
    );
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
    fn program_c_names_the_loader_contract() {
        let p = place(vec![("main".into(), nso(vec![1, 2, 3, 4]))]);
        let c = program_c(&p, None, &["rl_unit_main".into()]);
        assert!(c.contains("const rl_unit *const rl_static_units[] = { &rl_unit_main, };"));
        assert!(c.contains("static const uint8_t image0[] = {\n1,2,3,4,"));
        assert!(c.contains(
            "const rl_switch_program rl_program = { modules, 1u, UINT64_C(0x8000000), 3u, 44, 0,"
        ));
        assert!(c.contains("rl_switch_main(&rl_program, argc, argv)"));
    }
}
