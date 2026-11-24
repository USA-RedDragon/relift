//! A program as relift sees it: where it loads, the guest-neutral
//! [`Module`] analysis reads, and the C that hands the loaded image to the
//! runtime (`runtime/guests/linux/linux.h`).

use std::collections::BTreeMap;
use std::fmt::Write as _;

use relift_guest::{Module, Pad, Root, RootKind, Section, SectionKind};
use relift_objfile::{Image, Kind, Machine, Object, SeedKind, elf, unwind};

/// The page size of the guest.
pub const PAGE: u64 = 0x1000;

/// Where a position-independent executable loads: its lowest address.
pub const PIE_BASE: u64 = 0x4000_0000;

/// The most address space a program's segments may span, so a file with
/// segments far apart is refused instead of held in one block.
pub const IMAGE_LIMIT: u64 = 1 << 30;

/// The size the main stack gets when the file does not say.
pub const DEFAULT_STACK: u64 = 8 << 20;

/// The alignment of a library's base: what the loader gives a file whose
/// segments ask for less.
pub const LIBRARY_ALIGN: u64 = 0x1_0000;

/// What a file is to the process.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Role {
    /// The executable the kernel starts.
    Main,
    /// The program interpreter: the kernel maps it and enters it.
    Interpreter,
    /// A library the interpreter maps from the file system.
    Library,
}

/// A file of the program placed at its load address.
#[derive(Clone, Debug)]
pub struct Placed {
    /// The file's name.
    pub name: String,
    /// What the file is to the process.
    pub role: Role,
    /// The guest paths the file answers to when opened.
    pub paths: Vec<String>,
    /// The file as it is on disk.
    pub file: Vec<u8>,
    /// The alignment of the load address.
    pub align: u64,
    /// The object, moved to its load address.
    pub object: Object,
    /// The loaded bytes in one block from [`Image::base`].
    pub image: Image,
}

/// A file the program reads that is not code (the configuration of a
/// plugin directory), served to the program's reads by path.
#[derive(Clone, Debug)]
pub struct Data {
    /// The guest paths the file answers to when opened.
    pub paths: Vec<String>,
    /// The file.
    pub bytes: Vec<u8>,
}

/// The files of a program placed: the code, and the data it reads.
#[derive(Clone, Debug)]
pub struct Program {
    /// The executable, the interpreter and the libraries, placed.
    pub modules: Vec<Placed>,
    /// The data files.
    pub data: Vec<Data>,
    /// The host directory that is the guest's root file system (the
    /// import's sysroot), for `rl_linux_program.root`; `None` lets the
    /// program see the host's paths as they are.
    pub root: Option<String>,
}

/// A file to place.
#[derive(Clone, Debug)]
pub struct Input {
    /// The file's name.
    pub name: String,
    /// What the file is to the process.
    pub role: Role,
    /// The guest paths the file answers to when opened.
    pub paths: Vec<String>,
    /// The file.
    pub bytes: Vec<u8>,
}

fn round_up(v: u64, to: u64) -> u64 {
    v.div_ceil(to) * to
}

