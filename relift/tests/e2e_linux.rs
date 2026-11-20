//! End to end for the Linux guest: a statically linked aarch64 ELF goes
//! through `relift import` and `relift build`, and the program runs with the
//! output and exit status the kernel would give it. The first test builds
//! its program from machine code written here; the second builds programs of
//! the guest's corpus (`guests/linux/guest/corpus`) with zig and compares
//! them with the answers recorded on a real arm64 Linux. Both skip with a
//! note when clang is not on `PATH`, and the second when zig is not, unless
//! `RELIFT_LINUX_REQUIRE` is set.

mod tiny;

use std::path::Path;

use relift_objfile::synth::{Builder, SegmentSpec};

const BASE: u64 = 0x40_0000;

/// `write(1, "hello\n", 6); exit_group(7)`.
fn program() -> Vec<u8> {
    let code: [u32; 8] = [
        0xd280_0808,
        0xd280_0020,
        0x1000_7fc1,
        0xd280_00c2,
        0xd400_0001,
        0xd280_0ba8,
        0xd280_00e0,
        0xd400_0001,
    ];
    let mut text: Vec<u8> = code.iter().flat_map(|w| w.to_le_bytes()).collect();
    text.extend_from_slice(&0xd503_201fu32.to_le_bytes());
    let size = text.len() as u64;
    let msg = b"hello\n".to_vec();
    Builder {
        entry: BASE,
        segments: vec![
            SegmentSpec {
                addr: BASE,
                data: text,
                mem_size: size,
                flags: 5,
            },
            SegmentSpec {
                addr: BASE + 0x1000,
                mem_size: msg.len() as u64,
                data: msg,
                flags: 4,
            },
        ],
        ..Builder::default()
    }
    .build()
}

fn built(root: &Path, program: &Path, cache: &str) -> std::path::PathBuf {
    let project = root.join("game");
    std::fs::create_dir_all(&project).expect("the project dir is creatable");
    let image = program.display().to_string();
    let out = tiny::relift(&project, &["--cache-dir", cache, "import", &image]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let toml = std::fs::read_to_string(project.join("game.toml")).expect("game.toml");
    assert!(toml.contains("guest = \"linux\""), "{toml}");
    let out = tiny::relift(&project, &["--cache-dir", cache, "-j", "4", "build"]);
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stderr}");
    assert!(!stderr.contains("missed"), "{stderr}");
    root.join("cache/projects/game/build/host")
        .join(format!("game{}", std::env::consts::EXE_SUFFIX))
}

#[test]
fn a_hand_assembled_program_builds_and_exits_with_its_status() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let root = tiny::scratch("e2e-linux");
    let elf = root.join("hello");
    std::fs::create_dir_all(&root).expect("the scratch dir is creatable");
    relift_cache::write_if_changed(&elf, &program()).expect("the program is writable");
    let cache = root.join("cache").display().to_string();
    let exe = built(&root, &elf, &cache);
    let out = relift_proc::command(&exe)
        .env("RELIFT_REPORT_DIR", &root)
        .output()
        .expect("the program runs");
    assert_eq!(String::from_utf8_lossy(&out.stdout), "hello\n");
    assert_eq!(out.status.code(), Some(7));
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn corpus_programs_print_what_they_printed_on_arm64_linux() {
    let have = |t: &str| relift_build::find_tool(t).is_some();
    if !have("clang") || !have("zig") {
        assert!(
            std::env::var_os("RELIFT_LINUX_REQUIRE").is_none(),
            "RELIFT_LINUX_REQUIRE is set and clang or zig is missing"
        );
        eprintln!("skipped: no clang or zig on PATH");
        return;
    }
    let corpus = Path::new(env!("CARGO_MANIFEST_DIR")).join("../guests/linux/guest/corpus");
    let root = tiny::scratch("e2e-linux-corpus");
    std::fs::create_dir_all(&root).expect("the scratch dir is creatable");
    let programs = root.join("programs");
    let status = relift_proc::command("sh")
        .arg(corpus.join("build.sh"))
        .arg(&programs)
        .env("ZIG_GLOBAL_CACHE_DIR", root.join("zig"))
        .env("ZIG_LOCAL_CACHE_DIR", root.join("zig"))
        .status()
        .expect("build.sh runs");
    assert!(status.success());
    let cache = root.join("cache").display().to_string();
    for name in [
        "hello.static",
        "setjmp.static",
        "setjmp.pie",
        "jmp_ptr.static",
        "threads.static",
        "signals.static",
        "cxx_exceptions.static",
        "fileio.static",
    ] {
        let _ = std::fs::remove_dir_all(root.join("game"));
        let _ = std::fs::remove_dir_all(root.join("cache"));
        let exe = built(&root, &programs.join(name), &cache);
        let dir = corpus.join(name.split('.').next().expect("a name"));
        let mut cmd = relift_proc::command(&exe);
        cmd.env("RELIFT_REPORT_DIR", &root).env("TMPDIR", &root);
        if let Ok(args) = std::fs::read_to_string(dir.join("args")) {
            cmd.args(args.lines());
        }
        let out = cmd.output().expect("the program runs");
        let want = std::fs::read(dir.join("expected.stdout")).expect("expected.stdout");
        assert_eq!(
            String::from_utf8_lossy(&out.stdout),
            String::from_utf8_lossy(&want),
            "{name}"
        );
        let code: i32 = std::fs::read_to_string(dir.join("expected.exit"))
            .expect("expected.exit")
            .trim()
            .parse()
            .expect("a status");
        assert_eq!(out.status.code(), Some(code), "{name}");
    }
    let _ = std::fs::remove_dir_all(&root);
}
