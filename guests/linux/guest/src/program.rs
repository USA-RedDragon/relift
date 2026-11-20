//! A program as relift sees it: where it loads, the guest-neutral
//! [`Module`] analysis reads, and the C that hands the loaded image to the
//! runtime (`runtime/guests/linux/linux.h`).

use std::fmt::Write as _;

use relift_guest::{Module, Pad, Root, RootKind, Section, SectionKind};
use relift_objfile::{Image, Kind, Machine, Object, SeedKind, unwind};

/// The page size of the guest.
pub const PAGE: u64 = 0x1000;

/// Where a position-independent executable loads: its lowest address.
pub const PIE_BASE: u64 = 0x4000_0000;

/// The most address space a program's segments may span, so a file with
/// segments far apart is refused instead of held in one block.
pub const IMAGE_LIMIT: u64 = 1 << 30;

/// The size the main stack gets when the file does not say.
pub const DEFAULT_STACK: u64 = 8 << 20;

/// The program placed at its load address.
#[derive(Clone, Debug)]
pub struct Placed {
    /// The program's name.
    pub name: String,
    /// The object, moved to its load address.
    pub object: Object,
    /// The loaded bytes in one block from [`Image::base`].
    pub image: Image,
}

/// Places `object`: a file linked for fixed addresses stays where it is
/// linked, a position-independent one goes to [`PIE_BASE`].
///
/// # Errors
///
/// The file is not aarch64 code, has nothing to load, loads where the
/// guest has no memory, or spans more than [`IMAGE_LIMIT`].
pub fn place(name: &str, mut object: Object) -> Result<Placed, String> {
    if object.machine != Machine::Aarch64 {
        return Err(format!(
            "{name}: not an aarch64 program ({:?})",
            object.machine
        ));
    }
    if object.interpreter.is_some()
        || object
            .dynamic
            .as_ref()
            .is_some_and(|d| !d.needed.is_empty())
    {
        return Err(format!(
            "{name}: a dynamically linked program needs its libraries, which relift does not load yet; link it statically"
        ));
    }
    let low = object
        .low()
        .ok_or_else(|| format!("{name}: no loadable segment"))?;
    if object.kind == Kind::Relocatable {
        object
            .rebase(PIE_BASE.wrapping_sub(low & !0xffff))
            .map_err(|e| format!("{name}: {e}"))?;
    }
    let high = object.high().unwrap_or(0);
    if high > 1 << 36 {
        return Err(format!(
            "{name}: loads up to {high:#x}, past the guest's 36-bit address space"
        ));
    }
    let image = object
        .image(IMAGE_LIMIT)
        .map_err(|e| format!("{name}: {e}"))?;
    Ok(Placed {
        name: name.into(),
        object,
        image,
    })
}

/// The analysis view of `p`: its sections and the entries its symbols,
/// unwind table and relocations name.
pub fn module(p: &Placed) -> Module {
    let o = &p.object;
    let mut sections = Vec::new();
    let code: Vec<(u64, u64)> = if o.sections.is_empty() {
        o.segments
            .iter()
            .filter(|s| s.perm.exec)
            .map(|s| (s.addr, s.mem_size))
            .collect()
    } else {
        o.sections
            .iter()
            .filter(|s| s.exec)
            .map(|s| (s.addr, s.size))
            .collect()
    };
    for (addr, size) in code {
        sections.push(Section {
            addr,
            size,
            kind: SectionKind::Code,
        });
    }
    for s in o.segments.iter().filter(|s| !s.perm.exec) {
        sections.push(Section {
            addr: s.addr,
            size: s.mem_size,
            kind: if s.perm.write {
                SectionKind::Data
            } else {
                SectionKind::ReadOnly
            },
        });
    }
    let roots = o
        .seeds()
        .into_iter()
        .map(|s| Root {
            addr: s.addr,
            kind: match s.kind {
                SeedKind::Entry => RootKind::Entry,
                SeedKind::Symbol => RootKind::Symbol,
                SeedKind::Unwind => RootKind::Unwind,
                SeedKind::Relocation | SeedKind::Initializer | SeedKind::Pointer => {
                    RootKind::Pointer
                }
            },
            name: s.name,
        })
        .collect();
    let pads = o
        .unwind
        .and_then(|(hdr, _)| usize::try_from(hdr.checked_sub(p.image.base)?).ok())
        .map(|hdr| unwind::landing_pads(&p.image.bytes, hdr, p.image.base))
        .unwrap_or_default()
        .into_iter()
        .map(|l| Pad {
            site: l.site,
            len: l.len,
            addr: l.addr,
        })
        .collect();
    Module {
        name: p.name.clone(),
        base: p.image.base,
        image: p.image.bytes.clone(),
        sections,
        roots,
        pads,
    }
}