/// Places the files of a program: the executable (the first input) where
/// it is linked or at [`PIE_BASE`], then the others one after another from
/// the end of the executable, each aligned to [`LIBRARY_ALIGN`] or its
/// segments' alignment. The layout depends on the files and their order
/// only, so lifted code and the run agree on every address.
///
/// # Errors
///
/// A file is not aarch64 code, has nothing to load, loads where the guest
/// has no memory, spans more than [`IMAGE_LIMIT`], or is a fixed-address
/// file placed after the executable; or the executable is dynamically
/// linked and no other file was given.
pub fn place(inputs: Vec<Input>, data: Vec<Data>) -> Result<Program, String> {
    let mut out: Vec<Placed> = Vec::new();
    let mut cursor = 0;
    let count = inputs.len();
    for input in inputs {
        let name = input.name.clone();
        let mut object = elf::parse(&input.bytes).map_err(|e| format!("{name}: {e}"))?;
        if object.machine != Machine::Aarch64 {
            return Err(format!(
                "{name}: not an aarch64 program ({:?})",
                object.machine
            ));
        }
        if out.is_empty()
            && count == 1
            && (object.interpreter.is_some()
                || object
                    .dynamic
                    .as_ref()
                    .is_some_and(|d| !d.needed.is_empty()))
        {
            return Err(format!(
                "{name}: a dynamically linked program needs its interpreter and libraries; import it with --sysroot"
            ));
        }
        let low = object
            .low()
            .ok_or_else(|| format!("{name}: no loadable segment"))?;
        let align = object
            .segments
            .iter()
            .map(|s| s.align)
            .max()
            .unwrap_or(1)
            .clamp(LIBRARY_ALIGN, 1 << 21);
        let want = if out.is_empty() {
            (object.kind == Kind::Relocatable).then_some(PIE_BASE)
        } else {
            if object.kind != Kind::Relocatable {
                return Err(format!(
                    "{name}: linked for fixed addresses, but loaded after the executable"
                ));
            }
            Some(round_up(cursor, align))
        };
        if let Some(base) = want {
            object
                .rebase(base.wrapping_sub(low & !(align - 1)))
                .map_err(|e| format!("{name}: {e}"))?;
        }
        let high = object.high().unwrap_or(0);
        if high > 1 << 36 {
            return Err(format!(
                "{name}: loads up to {high:#x}, past the guest's 36-bit address space"
            ));
        }
        cursor = high + LIBRARY_ALIGN;
        let image = object
            .image(IMAGE_LIMIT)
            .map_err(|e| format!("{name}: {e}"))?;
        out.push(Placed {
            name: input.name,
            role: input.role,
            paths: input.paths,
            file: input.bytes,
            align,
            object,
            image,
        });
    }
    Ok(Program {
        modules: out,
        data,
        root: None,
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

/// The files that describe the program `modules` to the runtime and hold
/// its files: per module `module<N>.bin` and `module<N>.c`, and
/// `program.c`, which lists the lifted units `units` (C symbols of
/// `const rl_unit`), the guest path of the executable and `image_id`;
/// `trampoline` says the units were emitted in trampoline mode and
/// `code_maps` lists the cold copies' maps.
pub fn program_files(
    program: &Program,
    cpu: crate::cpu::GuestCpu,
    exec_path: &str,
    (units, code_maps): (&[String], &[String]),
    image_id: [u8; 32],
    trampoline: bool,
) -> Vec<ProgramFile> {
    let mut out = Vec::new();
    let files = program
        .modules
        .iter()
        .map(|m| &m.file)
        .chain(program.data.iter().map(|d| &d.bytes));
    for (i, bytes) in files.enumerate() {
        out.push(ProgramFile {
            name: format!("module{i}.bin"),
            bytes: bytes.clone(),
        });
        out.push(ProgramFile {
            name: format!("module{i}.c"),
            bytes: relift_guest::blob::blob_c(
                &format!("rl_linux_file_{i}"),
                &format!("c/module{i}.bin"),
                bytes,
            )
            .into_bytes(),
        });
    }
    out.push(ProgramFile {
        name: "program.c".into(),
        bytes: program_c(
            program,
            cpu,
            exec_path,
            (units, code_maps),
            image_id,
            trampoline,
        )
        .into_bytes(),
    });
    out
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

fn role_c(r: Role) -> &'static str {
    match r {
        Role::Main => "RL_LINUX_MAIN",
        Role::Interpreter => "RL_LINUX_INTERPRETER",
        Role::Library => "RL_LINUX_LIBRARY",
    }
}

/// `program.c`: per module its segments, paths, entry, program headers and
/// file, then the stack size, the units and `main`.
pub fn program_c(
    program: &Program,
    cpu: crate::cpu::GuestCpu,
    exec_path: &str,
    (units, code_maps): (&[String], &[String]),
    image_id: [u8; 32],
    trampoline: bool,
) -> String {
    let mut t = String::from("#include <stdint.h>\n#include \"linux.h\"\n");
    if trampoline {
        t.push_str("#include \"arm64.h\"\n");
    }
    for u in units {
        let _ = writeln!(t, "extern const rl_unit {u};");
    }
    for m in code_maps {
        let _ = writeln!(t, "extern const rl_code_map {m};");
    }
    t.push_str("static const rl_code_map *const code_maps[] = {");
    for m in code_maps {
        let _ = write!(t, " &{m},");
    }
    if code_maps.is_empty() {
        t.push_str(" 0");
    }
    t.push_str(" };\n");
    t.push_str("static const rl_unit *const static_units[] = {");
    for u in units {
        let _ = write!(t, " &{u},");
    }
    if units.is_empty() {
        t.push_str(" 0");
    }
    t.push_str(" };\n");
    let modules = &program.modules;
    for (i, p) in modules.iter().enumerate() {
        let o = &p.object;
        let _ = writeln!(t, "extern const uint8_t rl_linux_file_{i}[];");
        let _ = writeln!(t, "static const rl_linux_segment segments_{i}[] = {{");
        for s in &o.segments {
            let _ = writeln!(
                t,
                "    {{ UINT64_C({:#x}), UINT64_C({:#x}), UINT64_C({:#x}), UINT64_C({:#x}), {} }},",
                s.addr,
                s.mem_size,
                s.offset,
                s.data.len(),
                perm_c(s)
            );
        }
        let _ = writeln!(t, "}};");
        let _ = write!(t, "static const char *const paths_{i}[] = {{");
        for path in &p.paths {
            let _ = write!(t, " {},", c_string(path));
        }
        if p.paths.is_empty() {
            t.push_str(" 0");
        }
        t.push_str(" };\n");
    }
    for (k, d) in program.data.iter().enumerate() {
        let i = modules.len() + k;
        let _ = writeln!(t, "extern const uint8_t rl_linux_file_{i}[];");
        let _ = write!(t, "static const char *const paths_{i}[] = {{");
        for path in &d.paths {
            let _ = write!(t, " {},", c_string(path));
        }
        t.push_str(" };\n");
    }
    t.push_str("static const rl_linux_module modules[] = {\n");
    for (i, p) in modules.iter().enumerate() {
        let o = &p.object;
        let ph = o
            .program_headers
            .map_or((0, 0, 0), |h| (h.addr, h.count, h.entry_size));
        let _ = writeln!(
            t,
            "    {{ .name = {}, .role = {}, .paths = paths_{i}, .path_count = {}u, .file = rl_linux_file_{i}, .file_size = UINT64_C({:#x}), .segments = segments_{i}, .segment_count = {}u, .base = UINT64_C({:#x}), .span = UINT64_C({:#x}), .align = UINT64_C({:#x}), .entry = UINT64_C({:#x}), .phdr = UINT64_C({:#x}), .phent = {}u, .phnum = {}u }},",
            c_string(&p.name),
            role_c(p.role),
            p.paths.len(),
            p.file.len(),
            o.segments.len(),
            o.low().unwrap_or(0) & !(PAGE - 1),
            o.high().unwrap_or(0).div_ceil(PAGE) * PAGE - (o.low().unwrap_or(0) & !(PAGE - 1)),
            p.align,
            o.entry,
            ph.0,
            ph.2,
            ph.1,
        );
    }
    for (k, d) in program.data.iter().enumerate() {
        let i = modules.len() + k;
        let _ = writeln!(
            t,
            "    {{ .name = \"data\", .role = RL_LINUX_DATA, .paths = paths_{i}, .path_count = {}u, .file = rl_linux_file_{i}, .file_size = UINT64_C({:#x}) }},",
            d.paths.len(),
            d.bytes.len(),
        );
    }
    let stack = match modules.first().map_or(0, |m| m.object.stack_size) {
        0 => DEFAULT_STACK,
        n => n,
    };
    let id: Vec<String> = image_id.iter().map(|b| format!("0x{b:02x}")).collect();
    let low = modules
        .iter()
        .filter_map(|m| m.object.low())
        .min()
        .unwrap_or(0);
    let high = modules
        .iter()
        .filter_map(|m| m.object.high())
        .max()
        .unwrap_or(0);
    let _ = write!(
        t,
        "}};\nstatic const uint8_t image_id[32] = {{ {} }};\nconst rl_linux_program rl_program = {{\n    .name = {},\n    .exec_path = {},\n    .modules = modules,\n    .module_count = {}u,\n    .code_lo = UINT64_C({:#x}),\n    .code_hi = UINT64_C({:#x}),\n    .stack_size = UINT64_C({:#x}),\n    .hwcap = UINT64_C({:#x}),\n    .hwcap2 = UINT64_C({:#x}),\n    .units = static_units,\n    .unit_count = {}u,\n    .image_id = image_id,\n    .code_maps = code_maps,\n    .code_map_count = {}u,\n    .root = {},\n}};\nRL_TEXT_BOUNDS_DEFINE()\nint main(int argc, char **argv)\n{{\n{}    return rl_linux_main(&rl_program, argc, argv);\n}}\n",
        id.join(", "),
        c_string(modules.first().map_or("program", |m| m.name.as_str())),
        c_string(exec_path),
        modules.len() + program.data.len(),
        low & !(PAGE - 1),
        high.div_ceil(PAGE) * PAGE,
        stack,
        cpu.hwcap(),
        cpu.hwcap2(),
        units.len(),
        code_maps.len(),
        program
            .root
            .as_deref()
            .map_or_else(|| "NULL".to_string(), c_string),
        if trampoline {
            "    rl_arm64_set_trampoline(1);\n"
        } else {
            ""
        },
    );
    t
}

/// Per module of `placed` (in the order the loader searches them), each
/// loader-filled slot (a global offset table or procedure linkage word)
/// with the function the loader binds it to when its symbol is defined in
/// the program: the first definition in search order. A function defined
/// by an indirect-function resolver is left out (the loader binds what the
/// resolver returns, which is not known before the run).
pub fn bound_slots(placed: &[Placed]) -> Vec<BTreeMap<u64, u64>> {
    let mut defs: BTreeMap<&str, u64> = BTreeMap::new();
    for p in placed {
        for s in p.object.exports() {
            if s.kind == relift_objfile::SymbolKind::Function && p.object.is_code(s.value) {
                defs.entry(s.name.as_str()).or_insert(s.value);
            }
        }
    }
    placed
        .iter()
        .map(|p| {
            p.object
                .relocations
                .iter()
                .filter(|r| {
                    matches!(
                        r.kind,
                        relift_objfile::RelocKind::JumpSlot | relift_objfile::RelocKind::GlobalData
                    )
                })
                .filter_map(|r| {
                    let name = &p.object.relocation_symbol(r)?.name;
                    Some((r.offset, *defs.get(name.as_str())?))
                })
                .collect()
        })
        .collect()
}
