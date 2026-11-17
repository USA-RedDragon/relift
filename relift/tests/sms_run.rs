//! A hand-assembled Master System ROM lifted by `relift-sms` (one unit per
//! bank and slot, one for code copied to work RAM), linked with the
//! runtime's core, Z80 layer, machine and run layer, and run headless. The
//! frame interrupt's handler counts in work RAM (from 0xAB, the byte the
//! console leaves at 0xC000), calls a routine in two
//! banks it maps in turn at 0x8000, and calls the routine it copied to work
//! RAM after flipping the address operand of that routine's first
//! instruction, so RAM code lifted once runs with both operands; after 60
//! frames the counters hold the number of interrupts (the RAM routine's
//! split between its two targets), and
//! the run never reaches code with no lifted body. A second build leaves
//! out the RAM routine's unit and must stop with a stop report at it.
//! Skipped with a note when neither clang nor zig is on `PATH`.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use relift_sms::code::{Code, CodeSrc, Loc, length};
use relift_sms::program::{self, Console, Mapper, Program, Region};

/// Bank 0: the reset code, the interrupt handlers and the RAM routine's
/// source at 0x0200.
const BANK0: &[(usize, &[u8])] = &[
    (
        0x0000,
        &[
            0xF3, 0xED, 0x56, 0x31, 0xF0, 0xDF, 0x21, 0x00, 0x02, 0x11, 0x00, 0xC1, 0x01, 0x05,
            0x00, 0xED, 0xB0, 0x3E, 0x60, 0xD3, 0xBF, 0x3E, 0x81, 0xD3, 0xBF, 0xFB, 0x76, 0x18,
            0xFD,
        ],
    ),
    (
        0x0038,
        &[
            0xF5, 0xDB, 0xBF, 0x3A, 0x00, 0xC0, 0x3C, 0x32, 0x00, 0xC0, 0x3E, 0x02, 0x32, 0xFF,
            0xFF, 0xCD, 0x00, 0x80, 0x3E, 0x03, 0x32, 0xFF, 0xFF, 0xCD, 0x00, 0x80, 0x3A, 0x01,
            0xC1, 0xEE, 0x07, 0x32, 0x01, 0xC1, 0xCD, 0x00, 0xC1, 0xF1, 0xFB, 0xED, 0x4D,
        ],
    ),
    (0x0066, &[0xED, 0x45]),
    (0x0200, &[0x21, 0x03, 0xC0, 0x34, 0xC9]),
];

fn rom() -> Vec<u8> {
    let mut rom = vec![0xFFu8; 0x10000];
    for (at, bytes) in BANK0 {
        rom[*at..*at + bytes.len()].copy_from_slice(bytes);
    }
    rom[0x8000..0x8005].copy_from_slice(&[0x21, 0x01, 0xC0, 0x34, 0xC9]);
    rom[0xC000..0xC005].copy_from_slice(&[0x21, 0x02, 0xC0, 0x34, 0xC9]);
    rom
}

/// The instructions of `bytes` from `addr` on, one after another.
fn linear(code: &mut BTreeMap<Loc, u8>, bytes: &[u8], addr: u16, src: impl Fn(&[u8]) -> CodeSrc) {
    let mut off = 0;
    while off < bytes.len() {
        let mut window = bytes[off..].to_vec();
        window.resize(off.max(4) + 4, 0);
        let n = length(&window, addr + off as u16).expect("decodes");
        let insn = &bytes[off..(off + usize::from(n)).min(bytes.len())];
        code.insert(
            Loc {
                addr: addr + off as u16,
                src: src(insn),
            },
            n,
        );
        off += usize::from(n);
    }
}

fn code(with_ram: bool) -> Code {
    let mut c = BTreeMap::new();
    for (at, bytes) in &BANK0[..3] {
        linear(&mut c, bytes, *at as u16, |_| CodeSrc::Rom { bank: 0 });
    }
    let r = rom();
    linear(&mut c, &r[0x8000..0x8005], 0x8000, |_| CodeSrc::Rom {
        bank: 2,
    });
    linear(&mut c, &r[0xC000..0xC005], 0x8000, |_| CodeSrc::Rom {
        bank: 3,
    });
    if with_ram {
        linear(&mut c, BANK0[3].1, 0xC100, |b| CodeSrc::Ram {
            bytes: b.to_vec(),
        });
    }
    Code { instructions: c }
}

fn compiler() -> Option<(PathBuf, Vec<String>)> {
    if let Some(c) = relift_build::find_tool("clang") {
        return Some((c, Vec::new()));
    }
    relift_build::find_tool("zig").map(|z| (z, vec!["cc".to_string()]))
}

fn run_cc(cc: &(PathBuf, Vec<String>), args: &[String]) {
    let out = relift_proc::command(&cc.0)
        .args(&cc.1)
        .args(args)
        .output()
        .expect("the C compiler runs");
    assert!(
        out.status.success(),
        "{} {}:\n{}",
        cc.0.display(),
        args.join(" "),
        String::from_utf8_lossy(&out.stderr)
    );
}