/// The C identifier of the lifted function at `addr`: its address, which
/// is unique where symbol names are not (local symbols, aliases).
pub fn host_name(addr: u64) -> String {
    format!("linux_{addr:08x}")
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

/// The files that describe `p` to the runtime and hold its image:
/// `image.bin`, `image.c` and `program.c`, which lists the lifted units
/// `units` (C symbols of `const rl_unit`) and carries `image_id`; `trampoline`
/// says the units were emitted in trampoline mode.
pub fn program_files(
    p: &Placed,
    units: &[String],
    image_id: [u8; 32],
    trampoline: bool,
) -> Vec<ProgramFile> {
    vec![
        ProgramFile {
            name: "image.bin".into(),
            bytes: p.image.bytes.clone(),
        },
        ProgramFile {
            name: "image.c".into(),
            bytes: relift_guest::blob::blob_c("rl_linux_image", "c/image.bin", &p.image.bytes)
                .into_bytes(),
        },
        ProgramFile {
            name: "program.c".into(),
            bytes: program_c(p, units, image_id, trampoline).into_bytes(),
        },
    ]
}

fn perm_c(s: &relift_objfile::Segment) -> &'static str {
    match (s.perm.write, s.perm.exec) {
        (true, true) => "RL_LINUX_RWX",
        (true, false) => "RL_LINUX_RW",
        (false, true) => "RL_LINUX_RX",
        (false, false) => "RL_LINUX_R",
    }
}

fn c_string(s: &str) -> String {
    let mut o = String::from("\"");
    for c in s.chars() {
        match c {
            '"' => o.push_str("\\\""),
            '\\' => o.push_str("\\\\"),
            c if c.is_ascii_graphic() || c == ' ' => o.push(c),
            _ => o.push('?'),
        }
    }
    o.push('"');
    o
}

/// `program.c`: the segments, the entry, the program headers, the stack
/// size, the units and `main`.
pub fn program_c(p: &Placed, units: &[String], image_id: [u8; 32], trampoline: bool) -> String {
    let o = &p.object;
    let mut t = String::from("#include <stdint.h>\n#include \"linux.h\"\n");
    if trampoline {
        t.push_str("#include \"arm64.h\"\n");
    }
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
    t.push_str(" };\nextern const uint8_t rl_linux_image[];\nstatic const rl_linux_segment segments[] = {\n");
    for s in &o.segments {
        let _ = writeln!(
            t,
            "    {{ UINT64_C({:#x}), UINT64_C({:#x}), UINT64_C({:#x}), UINT64_C({:#x}), {} }},",
            s.addr,
            s.mem_size,
            s.addr - p.image.base,
            s.data.len(),
            perm_c(s)
        );
    }
    let ph = o
        .program_headers
        .map_or((0, 0, 0), |h| (h.addr, h.count, h.entry_size));
    let stack = if o.stack_size == 0 {
        DEFAULT_STACK
    } else {
        o.stack_size
    };
    let id: Vec<String> = image_id.iter().map(|b| format!("0x{b:02x}")).collect();
    let _ = write!(
        t,
        "}};\nstatic const uint8_t image_id[32] = {{ {} }};\nconst rl_linux_program rl_program = {{\n    .name = {},\n    .segments = segments,\n    .segment_count = {}u,\n    .image = rl_linux_image,\n    .image_base = UINT64_C({:#x}),\n    .image_size = UINT64_C({:#x}),\n    .entry = UINT64_C({:#x}),\n    .phdr = UINT64_C({:#x}),\n    .phent = {}u,\n    .phnum = {}u,\n    .stack_size = UINT64_C({:#x}),\n    .hwcap = UINT64_C({:#x}),\n    .units = static_units,\n    .unit_count = {}u,\n    .image_id = image_id,\n}};\nRL_TEXT_BOUNDS_DEFINE()\nint main(int argc, char **argv)\n{{\n{}    return rl_linux_main(&rl_program, argc, argv);\n}}\n",
        id.join(", "),
        c_string(&p.name),
        o.segments.len(),
        p.image.base,
        p.image.bytes.len(),
        o.entry,
        ph.0,
        ph.2,
        ph.1,
        stack,
        crate::cpu::hwcap(),
        units.len(),
        if trampoline {
            "    rl_arm64_set_trampoline(1);\n"
        } else {
            ""
        },
    );
    t
}