/// Writes the program's C into `dir/c` and links `dir/prog`.
fn build(dir: &Path, rom: &[u8], lifted: &program::Lifted) -> Option<PathBuf> {
    let cc = compiler().or_else(|| {
        eprintln!("skipped: no clang or zig on PATH");
        None
    })?;
    let c = dir.join("c");
    let mut sources = Vec::new();
    for u in std::iter::once(&lifted.entries).chain(&lifted.units) {
        let d = c.join(&u.dir);
        std::fs::create_dir_all(&d).expect("creates");
        for f in &u.output.files {
            relift_cache::write_if_changed(&d.join(&f.name), f.text.as_bytes()).expect("writes");
            if f.name.ends_with(".c") {
                sources.push(d.join(&f.name));
            }
        }
    }
    let p = Program {
        title: "synthetic",
        console: Console::Ms,
        region: Region::NtscU,
        mapper: Mapper::Sega,
    };
    relift_cache::write_if_changed(
        &c.join("program.c"),
        program::program_c(&p, lifted).as_bytes(),
    )
    .expect("writes");
    relift_cache::write_if_changed(&c.join("rom.c"), program::rom_c(rom).as_bytes())
        .expect("writes");
    relift_cache::write_if_changed(&c.join("rom.bin"), rom).expect("writes");
    sources.push(c.join("program.c"));
    sources.push(c.join("rom.c"));
    let rt = Path::new(env!("CARGO_MANIFEST_DIR")).join("../runtime");
    for sub in ["core", "guests/sms", "guests/sms/run"] {
        for entry in std::fs::read_dir(rt.join(sub)).expect("lists") {
            let p = entry.expect("entry").path();
            if matches!(p.extension().and_then(|e| e.to_str()), Some("c" | "S")) {
                sources.push(p);
            }
        }
    }
    sources.push(rt.join("cpus/z80/z80.c"));
    let mut flags: Vec<String> = vec![
        "-std=gnu11".into(),
        "-O1".into(),
        "-w".into(),
        "-D_GNU_SOURCE".into(),
        "-DRL_BUILDING_RUNTIME".into(),
        format!("-I{}", dir.display()),
    ];
    for inc in [
        "core/include",
        "core",
        "cpus/z80/include",
        "cpus/z80",
        "guests/sms",
        "guests/sms/run",
        "guests/sms/run/include",
    ] {
        flags.push(format!("-I{}", rt.join(inc).display()));
    }
    if cfg!(target_arch = "x86_64") {
        flags.push("-mcx16".into());
    }
    if cc.0.ends_with("zig") {
        flags.push("-fno-sanitize=undefined".into());
    }
    let objs: Vec<PathBuf> = (0..sources.len())
        .map(|i| dir.join(format!("o{i}.o")))
        .collect();
    let next = std::sync::atomic::AtomicUsize::new(0);
    let threads = std::thread::available_parallelism().map_or(4, |n| n.get().min(16));
    std::thread::scope(|s| {
        for _ in 0..threads {
            s.spawn(|| {
                loop {
                    let i = next.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                    let Some(src) = sources.get(i) else { break };
                    let mut a = flags.clone();
                    a.extend([
                        "-c".into(),
                        src.display().to_string(),
                        "-o".into(),
                        objs[i].display().to_string(),
                    ]);
                    run_cc(&cc, &a);
                }
            });
        }
    });
    let exe = dir.join("prog");
    let mut a: Vec<String> = objs.iter().map(|o| o.display().to_string()).collect();
    a.extend([
        "-lm".into(),
        "-lpthread".into(),
        "-o".into(),
        exe.display().to_string(),
    ]);
    if !cfg!(windows) {
        a.push("-ldl".into());
    }
    run_cc(&cc, &a);
    Some(exe)
}

fn scratch(name: &str) -> PathBuf {
    let dir = Path::new(env!("CARGO_TARGET_TMPDIR")).join(format!("{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).expect("the scratch dir is creatable");
    dir
}

#[test]
fn banked_and_ram_code_runs_lifted_and_a_miss_stops_with_a_report() {
    let rom = rom();
    let lifted = program::lift(&rom, &code(true), [0; 32]);
    assert!(lifted.unlifted.is_empty(), "{:?}", lifted.unlifted);
    assert_eq!(
        lifted.units.len(),
        4,
        "bank 0, bank 2 and bank 3 in slot 2, RAM"
    );
    let dir = scratch("sms-run");
    let Some(exe) = build(&dir, &rom, &lifted) else {
        return;
    };
    let ram = dir.join("ram.bin");
    let run = relift_proc::command(&exe)
        .args(["--headless", "--frames", "60", "--dump-ram"])
        .arg(&ram)
        .env("RELIFT_REPORT_DIR", &dir)
        .output()
        .expect("runs");
    let text = String::from_utf8_lossy(&run.stdout).to_string();
    assert!(
        run.status.success(),
        "{text}\n{}",
        String::from_utf8_lossy(&run.stderr)
    );
    assert!(text.starts_with("frames 60 "), "{text}");
    let ram = std::fs::read(&ram).expect("the RAM dump");
    let irqs = ram[0].wrapping_sub(0xAB);
    assert!((58..=60).contains(&irqs), "{irqs} frame interrupts");
    assert_eq!(&ram[1..3], &[irqs, irqs], "bank 2, bank 3");
    assert_eq!(
        (ram[3], ram[4]),
        (irqs / 2, irqs - irqs / 2),
        "the RAM routine with its operand patched before each call"
    );

    let missing = program::lift(&rom, &code(false), [0; 32]);
    let dir2 = scratch("sms-run-miss");
    let Some(exe) = build(&dir2, &rom, &missing) else {
        return;
    };
    let run = relift_proc::command(&exe)
        .args(["--headless", "--frames", "60"])
        .env("RELIFT_REPORT_DIR", &dir2)
        .output()
        .expect("runs");
    let err = String::from_utf8_lossy(&run.stderr).to_string();
    assert!(!run.status.success(), "{err}");
    assert!(
        err.contains("no lifted code at guest 0xc100 (work RAM)"),
        "{err}"
    );
    let _ = std::fs::remove_dir_all(&dir);
    let _ = std::fs::remove_dir_all(&dir2);
}
